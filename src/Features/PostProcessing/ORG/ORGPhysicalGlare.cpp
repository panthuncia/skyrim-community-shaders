#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/LinearLighting.h"
#	include "Features/PostProcessing/PhysicalGlare.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"

#	include <algorithm>
#	include <array>
#	include <format>
#	include <optional>

namespace ORGPostChain
{
	namespace
	{
		constexpr DXGI_FORMAT kComplexFormat = DXGI_FORMAT_R32G32_FLOAT;       // texFFT, texPSF_FFT, texApertureBase
		constexpr DXGI_FORMAT kPackedFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;  // texGlarePacked
		constexpr const char* kChannels[3] = { "R", "G", "B" };

		// Each shader's resource list in order.
		struct ThresholdBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle texColor, fftR, fftG, fftB;
			ShaderBindings::Latch glare;
		};
		struct ApertureBlock
		{
			ShaderBindings::Handle aperture;
			ShaderBindings::Latch glare;
		};
		struct TearFilmBlock
		{
			ShaderBindings::Handle apertureBase, aperture;
			ShaderBindings::Latch glare;
		};
		struct PsfBlock
		{
			ShaderBindings::Handle diffraction, psfR, psfG, psfB, wrapSampler;
			ShaderBindings::Latch glare;
		};
		struct FftBlock
		{
			ShaderBindings::Handle input, output;
		};
		struct MultiplyBlock
		{
			ShaderBindings::Handle sceneR, psfR, sceneG, psfG, sceneB, psfB, resultR, resultG, resultB;
			ShaderBindings::Latch glare;
		};
		struct PackBlock
		{
			ShaderBindings::Handle ifftR, ifftG, ifftB, packed;
		};
		struct CompositeBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle scene, ifft, output;
			ShaderBindings::Latch glare;
		};

		/** @brief The PSF then starts from the tear-film pass and is made every frame. */
		bool TearFilmAnimated(const PhysicalGlare::Settings& a_effective) { return a_effective.ApertureMode == 1 && a_effective.TearFilmStrength > 0.f; }

		/** @brief The pipeline's instance now, since SyncPipelineResources may replace it. */
		PhysicalGlare& Live()
		{
			return *globals::features::postProcessing.GetPipelineFeature<PhysicalGlare>(PostProcessing::FeaturePipelineIndex::PhysicalGlare);
		}

		/** @brief What every build shares: the declared aperture and PSF spectrum, and what the PSF in them was made from. */
		struct Persistent
		{
			RenderGraph::Frame::DeclaredTexture2D apertureBase;
			std::array<RenderGraph::Frame::DeclaredTexture2D, 3> psfSpectrum;
			org::OwnedDescriptorBinding wrapSampler;
			PhysicalGlare::PSFParams made{};
			bool madeValid = false;
		};

		/** @brief One build's latch regions, gate entries and sizes, and what its frames asked for until Executed. */
		class PhysicalGlareRun final : public Run
		{
		public:
			struct Regions
			{
				LatchRegion<PhysicalGlare::GlareCB> constants, apertureConstants;
				GateEntry apertureGate, psfGate, psfFftGate;
				uint fftSize = PhysicalGlare::FFT_MIN;
				uint groups = 0;
				float width = 0.f, height = 0.f;
			};

			PhysicalGlareRun(Persistent& a_persistent, const Regions& a_regions) :
				persistent(a_persistent), regions(a_regions) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& glare = static_cast<PhysicalGlare&>(a_feature);
				const auto effective = glare.GetEffectiveSettings();
				const float deltaTime = *globals::game::deltaTime;
				// The tear film's clock runs while it animates.
				if (TearFilmAnimated(effective))
					glare.tearFilmTimeAccum += deltaTime;
				const bool useAP1 = globals::features::linearLighting.IsACEScgActive();

				pending.reset();

				// A new version of the textures is born empty. A new build may have new programs. Either way, make the PSF again.
				const auto& adopted = RenderGraph::Frame::Revision::Adopted();
				if (fresh || adopted.FirstFrame(persistent.apertureBase) || adopted.FirstFrame(persistent.psfSpectrum[0]))
					persistent.madeValid = false;
				fresh = false;

				const uint fftSize = regions.fftSize;
				const bool remakeAperture = !persistent.madeValid || PhysicalGlare::ApertureStale(persistent.made, effective, fftSize);
				const bool remakePsf = remakeAperture || PhysicalGlare::PSFStale(persistent.made, effective, fftSize, useAP1);
				if (remakePsf)
					pending = PhysicalGlare::CapturePSFParams(effective, fftSize, useAP1);

				const auto data = glare.BuildConstants(effective, deltaTime, fftSize, regions.width, regions.height);
				a_commit.Write(regions.constants, data);
				// The static aperture leaves out the tear film, which has its own pass.
				auto apertureData = data;
				apertureData.TearFilmStrength = 0.f;
				a_commit.Write(regions.apertureConstants, apertureData);
				if (remakeAperture)
					a_commit.Open(regions.apertureGate, regions.groups, regions.groups);
				if (remakePsf) {
					a_commit.Open(regions.psfGate, regions.groups, regions.groups);
					a_commit.Open(regions.psfFftGate, fftSize);
				}
			}

			void Executed(PostProcessFeature&) override
			{
				if (!pending)
					return;
				persistent.made = *pending;
				persistent.madeValid = true;
				pending.reset();
			}

		private:
			Persistent& persistent;
			Regions regions;
			std::optional<PhysicalGlare::PSFParams> pending;
			bool fresh = true;  // until the first commit
		};

		/**
		* @brief PhysicalGlare::Draw on the graph. The aperture and PSF spectrum are persistent, declared on the FFT size, and remade only
		* when their settings change. Their passes are gated, so the recording stays the same, and a request repeats until a frame with
		* it reached the GPU with this build. The glare is a published side output for Composite.
		*/
		class PhysicalGlareEffect final : public Effect
		{
		public:
			bool Legacy() const override { return false; }

			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies& a_dependencies) override
			{
				using namespace RenderGraph::Frame;
				auto& glare = static_cast<PhysicalGlare&>(a_feature);
				a_dependencies.push_back(DependencyOf(glare.settings.FFTResolution));
				a_dependencies.push_back(DependencyOf(glare.settings.ApertureMode));

				// N x N at the FFT size, and one texel while the effect is off, so it holds no memory then.
				const auto desc = [](const Inputs& a_in) {
					const auto& live = Live();
					const uint size = a_in.Get(live.enabled) ? PhysicalGlare::NormaliseFFTResolution(a_in.Get(live.settings.FFTResolution)) : 1u;
					D3D11_TEXTURE2D_DESC result{};
					result.Width = result.Height = size;
					result.MipLevels = 1;
					result.ArraySize = 1;
					result.Format = kComplexFormat;
					result.SampleDesc = { 1, 0 };
					result.Usage = D3D11_USAGE_DEFAULT;
					result.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					return result;
				};
				const auto dependencies = DependsOn(glare.settings.FFTResolution, glare.enabled);
				const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::Persistent };
				persistent.apertureBase = DeclareTexture2D("PostProcessing::PhysicalGlare::ApertureBase", dependencies, desc, options);
				for (int channel = 0; channel < 3; ++channel)
					persistent.psfSpectrum[channel] = DeclareTexture2D(std::format("PostProcessing::PhysicalGlare::PSF_FFT_{}", kChannels[channel]), dependencies, desc, options);
			}

			void Key(const RenderGraph::Frame::Inputs& a_values, PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				const auto& glare = static_cast<PhysicalGlare&>(a_feature);
				// The FFT size, and pupil mode (the tear film's pass needs it).
				a_key.push_back(PhysicalGlare::NormaliseFFTResolution(a_values.Get(glare.settings.FFTResolution)));
				a_key.push_back(a_values.Get(glare.settings.ApertureMode) == 1 ? 1u : 0u);
			}

			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_observed) override
			{
				auto& glare = static_cast<PhysicalGlare&>(a_feature);
				if (glare.DebugViewShown())
					return "its debug view shows the D3D11 glare";
				// The tear film's strength is no variable yet.
				a_observed.push_back(glare.GetEffectiveSettings().TearFilmStrength > 0.f ? 1u : 0u);
				return nullptr;
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature& a_feature, std::span<const std::uint64_t> a_key) override
			{
				if (a_key.size() != 2) {
					a_builder.Fail("its key is not the one Key gives");
					return nullptr;
				}
				const uint size = static_cast<uint>(a_key[0]);
				// The strength is observed each frame, so this build was made for the strength read here.
				const bool tearFilm = a_key[1] != 0 && static_cast<PhysicalGlare&>(a_feature).GetEffectiveSettings().TearFilmStrength > 0.f;
				const auto& shape = a_builder.GetShape();

				auto& wrapSampler = persistent.wrapSampler;
				if (!wrapSampler) {
					// PhysicalGlare::wrapSampler: linear, wrapped, every mip.
					wrapSampler = ShaderBindings::CreateSampler(
						{ .addressU = rhi::AddressMode::Wrap, .addressV = rhi::AddressMode::Wrap, .addressW = rhi::AddressMode::Wrap });
					if (!wrapSampler) {
						a_builder.Fail("its sampler could not be made");
						return nullptr;
					}
				}
				const auto& apertureBase = persistent.apertureBase;
				const auto& psfSpectrum = persistent.psfSpectrum;
				if (!apertureBase || !*apertureBase || !std::ranges::all_of(psfSpectrum, [](const auto& a_texture) { return a_texture && *a_texture; })) {
					a_builder.Fail("its PSF textures could not be made");
					return nullptr;
				}
				a_builder.Register(*apertureBase);
				for (const auto& texture : psfSpectrum)
					a_builder.Register(*texture);

				constexpr const char* kDir = "PostProcessing/PhysicalGlare/";
				const auto program = [&](const char* a_file, std::uint32_t a_words, const wchar_t* a_entry, std::vector<std::pair<std::string, std::string>> a_defines = {}) {
					return a_builder.Compute(std::format("{}{}", kDir, a_file), a_words, std::move(a_defines), a_entry);
				};
				const auto fft = [&](const char* a_axis, const char* a_direction) {
					return program("fft.cs.hlsl", ShaderBindings::Words<FftBlock>(), L"CS_FFT", { { a_axis, "" }, { a_direction, "" }, { "FFT_SIZE", std::to_string(size) } });
				};
				const auto threshold = program("threshold.cs.hlsl", ShaderBindings::Words<ThresholdBlock>(), L"CS_Threshold");
				const auto aperture = program("aperture.cs.hlsl", ShaderBindings::Words<ApertureBlock>(), L"CS_Aperture");
				const auto tearFilmProgram = tearFilm ? program("tearfilm.cs.hlsl", ShaderBindings::Words<TearFilmBlock>(), L"CS_TearFilm") : nullptr;
				const auto psf = program("psf.cs.hlsl", ShaderBindings::Words<PsfBlock>(), L"CS_ChromaticBlur");
				const auto multiply = program("multiply.cs.hlsl", ShaderBindings::Words<MultiplyBlock>(), L"CS_Multiply");
				const auto pack = program("pack.cs.hlsl", ShaderBindings::Words<PackBlock>(), L"CS_Pack");
				const auto composite = program("composite.cs.hlsl", ShaderBindings::Words<CompositeBlock>(), L"CS_Composite");
				const auto rowForward = fft("ROW_PASS", "FORWARD");
				const auto colForward = fft("COL_PASS", "FORWARD");
				const auto rowInverse = fft("ROW_PASS", "INVERSE");
				const auto colInverse = fft("COL_PASS", "INVERSE");
				if (a_builder.Failed())
					return nullptr;

				// The FFT ping-pong pair per channel, the packed result, and the output.
				Image work[3][2];
				for (int channel = 0; channel < 3; ++channel)
					for (int side = 0; side < 2; ++side)
						work[channel][side] = a_builder.CreateImage(std::format("PostProcessing::PhysicalGlare::FFT{}{}", kChannels[channel], side), kComplexFormat,
							D3D11_BIND_UNORDERED_ACCESS, size, size);
				const auto packed = a_builder.CreateImage("PostProcessing::PhysicalGlare::Packed", kPackedFormat, D3D11_BIND_UNORDERED_ACCESS, size, size);
				// As PhysicalGlare::SetupResources, the output takes kMAIN_COPY's format.
				const auto output = a_builder.CreateImage(Published::kGlare, shape.mainCopyFormat, D3D11_BIND_UNORDERED_ACCESS);
				if (a_builder.Failed())
					return nullptr;

				PhysicalGlareRun::Regions regions;
				regions.constants = a_builder.Latch<PhysicalGlare::GlareCB>();
				regions.apertureConstants = a_builder.Latch<PhysicalGlare::GlareCB>();
				regions.apertureGate = a_builder.Gate();
				regions.psfGate = a_builder.Gate();
				regions.psfFftGate = a_builder.Gate();
				regions.groups = (size + 7) >> 3;
				regions.fftSize = size;
				regions.width = static_cast<float>(shape.width);
				regions.height = static_cast<float>(shape.height);
				const auto groups = regions.groups;
				const auto psfFftGate = regions.psfFftGate;

				const auto glareLatch = Bind::Latch(regions.constants);
				const auto dispatchFft = [&](std::string a_name, const std::shared_ptr<const ComputeProgram>& a_program, const auto& a_input, const auto& a_output,
											 bool a_psf) {
					ComputeDispatch dispatch{ .name = std::move(a_name), .program = a_program, .bindings = { Bind::Srv(a_input), Bind::Uav(a_output) }, .groups = { size, 1, 1 } };
					if (a_psf)
						dispatch.gate = psfFftGate;
					a_builder.Dispatch(std::move(dispatch));
				};

				// As GeneratePSF: static aperture, tear film, FFT, chromatic blur, then each channel's FFT into the spectrum.
				a_builder.Dispatch({ .name = "aperture",
					.program = aperture,
					.bindings = { Bind::Uav(*apertureBase), Bind::Latch(regions.apertureConstants) },
					.gate = regions.apertureGate });
				if (tearFilm) {
					a_builder.Dispatch({ .name = "tear-film",
						.program = tearFilmProgram,
						.bindings = { Bind::Srv(*apertureBase), Bind::Uav(work[0][0]), glareLatch },
						.gate = regions.psfGate });
					dispatchFft("psf-aperture-row", rowForward, work[0][0], work[0][1], true);
				} else {
					dispatchFft("psf-aperture-row", rowForward, *apertureBase, work[0][1], true);
				}
				dispatchFft("psf-aperture-col", colForward, work[0][1], work[0][0], true);
				a_builder.Dispatch({ .name = "psf",
					.program = psf,
					.bindings = { Bind::Srv(work[0][0]), Bind::Uav(work[0][1]), Bind::Uav(work[1][1]), Bind::Uav(work[2][1]), Bind::Handle(wrapSampler.Index()), glareLatch },
					.gate = regions.psfGate });
				for (int channel = 0; channel < 3; ++channel) {
					dispatchFft(std::format("psf-row-{}", kChannels[channel]), rowForward, work[channel][1], work[channel][0], true);
					dispatchFft(std::format("psf-col-{}", kChannels[channel]), colForward, work[channel][0], *psfSpectrum[channel], true);
				}

				// As Draw: threshold, forward FFT, multiply by the PSF, inverse FFT, pack, composite.
				a_builder.Dispatch({ .name = "threshold",
					.program = threshold,
					.bindings = { Bind::Shared(), Bind::Srv(a_builder.Current()), Bind::Uav(work[0][0]), Bind::Uav(work[1][0]), Bind::Uav(work[2][0]), glareLatch },
					.groups = { groups, groups, 1 } });
				for (int channel = 0; channel < 3; ++channel) {
					dispatchFft(std::format("row-{}", kChannels[channel]), rowForward, work[channel][0], work[channel][1], false);
					dispatchFft(std::format("col-{}", kChannels[channel]), colForward, work[channel][1], work[channel][0], false);
				}
				a_builder.Dispatch({ .name = "multiply",
					.program = multiply,
					.bindings = { Bind::Srv(work[0][0]), Bind::Srv(*psfSpectrum[0]), Bind::Srv(work[1][0]), Bind::Srv(*psfSpectrum[1]), Bind::Srv(work[2][0]),
						Bind::Srv(*psfSpectrum[2]), Bind::Uav(work[0][1]), Bind::Uav(work[1][1]), Bind::Uav(work[2][1]), glareLatch },
					.groups = { groups, groups, 1 } });
				for (int channel = 0; channel < 3; ++channel) {
					dispatchFft(std::format("inverse-row-{}", kChannels[channel]), rowInverse, work[channel][1], work[channel][0], false);
					dispatchFft(std::format("inverse-col-{}", kChannels[channel]), colInverse, work[channel][0], work[channel][1], false);
				}
				a_builder.Dispatch({ .name = "pack",
					.program = pack,
					.bindings = { Bind::Srv(work[0][1]), Bind::Srv(work[1][1]), Bind::Srv(work[2][1]), Bind::Uav(packed) },
					.groups = { groups, groups, 1 } });
				a_builder.Dispatch({ .name = "composite",
					.program = composite,
					.bindings = { Bind::Shared(), Bind::Srv(a_builder.Current()), Bind::Srv(packed), Bind::Uav(output), glareLatch },
					.groups = { (shape.width + 7) >> 3, (shape.height + 7) >> 3, 1 } });
				a_builder.Publish(Published::kGlare, output);
				if (a_builder.Failed())
					return nullptr;
				return std::make_shared<PhysicalGlareRun>(persistent, regions);
			}

		private:
			Persistent persistent;
		};
	}

	std::unique_ptr<Effect> MakePhysicalGlare() { return std::make_unique<PhysicalGlareEffect>(); }
}

#endif
