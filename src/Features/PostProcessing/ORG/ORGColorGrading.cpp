#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/ColorGrading.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <cstring>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/ColorGrading/colorgrading.hlsl";
		constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // texColor's and texLUT's, as ColorGrading::SetupResources
		constexpr std::uint32_t kLutGroups = ColorGrading::LUTDim >> 3;

		// colorgrading.hlsl's push block, both entries: COLOR_GRADING_RESOURCES, then the ColorGradingFrame latch.
		struct ColorBlock
		{
			ShaderBindings::Handle texColor, texLUT, rwLUT, linearSampler;
			ShaderBindings::Latch colorGradingFrame;
		};

		/**
		 * @brief ColorGrading::Draw on the graph: the LUT bake (CSLUTGen) whenever the constants changed, then the grading draw over
		 * the whole target. The bake runs through its own gate entry, which Commit opens on the frames it bakes, so the recording
		 * stays the same. The LUT is persistent and owned here. A bake counts as done once a frame that asked for it reached the GPU
		 * with this build (Executed); until then it is asked for again each frame. The curve preview and the image export read back
		 * through D3D11, so the chain stays on D3D11 while either runs.
		 */
		class ColorGradingEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& grading = static_cast<ColorGrading&>(a_feature);
				if (grading.curveReadbackRequested)
					return "its curve preview reads back through D3D11";
				if (grading.saveImagesFlag)
					return "it saves its LUT and output through D3D11";
				// The tonemapper is a define: another one is another program.
				a_key.push_back(std::hash<std::string_view>{}(grading.TonemapFunctionName()));
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature& a_feature) override
			{
				auto& grading = static_cast<ColorGrading&>(a_feature);
				if (!sampler) {
					// ColorGrading::linearSampler's description: linear, clamped, every mip.
					sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return;
					}
				}
				if (!lut) {
					D3D11_TEXTURE3D_DESC desc{};
					desc.Width = desc.Height = desc.Depth = ColorGrading::LUTDim;
					desc.MipLevels = 1;
					desc.Format = kFormat;
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					lut = std::make_unique<GraphResources::Texture3D>(desc, "PostProcessing::ColorGrading::LUT",
						GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
				}
				a_builder.Register(*lut);

				const std::vector<std::pair<std::string, std::string>> defines{ { "TONEMAP_FUNC", std::string(grading.TonemapFunctionName()) } };
				auto bake = a_builder.Compute(kShader, ShaderBindings::Words<ColorBlock>(), defines, L"CSLUTGen");
				auto grade = a_builder.Raster(kShader, ShaderBindings::Words<ColorBlock>(), { kFormat }, defines, L"PSColorGrading");
				auto image = a_builder.CreateImage("PostProcessing::ColorGrading::Output", kFormat, D3D11_BIND_RENDER_TARGET);
				constants = a_builder.Latch<ColorGrading::ColorCB>();
				bakeGate = a_builder.Gate();
				const auto linear = Bind::Handle(sampler.Index());
				a_builder.Dispatch({ .name = "lut",
					.program = std::move(bake),
					.bindings = { Bind::Unused(), Bind::Unused(), Bind::Uav(lut->Graph()), linear, Bind::Latch(constants) },
					.gate = bakeGate });
				a_builder.Draw({ .name = "draw",
					.program = std::move(grade),
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Srv(lut->Graph()), Bind::Unused(), linear, Bind::Latch(constants) },
					.target = image });
				a_builder.SetCurrent(image);
				// A new build may have a new bake program: bake again once it runs.
				lutValid = false;
				pending = false;
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				const auto data = static_cast<ColorGrading&>(a_feature).BuildConstants();
				a_commit.Write(constants, data);
				pending = false;
				// As the D3D11 draw: the LUT depends only on the constants (and the program, which a rebuild changes).
				const bool bake = !data.skipLUT && (!lutValid || std::memcmp(&data, lutKey.data(), sizeof(data)) != 0);
				if (bake) {
					a_commit.Open(bakeGate, kLutGroups, kLutGroups, kLutGroups);
					std::memcpy(pendingKey.data(), &data, sizeof(data));
					pending = true;
				}
			}

			void Executed(PostProcessFeature&) override
			{
				// The bake this frame's commit asked for reached the GPU with this build.
				if (!pending)
					return;
				lutKey = pendingKey;
				lutValid = true;
				pending = false;
			}

		private:
			std::unique_ptr<GraphResources::Texture3D> lut;
			org::OwnedDescriptorBinding sampler;
			LatchRegion<ColorGrading::ColorCB> constants;
			GateEntry bakeGate;
			std::array<char, sizeof(ColorGrading::ColorCB)> lutKey{};
			bool lutValid = false;
			// The constants this frame's commit baked with, until Executed.
			std::array<char, sizeof(ColorGrading::ColorCB)> pendingKey{};
			bool pending = false;
		};
	}

	std::unique_ptr<Effect> MakeColorGrading() { return std::make_unique<ColorGradingEffect>(); }
}

#endif
