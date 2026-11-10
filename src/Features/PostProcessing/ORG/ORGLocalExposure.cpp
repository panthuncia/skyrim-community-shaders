#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/LocalExposure.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/LocalExposure/localexposure.cs.hlsl";
		constexpr DXGI_FORMAT kLuminanceFormat = DXGI_FORMAT_R16_FLOAT;  // as LocalExposure::SetupResources
		constexpr DXGI_FORMAT kGridFormat = DXGI_FORMAT_R32G32_FLOAT;

		// localexposure.cs.hlsl's push block, every entry: LOCAL_EXPOSURE_RESOURCES, in order.
		struct LocalExposureBlock
		{
			ShaderBindings::Handle texColor, texLogLuminance, texLuminanceGrid, texBlurredLuminance;
			ShaderBindings::Handle linearSampler, mirrorSampler;
			ShaderBindings::Handle rwTexOutput, rwTexLuminanceGrid;
			ShaderBindings::Latch localExposure;
		};

		std::uint32_t Groups8(std::uint32_t a_size) { return (a_size + 7) >> 3; }

		/** @brief The D3D11 dispatch's size of a pyramid level: rounded up, where the level's texture is rounded down. */
		std::uint32_t DispatchSize(std::uint32_t a_size, std::uint32_t a_mip) { return (std::max)(1u, (a_size + ((1u << a_mip) - 1u)) >> a_mip); }

		/**
		 * @brief LocalExposure::Draw on the graph: the scene's log luminance, its pyramid down to the blur level, the broad blur, and
		 * the edge-aware grid. Each pyramid level is its own texture the size of that D3D11 mip, so every pass reads and writes whole
		 * textures, as the D3D11 path's single-mip views do. All of them are transient and owned here (the chain's images are kMAIN
		 * sized). Composite reads three of them: published as "PostProcessing::LocalExposure::LogLuminance", "::LuminanceGrid" and
		 * "::BlurredLuminance".
		 */
		class LocalExposureEffect final : public Effect
		{
		public:
			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				const auto shape = a_builder.GetShape();
				if (!linearSampler || !mirrorSampler) {
					// LocalExposure's samplers: linear over every mip, clamped or mirrored.
					linearSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					mirrorSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{ .addressU = rhi::AddressMode::Mirror, .addressV = rhi::AddressMode::Mirror });
					if (!linearSampler || !mirrorSampler) {
						a_builder.Fail("its samplers could not be made");
						return;
					}
				}
				if (width != shape.width || height != shape.height || levels.empty()) {
					if (!MakeTextures(shape.width, shape.height)) {
						a_builder.Fail("its textures could not be made");
						return;
					}
				}
				for (const auto& level : levels)
					a_builder.Register(*level);
				a_builder.Register(*blurTemp);
				a_builder.Register(*blurred);
				a_builder.Register(*grid);

				const auto words = ShaderBindings::Words<LocalExposureBlock>();
				constants = a_builder.Latch<LocalExposure::LocalExposureCB>();
				const auto linear = Bind::Handle(linearSampler.Index());
				const auto mirror = Bind::Handle(mirrorSampler.Index());
				// Every entry takes the whole list; each reads only its own resources.
				auto bindings = [&](Bind a_color, Bind a_logLuminance, Bind a_output, Bind a_grid) {
					return std::vector<Bind>{ std::move(a_color), std::move(a_logLuminance), Bind::Unused(), Bind::Unused(), linear, mirror,
						std::move(a_output), std::move(a_grid), Bind::Latch(constants) };
				};
				const auto image = [](const auto& a_texture, DXGI_FORMAT a_format) { return Image{ a_texture->Graph(), a_format }; };

				a_builder.Dispatch({ .name = "setup",
					.program = a_builder.Compute(kShader, words, {}, L"CSSetupLogLuminance"),
					.bindings = bindings(Bind::Srv(a_builder.Current()), Bind::Unused(), Bind::Uav(levels[0]->Graph()), Bind::Unused()),
					.groups = { Groups8(width), Groups8(height), 1 } });
				auto downsample = a_builder.Compute(kShader, words, {}, L"CSDownsampleLogLuminance");
				for (std::uint32_t mip = 1; mip < levels.size(); ++mip) {
					a_builder.Dispatch({ .name = std::format("pyramid{}", mip),
						.program = downsample,
						.bindings = bindings(Bind::Unused(), Bind::Srv(levels[mip - 1]->Graph()), Bind::Uav(levels[mip]->Graph()), Bind::Unused()),
						.groups = { Groups8(DispatchSize(width, mip)), Groups8(DispatchSize(height, mip)), 1 } });
				}
				const auto& blurDesc = blurred->Desc();
				a_builder.Dispatch({ .name = "blur-horizontal",
					.program = a_builder.Compute(kShader, words, {}, L"CSBlurHorizontal"),
					.bindings = bindings(Bind::Unused(), Bind::Srv(levels.back()->Graph()), Bind::Uav(blurTemp->Graph()), Bind::Unused()),
					.groups = { Groups8(blurDesc.Width), Groups8(blurDesc.Height), 1 } });
				a_builder.Dispatch({ .name = "blur-vertical",
					.program = a_builder.Compute(kShader, words, {}, L"CSBlurVertical"),
					.bindings = bindings(Bind::Unused(), Bind::Srv(blurTemp->Graph()), Bind::Uav(blurred->Graph()), Bind::Unused()),
					.groups = { Groups8(blurDesc.Width), Groups8(blurDesc.Height), 1 } });
				a_builder.Dispatch({ .name = "grid",
					.program = a_builder.Compute(kShader, words, {}, L"CSBuildLuminanceGrid"),
					.bindings = bindings(Bind::Unused(), Bind::Srv(levels[0]->Graph()), Bind::Unused(), Bind::Uav(grid->Graph())),
					.groups = { grid->Desc().Width, grid->Desc().Height, 1 } });

				a_builder.Publish("PostProcessing::LocalExposure::LogLuminance", image(levels[0], kLuminanceFormat));
				a_builder.Publish("PostProcessing::LocalExposure::LuminanceGrid", image(grid, kGridFormat));
				a_builder.Publish("PostProcessing::LocalExposure::BlurredLuminance", image(blurred, kLuminanceFormat));
				a_builder.PublishLatch(Published::kLocalExposureConstants, constants);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				const auto& blurDesc = blurred->Desc();
				a_commit.Write(constants, static_cast<LocalExposure&>(a_feature).Constants(width, height, blurDesc.Width, blurDesc.Height));
			}

		private:
			/** @brief The textures LocalExposure::SetupResources makes, for a scene a_width by a_height. False when one cannot be made. */
			bool MakeTextures(UINT a_width, UINT a_height)
			{
				levels.clear();
				blurTemp.reset();
				blurred.reset();
				grid.reset();
				width = a_width;
				height = a_height;

				const auto texture2D = [](UINT a_w, UINT a_h, std::string a_name) {
					D3D11_TEXTURE2D_DESC desc{};
					desc.Width = (std::max)(1u, a_w);
					desc.Height = (std::max)(1u, a_h);
					desc.MipLevels = 1;
					desc.ArraySize = 1;
					desc.Format = kLuminanceFormat;
					desc.SampleDesc = { 1, 0 };
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					return std::make_unique<GraphResources::Texture2D>(desc, std::move(a_name));
				};

				// The pyramid's levels 0 to the blur level: the D3D11 mips' sizes (each halved, rounded down).
				const UINT blurMip = LocalExposure::BlurMip(LocalExposure::MipCount(a_width, a_height));
				for (UINT mip = 0; mip <= blurMip; ++mip) {
					levels.push_back(texture2D(a_width >> mip, a_height >> mip, std::format("PostProcessing::LocalExposure::LogLuminance{}", mip)));
					if (!*levels.back())
						return Discard();
				}
				blurTemp = texture2D(a_width >> blurMip, a_height >> blurMip, "PostProcessing::LocalExposure::BlurTemp");
				blurred = texture2D(a_width >> blurMip, a_height >> blurMip, "PostProcessing::LocalExposure::BlurredLuminance");

				D3D11_TEXTURE3D_DESC gridDesc{};
				gridDesc.Width = (a_width + LocalExposure::s_GridTileSize - 1) / LocalExposure::s_GridTileSize;
				gridDesc.Height = (a_height + LocalExposure::s_GridTileSize - 1) / LocalExposure::s_GridTileSize;
				gridDesc.Depth = LocalExposure::s_GridDepth;
				gridDesc.MipLevels = 1;
				gridDesc.Format = kGridFormat;
				gridDesc.Usage = D3D11_USAGE_DEFAULT;
				gridDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				grid = std::make_unique<GraphResources::Texture3D>(gridDesc, "PostProcessing::LocalExposure::LuminanceGrid");
				if (!*blurTemp || !*blurred || !*grid)
					return Discard();
				return true;
			}

			bool Discard()
			{
				levels.clear();
				blurTemp.reset();
				blurred.reset();
				grid.reset();
				width = height = 0;
				return false;
			}

			std::vector<std::unique_ptr<GraphResources::Texture2D>> levels;  // the log-luminance pyramid, level 0 at the scene's size
			std::unique_ptr<GraphResources::Texture2D> blurTemp;
			std::unique_ptr<GraphResources::Texture2D> blurred;
			std::unique_ptr<GraphResources::Texture3D> grid;
			org::OwnedDescriptorBinding linearSampler, mirrorSampler;
			LatchRegion<LocalExposure::LocalExposureCB> constants;
			UINT width = 0, height = 0;
		};
	}

	std::unique_ptr<Effect> MakeLocalExposure() { return std::make_unique<LocalExposureEffect>(); }
}

#endif
