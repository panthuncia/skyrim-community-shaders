#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing.h"
#	include "Features/PostProcessing/ColorGrading.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <cstring>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/ColorGrading/colorgrading.hlsl";
		constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // as ColorGrading::SetupResources
		constexpr std::uint32_t kLutGroups = ColorGrading::LUTDim >> 3;

		// COLOR_GRADING_RESOURCES, then the ColorGradingFrame latch.
		struct ColorBlock
		{
			ShaderBindings::Handle texColor, texLUT, rwLUT, linearSampler;
			ShaderBindings::Latch colorGradingFrame;
		};

		ColorGrading& Live() { return *globals::features::postProcessing.GetPipelineFeature<ColorGrading>(PostProcessing::FeaturePipelineIndex::ColorGrading); }

		/**
		* @brief One build's draw and gated LUT bake. A bake repeats until a frame with it reached the GPU with this build, and a new
		* build bakes again, since it may have a new bake program. A new LUT version bakes too.
		*/
		class ColorGradingRun final : public Run
		{
		public:
			ColorGradingRun(const RenderGraph::Frame::DeclaredTexture3D& a_lut, LatchRegion<ColorGrading::ColorCB> a_constants, GateEntry a_bakeGate) :
				lut(a_lut), constants(a_constants), bakeGate(a_bakeGate) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				const auto data = static_cast<ColorGrading&>(a_feature).BuildConstants();
				a_commit.Write(constants, data);
				pending = false;
				if (RenderGraph::Frame::Revision::Adopted().FirstFrame(lut))
					lutValid = false;
				// The LUT depends only on the constants and the program.
				const bool bake = !data.skipLUT && (!lutValid || std::memcmp(&data, lutKey.data(), sizeof(data)) != 0);
				if (bake) {
					a_commit.Open(bakeGate, kLutGroups, kLutGroups, kLutGroups);
					std::memcpy(pendingKey.data(), &data, sizeof(data));
					pending = true;
				}
			}

			void Executed(PostProcessFeature&) override
			{
				if (!pending)
					return;
				lutKey = pendingKey;
				lutValid = true;
				pending = false;
			}

		private:
			const RenderGraph::Frame::DeclaredTexture3D& lut;  // the effect's, for the session
			LatchRegion<ColorGrading::ColorCB> constants;
			GateEntry bakeGate;
			std::array<char, sizeof(ColorGrading::ColorCB)> lutKey{};
			bool lutValid = false;
			// Until Executed.
			std::array<char, sizeof(ColorGrading::ColorCB)> pendingKey{};
			bool pending = false;
		};

		/**
		* @brief ColorGrading::Draw on the graph: a gated LUT bake when the constants changed, then the grading draw. The LUT is
		* declared on the effect's on/off, one texel while off. The curve preview and image export read back through D3D11, so the
		* chain stays on D3D11 while either runs. The tonemapper is a define, so it is observed (a program choice: DeclareProgram later).
		*/
		class ColorGradingEffect final : public Effect
		{
		public:
			bool Legacy() const override { return false; }

			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies& a_dependencies) override
			{
				using namespace RenderGraph::Frame;
				(void)a_dependencies;
				lut = DeclareTexture3D("PostProcessing::ColorGrading::LUT", DependsOn(a_feature.enabled),
					[](const Inputs& a_in) {
						const UINT size = a_in.Get(Live().enabled) ? ColorGrading::LUTDim : 1u;
						D3D11_TEXTURE3D_DESC desc{};
						desc.Width = desc.Height = desc.Depth = size;
						desc.MipLevels = 1;
						desc.Format = kFormat;
						desc.Usage = D3D11_USAGE_DEFAULT;
						desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
						return desc;
					},
					GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
			}

			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_observed) override
			{
				auto& grading = static_cast<ColorGrading&>(a_feature);
				if (grading.curveReadbackRequested)
					return "its curve preview reads back through D3D11";
				if (grading.saveImagesFlag)
					return "it saves its LUT and output through D3D11";
				if (!lut)
					return "its LUT could not be declared";
				// The tonemapper is a define, so another one is another program.
				a_observed.push_back(std::hash<std::string_view>{}(grading.TonemapFunctionName()));
				return nullptr;
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature& a_feature, std::span<const std::uint64_t>) override
			{
				auto& grading = static_cast<ColorGrading&>(a_feature);
				if (!sampler) {
					// Linear, clamped, every mip.
					sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return nullptr;
					}
				}
				a_builder.Register(*lut);

				// Observed in the key (Update), so the build and its key agree.
				const std::vector<std::pair<std::string, std::string>> defines{ { "TONEMAP_FUNC", std::string(grading.TonemapFunctionName()) } };
				auto bake = a_builder.Compute(kShader, ShaderBindings::Words<ColorBlock>(), defines, L"CSLUTGen");
				auto grade = a_builder.Raster(kShader, ShaderBindings::Words<ColorBlock>(), { kFormat }, defines, L"PSColorGrading");
				auto image = a_builder.CreateImage("PostProcessing::ColorGrading::Output", kFormat, D3D11_BIND_RENDER_TARGET);
				const auto constants = a_builder.Latch<ColorGrading::ColorCB>();
				const auto bakeGate = a_builder.Gate();
				const auto linear = Bind::Handle(sampler.Index());
				a_builder.Dispatch({ .name = "lut",
					.program = std::move(bake),
					.bindings = { Bind::Unused(), Bind::Unused(), Bind::Uav(*lut), linear, Bind::Latch(constants) },
					.gate = bakeGate });
				a_builder.Draw({ .name = "draw",
					.program = std::move(grade),
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Srv(*lut), Bind::Unused(), linear, Bind::Latch(constants) },
					.target = image });
				a_builder.SetCurrent(image);
				return std::make_shared<ColorGradingRun>(lut, constants, bakeGate);
			}

		private:
			RenderGraph::Frame::DeclaredTexture3D lut;
			org::OwnedDescriptorBinding sampler;
		};
	}

	std::unique_ptr<Effect> MakeColorGrading() { return std::make_unique<ColorGradingEffect>(); }
}

#endif
