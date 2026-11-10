#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/MotionBlur.h"
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

		// The passes' push blocks: their shaders' resource lists, in order.
		struct HorizontalBlock  // motionblur_horizontalpass.cs.hlsl
		{
			ShaderBindings::Handle texVelocity, rwTexHorizontal;
			ShaderBindings::Latch reduction;
		};

		struct GridBlock  // motionblur_verticalpass.cs.hlsl, motionblur_neighborpass.cs.hlsl
		{
			ShaderBindings::Handle input, output;
		};

		struct BlurBlock  // motionblur_blurpass.cs.hlsl
		{
			ShaderBindings::Handle texColor, texVelocity, texNeighborMax, texDepth, rwTexOut, linearSampler, pointSampler;
			ShaderBindings::Latch blur;
		};

		D3D11_TEXTURE2D_DESC GridDesc(UINT a_height)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kGrid;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = kGridFormat;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			return desc;
		}

		/**
		 * @brief MotionBlur::Draw on the graph: the horizontal and vertical velocity reductions into a 20 x 20 grid, its neighbour
		 * maximum, then the blur over the dynamic-resolution area into an image of the input's format. The grid textures belong to
		 * the effect: the horizontal one keeps its rows across frames, as on D3D11 (the reductions read its whole height, the first
		 * pass writes the dynamic rows), the two 20 x 20 ones are written whole each frame.
		 */
		class MotionBlurEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature&, std::vector<std::uint64_t>& a_key) override
			{
				// What MotionBlur::Draw binds: kMAIN's depth and the motion vectors.
				depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
				motion = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMOTION_VECTOR));
				if (!depth || !motion)
					return "the main depth or the motion vectors cannot be imported";
				a_key.push_back(reinterpret_cast<std::uintptr_t>(depth.get()));
				a_key.push_back(reinterpret_cast<std::uintptr_t>(motion.get()));
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				const auto& shape = a_builder.GetShape();
				if (!Prepare(a_builder, shape.height))
					return;
				a_builder.Register(*horizontal);
				a_builder.Register(*vertical);
				a_builder.Register(*neighborMax);

				// As MotionBlur::CheckAndResizeResources: the output takes the input's format.
				const auto input = a_builder.Current();
				auto image = a_builder.CreateImage("PostProcessing::MotionBlur::Output", input.format, D3D11_BIND_UNORDERED_ACCESS);
				reduction = a_builder.Latch<MotionBlur::ReductionPassConstantBuffer>();
				blur = a_builder.Latch<MotionBlur::MotionBlurConstantBuffer>();
				// As MotionBlur::Draw: the dispatches cover the dynamic size, truncated.
				const auto dynamicWidth = static_cast<std::uint32_t>(shape.dynamicWidth);
				const auto dynamicHeight = static_cast<std::uint32_t>(shape.dynamicHeight);

				a_builder.Dispatch({ .name = "horizontal",
					.program = a_builder.Compute("PostProcessing/MotionBlur/motionblur_horizontalpass.cs.hlsl", ShaderBindings::Words<HorizontalBlock>()),
					.bindings = { Bind::Srv(motion), Bind::Uav(horizontal->Graph()), Bind::Latch(reduction) },
					.groups = { kGridGroups, (dynamicHeight + 7) >> 3, 1 } });
				a_builder.Dispatch({ .name = "vertical",
					.program = a_builder.Compute("PostProcessing/MotionBlur/motionblur_verticalpass.cs.hlsl", ShaderBindings::Words<GridBlock>()),
					.bindings = { Bind::Srv(horizontal->Graph()), Bind::Uav(vertical->Graph()) },
					.groups = { kGridGroups, kGridGroups, 1 } });
				a_builder.Dispatch({ .name = "neighbor-max",
					.program = a_builder.Compute("PostProcessing/MotionBlur/motionblur_neighborpass.cs.hlsl", ShaderBindings::Words<GridBlock>()),
					.bindings = { Bind::Srv(vertical->Graph()), Bind::Uav(neighborMax->Graph()) },
					.groups = { kGridGroups, kGridGroups, 1 } });
				a_builder.Dispatch({ .name = "blur",
					.program = a_builder.Compute("PostProcessing/MotionBlur/motionblur_blurpass.cs.hlsl", ShaderBindings::Words<BlurBlock>()),
					.bindings = { Bind::Srv(input), Bind::Srv(motion), Bind::Srv(neighborMax->Graph()), Bind::Srv(depth), Bind::Uav(image),
						Bind::Handle(linearSampler.Index()), Bind::Handle(pointSampler.Index()), Bind::Latch(blur) },
					.groups = { (dynamicWidth + 7) >> 3, (dynamicHeight + 7) >> 3, 1 } });
				a_builder.SetCurrent(image);
				size = float2(static_cast<float>(shape.width), static_cast<float>(shape.height));
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				// As MotionBlur::UpdateConstantBuffers: the target resolution is the full texture size.
				const auto constants = static_cast<MotionBlur&>(a_feature).Constants(size);
				a_commit.Write(reduction, constants.reduction);
				a_commit.Write(blur, constants.blur);
			}

		private:
			/** @brief The samplers and the grid textures, the horizontal one a_height rows tall. False, with the build failed, when one cannot be made. */
			bool Prepare(Builder& a_builder, UINT a_height)
			{
				if (!linearSampler || !pointSampler) {
					// MotionBlur::SetupResources' samplers: linear and point, clamped, every mip.
					linearSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					pointSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .minFilter = rhi::Filter::Nearest,
						.magFilter = rhi::Filter::Nearest,
						.mipFilter = rhi::MipFilter::Nearest });
					if (!linearSampler || !pointSampler) {
						a_builder.Fail("its samplers could not be made");
						return false;
					}
				}
				if (!horizontal || !*horizontal || horizontal->Desc().Height != a_height) {
					// Its rows below the dynamic height carry over from earlier frames, as the D3D11 texture's do: never aliased.
					horizontal = std::make_unique<GraphResources::Texture2D>(GridDesc(a_height), "PostProcessing::MotionBlur::Horizontal",
						GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
				}
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

			std::shared_ptr<org::Resource> depth, motion;
			org::OwnedDescriptorBinding linearSampler, pointSampler;
			std::unique_ptr<GraphResources::Texture2D> horizontal, vertical, neighborMax;
			LatchRegion<MotionBlur::ReductionPassConstantBuffer> reduction;
			LatchRegion<MotionBlur::MotionBlurConstantBuffer> blur;
			float2 size;
		};
	}

	std::unique_ptr<Effect> MakeMotionBlur() { return std::make_unique<MotionBlurEffect>(); }
}

#endif
