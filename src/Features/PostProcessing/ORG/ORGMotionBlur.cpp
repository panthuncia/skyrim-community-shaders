#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/MotionBlur.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <Resources/ExternalTextureResource.h>

namespace ORGPostChain
{
	namespace
	{
		constexpr DXGI_FORMAT kGridFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // MotionBlur::CheckAndResizeResources' grid textures
		constexpr UINT kGrid = MotionBlur::FixedGridSize;
		constexpr std::uint32_t kGridGroups = (kGrid + 7) >> 3;

		// Each shader's resource list in order.
		struct HorizontalBlock
		{
			ShaderBindings::Handle texVelocity, rwTexHorizontal;
			ShaderBindings::Latch reduction;
		};

		struct GridBlock
		{
			ShaderBindings::Handle input, output;
		};

		struct BlurBlock
		{
			ShaderBindings::Handle texColor, texVelocity, texNeighborMax, texDepth, rwTexOut, linearSampler, pointSampler;
			ShaderBindings::Latch blur;
		};

		D3D11_TEXTURE2D_DESC GridDesc(UINT a_height, UINT a_width = kGrid)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = kGridFormat;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			return desc;
		}

		/** @brief The pipeline's instance now, since SyncPipelineResources may replace it. */
		MotionBlur& Live() { return *globals::features::postProcessing.GetPipelineFeature<MotionBlur>(PostProcessing::FeaturePipelineIndex::MotionBlur); }

		/** @brief One build's latch regions and the size its constants take. */
		class MotionBlurRun final : public Run
		{
		public:
			MotionBlurRun(LatchRegion<MotionBlur::ReductionPassConstantBuffer> a_reduction, LatchRegion<MotionBlur::MotionBlurConstantBuffer> a_blur, float2 a_size) :
				reduction(a_reduction), blur(a_blur), size(a_size) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				// The target resolution is the full texture size, as UpdateConstantBuffers.
				const auto constants = static_cast<MotionBlur&>(a_feature).Constants(size);
				a_commit.Write(reduction, constants.reduction);
				a_commit.Write(blur, constants.blur);
			}

