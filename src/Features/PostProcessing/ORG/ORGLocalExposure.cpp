#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/LocalExposure.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"

#	include <algorithm>
#	include <format>
#	include <optional>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/LocalExposure/localexposure.cs.hlsl";
		constexpr DXGI_FORMAT kLuminanceFormat = DXGI_FORMAT_R16_FLOAT;
		constexpr DXGI_FORMAT kGridFormat = DXGI_FORMAT_R32G32_FLOAT;

		// LOCAL_EXPOSURE_RESOURCES order, every entry.
		struct LocalExposureBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle texColor, texLogLuminance, texLuminanceGrid, texBlurredLuminance;
			ShaderBindings::Handle linearSampler, mirrorSampler;
			ShaderBindings::Handle rwTexOutput, rwTexLuminanceGrid;
			ShaderBindings::Latch localExposure;
		};

		std::uint32_t Groups8(std::uint32_t a_size) { return (a_size + 7) >> 3; }

		/** @brief Rounded up, where the level's texture is rounded down. */
		std::uint32_t DispatchSize(std::uint32_t a_size, std::uint32_t a_mip) { return (std::max)(1u, (a_size + ((1u << a_mip) - 1u)) >> a_mip); }

		/** @brief The pipeline's instance now, since SyncPipelineResources may replace it. */
		LocalExposure& Live()
		{
			return *globals::features::postProcessing.GetPipelineFeature<LocalExposure>(PostProcessing::FeaturePipelineIndex::LocalExposure);
		}

		/** @brief Every level the pyramid can use. A small image uses fewer, and the rest go unused. */
		constexpr UINT kLevels = LocalExposure::s_BlurMip + 1;

		/** @brief The sizes one build was made for: kMAIN's, and the blurred level's. */
		struct Sizes
		{
			UINT width = 1, height = 1;
			UINT blurMip = 0;
			UINT blurWidth = 1, blurHeight = 1;
			UINT gridWidth = 1, gridHeight = 1;

			explicit Sizes(UINT a_width, UINT a_height) :
				width((std::max)(1u, a_width)), height((std::max)(1u, a_height))
			{
				// Each level halves, rounded down, as the D3D11 mips.
				blurMip = LocalExposure::BlurMip(LocalExposure::MipCount(width, height));
				blurWidth = (std::max)(1u, width >> blurMip);
				blurHeight = (std::max)(1u, height >> blurMip);
				gridWidth = (width + LocalExposure::s_GridTileSize - 1) / LocalExposure::s_GridTileSize;
				gridHeight = (height + LocalExposure::s_GridTileSize - 1) / LocalExposure::s_GridTileSize;
			}
		};

		D3D11_TEXTURE2D_DESC LuminanceDesc(UINT a_width, UINT a_height)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = (std::max)(1u, a_width);
			desc.Height = (std::max)(1u, a_height);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = kLuminanceFormat;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			return desc;
		}

		D3D11_TEXTURE3D_DESC GridDesc(UINT a_width, UINT a_height, UINT a_depth)
		{
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = (std::max)(1u, a_width);
			desc.Height = (std::max)(1u, a_height);
			desc.Depth = (std::max)(1u, a_depth);
			desc.MipLevels = 1;
			desc.Format = kGridFormat;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			return desc;
		}

		/** @brief One build's latch region and the sizes its constants take. */
		class LocalExposureRun final : public Run
		{
		public:
			LocalExposureRun(LatchRegion<LocalExposure::LocalExposureCB> a_constants, const Sizes& a_sizes) :
				constants(a_constants), sizes(a_sizes) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				a_commit.Write(constants, static_cast<LocalExposure&>(a_feature).Constants(sizes.width, sizes.height, sizes.blurWidth, sizes.blurHeight));
			}

		private:
			LatchRegion<LocalExposure::LocalExposureCB> constants;
			Sizes sizes;
		};

		/**
		* @brief LocalExposure::Draw on the graph. Each pyramid level is its own texture the size of that D3D11 mip, so every pass reads
		* and writes whole textures. All are transient, declared on the display size, and one texel while the effect is off.
		* Composite reads three of them by name.
		*/
		class LocalExposureEffect final : public Effect
		{
		public:
			bool Legacy() const override { return false; }

			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies&) override
			{
				using namespace RenderGraph::Frame;
				const auto dependencies = DependsOn(Vars::DisplaySize(), a_feature.enabled);
				// The chain's size is kMAIN's, which is the display size.
				const auto sizesOf = [](const Inputs& a_in) -> std::optional<Sizes> {
					if (!a_in.Get(Live().enabled))
						return std::nullopt;
					const auto display = a_in.Get(Vars::DisplaySize());
					return Sizes(display.width, display.height);
				};
				levels.clear();
				for (UINT mip = 0; mip < kLevels; ++mip) {
					levels.push_back(DeclareTexture2D(std::format("PostProcessing::LocalExposure::LogLuminance{}", mip), dependencies,
						[sizesOf, mip](const Inputs& a_in) {
							const auto sizes = sizesOf(a_in);
							return sizes ? LuminanceDesc(sizes->width >> mip, sizes->height >> mip) : LuminanceDesc(1, 1);
						},
						GraphResources::Options{}));
				}
				const auto blurDesc = [sizesOf](const Inputs& a_in) {
					const auto sizes = sizesOf(a_in);
					return sizes ? LuminanceDesc(sizes->blurWidth, sizes->blurHeight) : LuminanceDesc(1, 1);
				};
				blurTemp = DeclareTexture2D("PostProcessing::LocalExposure::BlurTemp", dependencies, blurDesc, GraphResources::Options{});
				blurred = DeclareTexture2D("PostProcessing::LocalExposure::BlurredLuminance", dependencies, blurDesc, GraphResources::Options{});
				grid = DeclareTexture3D("PostProcessing::LocalExposure::LuminanceGrid", dependencies,
					[sizesOf](const Inputs& a_in) {
						const auto sizes = sizesOf(a_in);
						return sizes ? GridDesc(sizes->gridWidth, sizes->gridHeight, LocalExposure::s_GridDepth) : GridDesc(1, 1, 1);
					},
					GraphResources::Options{});
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				const auto shape = a_builder.GetShape();
				if (!linearSampler || !mirrorSampler) {
					// Linear over every mip, clamped or mirrored.
					linearSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					mirrorSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .addressU = rhi::AddressMode::Mirror, .addressV = rhi::AddressMode::Mirror });
					if (!linearSampler || !mirrorSampler) {
						a_builder.Fail("its samplers could not be made");
						return nullptr;
					}
				}
				const bool declared = levels.size() == kLevels && std::ranges::all_of(levels, [](const auto& a_level) { return a_level && *a_level; }) && blurTemp &&
				                      *blurTemp && blurred && *blurred && grid && *grid;
				if (!declared) {
					a_builder.Fail("its textures could not be made");
					return nullptr;
				}
				// The sizes come from the build's shape, which the declared extents follow at the same revision.
				const Sizes sizes(shape.width, shape.height);
				for (UINT mip = 0; mip <= sizes.blurMip; ++mip)
					a_builder.Register(*levels[mip]);
				a_builder.Register(*blurTemp);
				a_builder.Register(*blurred);
				a_builder.Register(*grid);

				const auto words = ShaderBindings::Words<LocalExposureBlock>();
				const auto constants = a_builder.Latch<LocalExposure::LocalExposureCB>();
				const auto linear = Bind::Handle(linearSampler.Index());
				const auto mirror = Bind::Handle(mirrorSampler.Index());
				// Every entry takes the whole list, and each reads only its own.
				auto bindings = [&](Bind a_color, Bind a_logLuminance, Bind a_output, Bind a_grid) {
					return std::vector<Bind>{ Bind::Shared(), std::move(a_color), std::move(a_logLuminance), Bind::Unused(), Bind::Unused(), linear, mirror,
						std::move(a_output), std::move(a_grid), Bind::Latch(constants) };
				};

				a_builder.Dispatch({ .name = "setup",
					.program = a_builder.Compute(kShader, words, {}, L"CSSetupLogLuminance"),
					.bindings = bindings(Bind::Srv(a_builder.Current()), Bind::Unused(), Bind::Uav(*levels[0]), Bind::Unused()),
					.groups = { Groups8(sizes.width), Groups8(sizes.height), 1 } });
				auto downsample = a_builder.Compute(kShader, words, {}, L"CSDownsampleLogLuminance");
				for (std::uint32_t mip = 1; mip <= sizes.blurMip; ++mip) {
					a_builder.Dispatch({ .name = std::format("pyramid{}", mip),
						.program = downsample,
						.bindings = bindings(Bind::Unused(), Bind::Srv(*levels[mip - 1]), Bind::Uav(*levels[mip]), Bind::Unused()),
						.groups = { Groups8(DispatchSize(sizes.width, mip)), Groups8(DispatchSize(sizes.height, mip)), 1 } });
				}
				a_builder.Dispatch({ .name = "blur-horizontal",
					.program = a_builder.Compute(kShader, words, {}, L"CSBlurHorizontal"),
					.bindings = bindings(Bind::Unused(), Bind::Srv(*levels[sizes.blurMip]), Bind::Uav(*blurTemp), Bind::Unused()),
					.groups = { Groups8(sizes.blurWidth), Groups8(sizes.blurHeight), 1 } });
				a_builder.Dispatch({ .name = "blur-vertical",
					.program = a_builder.Compute(kShader, words, {}, L"CSBlurVertical"),
					.bindings = bindings(Bind::Unused(), Bind::Srv(*blurTemp), Bind::Uav(*blurred), Bind::Unused()),
					.groups = { Groups8(sizes.blurWidth), Groups8(sizes.blurHeight), 1 } });
				a_builder.Dispatch({ .name = "grid",
					.program = a_builder.Compute(kShader, words, {}, L"CSBuildLuminanceGrid"),
					.bindings = bindings(Bind::Unused(), Bind::Srv(*levels[0]), Bind::Unused(), Bind::Uav(*grid)),
					.groups = { sizes.gridWidth, sizes.gridHeight, 1 } });

				a_builder.Publish(Published::kLogLuminance, ImageOf(*levels[0], kLuminanceFormat));
				a_builder.Publish(Published::kLuminanceGrid, ImageOf(*grid, kGridFormat));
				a_builder.Publish(Published::kBlurredLuminance, ImageOf(*blurred, kLuminanceFormat));
				a_builder.PublishLatch(Published::kLocalExposureConstants, constants);
				if (a_builder.Failed())
					return nullptr;
				return std::make_shared<LocalExposureRun>(constants, sizes);
			}

		private:
			std::vector<RenderGraph::Frame::DeclaredTexture2D> levels;  // level 0 at the scene's size
			RenderGraph::Frame::DeclaredTexture2D blurTemp;
			RenderGraph::Frame::DeclaredTexture2D blurred;
			RenderGraph::Frame::DeclaredTexture3D grid;
			org::OwnedDescriptorBinding linearSampler, mirrorSampler;
		};
	}

	std::unique_ptr<Effect> MakeLocalExposure() { return std::make_unique<LocalExposureEffect>(); }
}

#endif
