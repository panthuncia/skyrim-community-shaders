// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PostProcessing/ORGPostEffect.h"

#include "Features/PostProcessing.h"
#include "Features/PostProcessing/ColorGrading.h"
#include "Globals.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"

#include <DirectXPackedVector.h>

#include <cstring>

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

		// The curve preview's ramp: one row of CurveSamples RGBA16F texels, read back whole.
		constexpr std::uint32_t kCurveBytes = ColorGrading::CurveSamples * 4 * sizeof(std::uint16_t);
		// A texture readback reserves 16 bytes a texel in its part.
		constexpr std::uint32_t kCurvePartBytes = ColorGrading::CurveSamples * 16;

		/** @brief As ColorGrading::Draw reads its staging copy: the R, G and B halves of each texel. */
		void TakeCurve(ColorGrading& a_grading, const std::vector<std::byte>& a_bytes)
		{
			if (a_bytes.size() < kCurveBytes)
				return;
			const auto* pixels = reinterpret_cast<const std::uint16_t*>(a_bytes.data());
			for (int i = 0; i < ColorGrading::CurveSamples; ++i) {
				a_grading.curveR[i] = DirectX::PackedVector::XMConvertHalfToFloat(pixels[i * 4 + 0]);
				a_grading.curveG[i] = DirectX::PackedVector::XMConvertHalfToFloat(pixels[i * 4 + 1]);
				a_grading.curveB[i] = DirectX::PackedVector::XMConvertHalfToFloat(pixels[i * 4 + 2]);
			}
		}

		/**
		* @brief One build's draw and gated LUT bake. A bake repeats until a frame with it reached the GPU with this build, and a new
		* build bakes again, since it may have a new bake program. A new LUT version bakes too.
		*/
		class ColorGradingRun final : public Run
		{
		public:
			ColorGradingRun(const RenderGraph::Frame::DeclaredTexture3D& a_lut, LatchRegion<ColorGrading::ColorCB> a_constants, GateEntry a_bakeGate,
				std::shared_ptr<RunReadback> a_curve, LatchRegion<org::RunReadback::Stamp> a_curveStamp) :
				lut(a_lut), constants(a_constants), bakeGate(a_bakeGate), curve(std::move(a_curve)), curveStamp(a_curveStamp) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& grading = static_cast<ColorGrading&>(a_feature);
				const auto data = grading.BuildConstants();
				a_commit.Write(constants, data);
				// The curve preview: every run draws the ramp and copies it back, and the menu takes it while the preview is open.
				curve->Commit(a_commit, curveStamp);
				const auto ramp = curve->Take(0);
				const bool previewOpen = grading.curveReadbackRequested &&
				                         grading.curveReadbackRequestFrame >= static_cast<std::int64_t>(globals::state->frameCount) - 1;
				if (previewOpen)
					TakeCurve(grading, ramp);
				else
					grading.curveReadbackRequested = false;
				pending = false;
				// A new LUT version holds only its birth until a bake of it was submitted.
				const auto& adopted = RenderGraph::Frame::Revision::Adopted();
				if (!adopted.Written(lut))
					lutValid = false;
				pendingVersion = adopted.Version(lut);
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
				curve->Executed();
				if (!pending)
					return;
				lutKey = pendingKey;
				lutValid = true;
				pending = false;
				RenderGraph::Frame::NoteWritten(lut, pendingVersion);
			}

		private:
			const RenderGraph::Frame::DeclaredTexture3D& lut;  // the effect's, for the session
			LatchRegion<ColorGrading::ColorCB> constants;
			GateEntry bakeGate;
			std::shared_ptr<RunReadback> curve;  // the effect's, kept across builds
			LatchRegion<org::RunReadback::Stamp> curveStamp;
			std::array<char, sizeof(ColorGrading::ColorCB)> lutKey{};
			bool lutValid = false;
			// Until Executed.
			std::array<char, sizeof(ColorGrading::ColorCB)> pendingKey{};
			bool pending = false;
			const void* pendingVersion = nullptr;  // the LUT version the bake writes
		};

		/**
		* @brief ColorGrading::Draw on the graph: a gated LUT bake when the constants changed, then the grading draw. The LUT is
		* declared on the effect's on/off, one texel while off. The curve preview draws ColorGrading's ramp with the same program
		* and reads it back (RunReadback). The image export captures D3D11 textures, so the menu does not offer it in graph mode.
		* The tonemapper is a define, so it is observed. Its programs are listed in Programs and built on the programs lane.
		*/
		class ColorGradingEffect final : public Effect
		{
		public:
			// The curve preview ramp is made by SetupResources.
			bool ReadsD3D11Resources() const override { return true; }

			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies& a_dependencies) override
			{
				using namespace RenderGraph::Frame;
				(void)a_dependencies;
				lut = DeclareTexture3D("PostProcessing::ColorGrading::LUT", DependsOn(),
					[](const Inputs&) {
						const UINT size = ColorGrading::LUTDim;
						D3D11_TEXTURE3D_DESC desc{};
						desc.Width = desc.Height = desc.Depth = size;
						desc.MipLevels = 1;
						desc.Format = kFormat;
						desc.Usage = D3D11_USAGE_DEFAULT;
						desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
						return desc;
					},
					GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent }, ExistsWhen(a_feature.enabled));
			}

			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_observed) override
			{
				auto& grading = static_cast<ColorGrading&>(a_feature);
				if (!lut)
					return "its LUT could not be declared";
				if (!grading.texCurveInput || !grading.texCurveInput->resource || !grading.texCurveInput->srv)
					return "its curve preview ramp is missing";
				// Loaded once at SetupResources and only read after.
				ramp = FrameAnchors::ImportTexture("cs.d3d11.PostProcessing::ColorGrading::CurveRamp",
					{ .texture = grading.texCurveInput->resource.get(), .srv = grading.texCurveInput->srv.get(), .immutable = true });
				if (!ramp)
					return "its curve preview ramp cannot be imported";
				a_observed.push_back(reinterpret_cast<std::uintptr_t>(ramp.Key()));
				// The tonemapper is a define, so another one is another program.
				a_observed.push_back(std::hash<std::string_view>{}(grading.TonemapFunctionName()));
				return nullptr;
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature& a_feature, std::span<const std::uint64_t>) override
			{
				LoadPrograms(a_programs, static_cast<ColorGrading&>(a_feature));
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature& a_feature, std::span<const std::uint64_t>) override
			{
				auto& grading = static_cast<ColorGrading&>(a_feature);
				if (!sampler) {
					// Linear, clamped, every mip.
					sampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp);
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return nullptr;
					}
				}
				if (!curve)
					curve = RunReadback::Create("PostProcessing::ColorGrading::CurveReadback", { kCurvePartBytes });
				if (!ramp) {
					a_builder.Fail("its curve preview ramp is not imported");
					return nullptr;
				}
				const auto curveStamp = curve->BuildStamp(a_builder);
				a_builder.Register(*lut);

				auto [bake, grade] = LoadPrograms(a_builder, grading);
				auto image = a_builder.CreateImage("PostProcessing::ColorGrading::Output", kFormat, D3D11_BIND_RENDER_TARGET);
				const auto constants = a_builder.Latch<ColorGrading::ColorCB>();
				const auto bakeGate = a_builder.Gate();
				const auto linear = Bind::Handle(sampler.Index());
				a_builder.Dispatch({ .name = "lut",
					.program = std::move(bake),
					.bindings = { Bind::Unused(), Bind::Unused(), Bind::Uav(*lut), linear, Bind::Latch(constants) },
					.gate = bakeGate });
				a_builder.Draw({ .name = "draw",
					.program = grade,
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Srv(*lut), Bind::Unused(), linear, Bind::Latch(constants) },
					.target = image });
				// As ColorGrading::Draw's curve: the same program and constants over the ramp, into one row, then read back.
				constexpr auto samples = static_cast<UINT>(ColorGrading::CurveSamples);
				const auto curveImage = a_builder.CreateImage("PostProcessing::ColorGrading::Curve", kFormat, D3D11_BIND_RENDER_TARGET, samples, 1);
				a_builder.Draw({ .name = "curve",
					.program = std::move(grade),
					.bindings = { Bind::Srv(ramp), Bind::Srv(*lut), Bind::Unused(), linear, Bind::Latch(constants) },
					.target = curveImage,
					.viewport = RasterProgram::Viewport{ .width = samples, .height = 1 } });
				curve->AddTexture(a_builder, "curve-readback", 0, curveImage.graph.Plain(), samples, 1);
				a_builder.SetCurrent(image);
				if (a_builder.Failed())
					return nullptr;
				return std::make_shared<ColorGradingRun>(lut, constants, bakeGate, curve, curveStamp);
			}

		private:
			struct GradingPrograms
			{
				std::shared_ptr<const ComputeProgram> bake;
				std::shared_ptr<const RasterProgram> grade;
			};

			/** @brief Listed in Programs and built on the programs lane. The tonemapper is observed in the key (Update), so the build and its key agree. */
			static GradingPrograms LoadPrograms(ProgramSource& a_programs, ColorGrading& a_grading)
			{
				const std::vector<std::pair<std::string, std::string>> defines{ { "TONEMAP_FUNC", std::string(a_grading.TonemapFunctionName()) } };
				return { .bake = a_programs.Compute(kShader, ShaderBindings::Words<ColorBlock>(), defines, L"CSLUTGen"),
					// The grading vertex stage computes the white balance once per draw (colorgrading.hlsl, VSColorGrading).
					.grade = a_programs.RasterTargets(kShader, ShaderBindings::Words<ColorBlock>(), { RasterProgram::Target{ .format = rhi::helpers::ToRHI(kFormat) } }, defines,
						L"PSColorGrading", kShader, L"VSColorGrading") };
			}

			RenderGraph::Frame::DeclaredTexture3D lut;
			org::OwnedDescriptorBinding sampler;
			FrameAnchors::Texture ramp;          // the curve preview's input, from Update
			std::shared_ptr<RunReadback> curve;  // kept across builds
		};
	}

	std::unique_ptr<Effect> MakeColorGrading() { return std::make_unique<ColorGradingEffect>(); }
}