		private:
			LatchRegion<MotionBlur::ReductionPassConstantBuffer> reduction;
			LatchRegion<MotionBlur::MotionBlurConstantBuffer> blur;
			float2 size;
		};

		/**
		* @brief MotionBlur::Draw on the graph: velocity reductions into a 20 x 20 grid, its neighbour max, then the blur.
		* The horizontal grid is declared on the display size. It is History: once the dispatches follow the dynamic size again
		* (frame revision plan, step 8), the rows below it carry over across frames as on D3D11.
		*/
		class MotionBlurEffect final : public Effect
		{
		public:
			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies&) override
			{
				using namespace RenderGraph::Frame;
				// One texel while the effect is off, so it holds no memory then.
				horizontal = DeclareTexture2D("PostProcessing::MotionBlur::Horizontal", DependsOn(Vars::DisplaySize(), a_feature.enabled),
					[](const Inputs& a_in) {
						if (!a_in.Get(Live().enabled))
							return GridDesc(1, 1);
						return GridDesc((std::max)(a_in.Get(Vars::DisplaySize()).height, 1u));
					},
					GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
			}

			const char* Update(PostProcessFeature&, std::vector<std::uint64_t>& a_observed) override
			{
				// As MotionBlur::Draw binds them.
				depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
				motion = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMOTION_VECTOR));
				if (!depth || !motion)
					return "the main depth or the motion vectors cannot be imported";
				a_observed.push_back(reinterpret_cast<std::uintptr_t>(depth.Key()));
				a_observed.push_back(reinterpret_cast<std::uintptr_t>(motion.Key()));
				return nullptr;
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override { LoadPrograms(a_programs); }

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				const auto& shape = a_builder.GetShape();
				if (!Prepare(a_builder))
					return nullptr;
				a_builder.Register(*horizontal);
				a_builder.Register(*vertical);
				a_builder.Register(*neighborMax);

				// The output takes the input's format.
				const auto input = a_builder.Current();
				auto image = a_builder.CreateImage("PostProcessing::MotionBlur::Output", input.format, D3D11_BIND_UNORDERED_ACCESS);
				const auto reduction = a_builder.Latch<MotionBlur::ReductionPassConstantBuffer>();
				const auto blur = a_builder.Latch<MotionBlur::MotionBlurConstantBuffer>();
				// The dispatches cover kMAIN. The blur skips pixels past its input, and the reduction rows past the full target
				// size in its constants.
				const std::uint32_t width = shape.width;
				const std::uint32_t height = shape.height;
				auto programs = LoadPrograms(a_builder);

				a_builder.Dispatch({ .name = "horizontal",
					.program = std::move(programs.horizontal),
					.bindings = { Bind::Srv(motion), Bind::Uav(*horizontal), Bind::Latch(reduction) },
					.groups = { kGridGroups, (height + 7) >> 3, 1 } });
				a_builder.Dispatch({ .name = "vertical",
					.program = std::move(programs.vertical),
					.bindings = { Bind::Srv(*horizontal), Bind::Uav(*vertical) },
					.groups = { kGridGroups, kGridGroups, 1 } });
				a_builder.Dispatch({ .name = "neighbor-max",
					.program = std::move(programs.neighborMax),
					.bindings = { Bind::Srv(*vertical), Bind::Uav(*neighborMax) },
					.groups = { kGridGroups, kGridGroups, 1 } });
				a_builder.Dispatch({ .name = "blur",
					.program = std::move(programs.blur),
					.bindings = { Bind::Srv(input), Bind::Srv(motion), Bind::Srv(*neighborMax), Bind::Srv(depth), Bind::Uav(image),
						Bind::Handle(linearSampler.Index()), Bind::Handle(pointSampler.Index()), Bind::Latch(blur) },
					.groups = { (width + 7) >> 3, (height + 7) >> 3, 1 } });
				a_builder.SetCurrent(image);
				if (a_builder.Failed())
					return nullptr;
				return std::make_shared<MotionBlurRun>(reduction, blur, float2(static_cast<float>(width), static_cast<float>(height)));
			}

		private:
			struct BlurPrograms
			{
				std::shared_ptr<const ComputeProgram> horizontal, vertical, neighborMax, blur;
			};

			/** @brief Listed in Programs and built on the programs lane. */
			static BlurPrograms LoadPrograms(ProgramSource& a_programs)
			{
				return { .horizontal = a_programs.Compute("PostProcessing/MotionBlur/motionblur_horizontalpass.cs.hlsl", ShaderBindings::Words<HorizontalBlock>()),
					.vertical = a_programs.Compute("PostProcessing/MotionBlur/motionblur_verticalpass.cs.hlsl", ShaderBindings::Words<GridBlock>()),
					.neighborMax = a_programs.Compute("PostProcessing/MotionBlur/motionblur_neighborpass.cs.hlsl", ShaderBindings::Words<GridBlock>()),
					.blur = a_programs.Compute("PostProcessing/MotionBlur/motionblur_blurpass.cs.hlsl", ShaderBindings::Words<BlurBlock>()) };
			}

			/** @brief False, with the build failed, when one cannot be made. */
			bool Prepare(Builder& a_builder)
			{
				if (!linearSampler || !pointSampler) {
					// Linear and point, clamped, every mip.
					linearSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					pointSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .minFilter = rhi::Filter::Nearest,
						.magFilter = rhi::Filter::Nearest,
						.mipFilter = rhi::MipFilter::Nearest });
					if (!linearSampler || !pointSampler) {
						a_builder.Fail("its samplers could not be made");
						return false;
					}
				}
				if (!horizontal) {
					a_builder.Fail("its horizontal grid was not declared");
					return false;
				}
				// The fixed grids never change size, so they are not declared.
				if (!vertical || !*vertical)
					vertical = std::make_unique<GraphResources::Texture2D>(GridDesc(kGrid), "PostProcessing::MotionBlur::Vertical");
				if (!neighborMax || !*neighborMax)
					neighborMax = std::make_unique<GraphResources::Texture2D>(GridDesc(kGrid), "PostProcessing::MotionBlur::NeighborMax");
				if (!*horizontal || !*vertical || !*neighborMax) {
					a_builder.Fail("its grid textures could not be created");
					return false;
				}
				return true;
			}

			// This frame's imports. The build binds them, and their identities are observed.
			FrameAnchors::Texture depth, motion;
			org::OwnedDescriptorBinding linearSampler, pointSampler;
			RenderGraph::Frame::DeclaredTexture2D horizontal;
			std::unique_ptr<GraphResources::Texture2D> vertical, neighborMax;
		};
	}

	std::unique_ptr<Effect> MakeMotionBlur() { return std::make_unique<MotionBlurEffect>(); }
}

#endif
