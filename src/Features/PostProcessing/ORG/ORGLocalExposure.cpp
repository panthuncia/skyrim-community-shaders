// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PostProcessing/ORGPostEffect.h"

#include "Features/PostProcessing/LocalExposure.h"
#include "Globals.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"

#include <algorithm>
#include <format>
#include <optional>

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
			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies&) override
			{
				using namespace RenderGraph::Frame;
				const auto dependencies = DependsOn(Vars::DisplaySize());
				// One texel while the effect is off (the existence rule).
				const auto exists = [&a_feature] { return ExistsWhen(a_feature.enabled); };
				const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::Persistent };
				// The chain's size is kMAIN's, which is the display size.
				const auto sizesOf = [](const Inputs& a_in) {
					const auto display = a_in.Get(Vars::DisplaySize());
					return Sizes(display.width, display.height);
				};
				levels.clear();
				for (UINT mip = 0; mip < kLevels; ++mip) {
					levels.push_back(DeclareTexture2D(std::format("PostProcessing::LocalExposure::LogLuminance{}", mip), dependencies,
						[sizesOf, mip](const Inputs& a_in) {
							const auto sizes = sizesOf(a_in);
							return LuminanceDesc((std::max)(sizes.width >> mip, 1u), (std::max)(sizes.height >> mip, 1u));
						},
						options, exists()));
				}
				const auto blurDesc = [sizesOf](const Inputs& a_in) {
					const auto sizes = sizesOf(a_in);
					return LuminanceDesc(sizes.blurWidth, sizes.blurHeight);
				};
				blurTemp = DeclareTexture2D("PostProcessing::LocalExposure::BlurTemp", dependencies, blurDesc, options, exists());
				blurred = DeclareTexture2D("PostProcessing::LocalExposure::BlurredLuminance", dependencies, blurDesc, options, exists());
				grid = DeclareTexture3D("PostProcessing::LocalExposure::LuminanceGrid", dependencies,
					[sizesOf](const Inputs& a_in) {
						const auto sizes = sizesOf(a_in);
						return GridDesc(sizes.gridWidth, sizes.gridHeight, LocalExposure::s_GridDepth);
					},
					options, exists());
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				LoadPrograms(a_programs);
				a_programs.Publishes(Published::kLogLuminance);
				a_programs.Publishes(Published::kLuminanceGrid);
				a_programs.Publishes(Published::kBlurredLuminance);
				a_programs.Publishes(Published::kLocalExposureConstants);
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				const auto shape = a_builder.GetShape();
				if (!linearSampler || !mirrorSampler) {
					// Linear over every mip, clamped or mirrored.
					linearSampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp);
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

				auto programs = LoadPrograms(a_builder);
				const auto constants = a_builder.Latch<LocalExposure::LocalExposureCB>();
				const auto linear = Bind::Handle(linearSampler.Index());
				const auto mirror = Bind::Handle(mirrorSampler.Index());
				// Every entry takes the whole list, and each reads only its own.
				auto bindings = [&](Bind a_color, Bind a_logLuminance, Bind a_output, Bind a_grid) {
					return std::vector<Bind>{ Bind::Shared(), std::move(a_color), std::move(a_logLuminance), Bind::Unused(), Bind::Unused(), linear, mirror,
						std::move(a_output), std::move(a_grid), Bind::Latch(constants) };
				};

				a_builder.Dispatch({ .name = "setup",
					.program = std::move(programs.setup),
					.bindings = bindings(Bind::Srv(a_builder.Current()), Bind::Unused(), Bind::Uav(*levels[0]), Bind::Unused()),
					.groups = { Groups8(sizes.width), Groups8(sizes.height), 1 } });
				for (std::uint32_t mip = 1; mip <= sizes.blurMip; ++mip) {
					a_builder.Dispatch({ .name = std::format("pyramid{}", mip),
						.program = programs.downsample,
						.bindings = bindings(Bind::Unused(), Bind::Srv(*levels[mip - 1]), Bind::Uav(*levels[mip]), Bind::Unused()),
						.groups = { Groups8(DispatchSize(sizes.width, mip)), Groups8(DispatchSize(sizes.height, mip)), 1 } });
				}
				a_builder.Dispatch({ .name = "blur-horizontal",
					.program = std::move(programs.blurHorizontal),
					.bindings = bindings(Bind::Unused(), Bind::Srv(*levels[sizes.blurMip]), Bind::Uav(*blurTemp), Bind::Unused()),
					.groups = { Groups8(sizes.blurWidth), Groups8(sizes.blurHeight), 1 } });
				a_builder.Dispatch({ .name = "blur-vertical",
					.program = std::move(programs.blurVertical),
					.bindings = bindings(Bind::Unused(), Bind::Srv(*blurTemp), Bind::Uav(*blurred), Bind::Unused()),
					.groups = { Groups8(sizes.blurWidth), Groups8(sizes.blurHeight), 1 } });
				a_builder.Dispatch({ .name = "grid",
					.program = std::move(programs.grid),
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
			struct ExposurePrograms
			{
				std::shared_ptr<const ComputeProgram> setup, downsample, blurHorizontal, blurVertical, grid;
			};

			/** @brief Listed in Programs and built on the programs lane. The downsample is listed even when no level uses it, as the build asks for it. */
			static ExposurePrograms LoadPrograms(ProgramSource& a_programs)
			{
				const auto words = ShaderBindings::Words<LocalExposureBlock>();
				return { .setup = a_programs.Compute(kShader, words, {}, L"CSSetupLogLuminance"),
					.downsample = a_programs.Compute(kShader, words, {}, L"CSDownsampleLogLuminance"),
					.blurHorizontal = a_programs.Compute(kShader, words, {}, L"CSBlurHorizontal"),
					.blurVertical = a_programs.Compute(kShader, words, {}, L"CSBlurVertical"),
					.grid = a_programs.Compute(kShader, words, {}, L"CSBuildLuminanceGrid") };
			}

			std::vector<RenderGraph::Frame::DeclaredTexture2D> levels;  // level 0 at the scene's size
			RenderGraph::Frame::DeclaredTexture2D blurTemp;
			RenderGraph::Frame::DeclaredTexture2D blurred;
			RenderGraph::Frame::DeclaredTexture3D grid;
			org::OwnedDescriptorBinding linearSampler, mirrorSampler;
		};
	}

	std::unique_ptr<Effect> MakeLocalExposure() { return std::make_unique<LocalExposureEffect>(); }
}

