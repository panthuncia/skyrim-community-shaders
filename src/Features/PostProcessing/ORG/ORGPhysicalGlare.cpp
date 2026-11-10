#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/LinearLighting.h"
#	include "Features/PostProcessing/PhysicalGlare.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr DXGI_FORMAT kComplexFormat = DXGI_FORMAT_R32G32_FLOAT;       // texFFT's, texPSF_FFT's and texApertureBase's
		constexpr DXGI_FORMAT kPackedFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;  // texGlarePacked's
		constexpr const char* kOutputName = "PostProcessing::PhysicalGlare::Output";
		constexpr const char* kChannels[3] = { "R", "G", "B" };

		// The push blocks, each the shader's resource list in order (PhysicalGlare/*.cs.hlsl).
		struct ThresholdBlock
		{
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
			ShaderBindings::Handle scene, ifft, output;
			ShaderBindings::Latch glare;
		};

		using Groups = std::array<std::uint32_t, 4>;

		/** @brief Whether the animated tear film is on: the PSF then starts from the tear-film pass and is made every frame. */
		bool TearFilmAnimated(const PhysicalGlare::Settings& a_effective) { return a_effective.ApertureMode == 1 && a_effective.TearFilmStrength > 0.f; }

		/**
		 * @brief PhysicalGlare::Draw on the graph. The aperture response and the PSF's spectrum are persistent and owned here; they
		 * are made again only when their settings change (every frame while the tear film animates). Those passes are indirect
		 * dispatches whose group counts Commit writes, zero on the frames they are skipped, so the recording stays the same. The
		 * scene's FFT work images and the packed result are transient chain images at the FFT size. The glare is a side output:
		 * the image is published as "PostProcessing::PhysicalGlare::Output" for Composite, and Current is left alone.
		 *
		 * A regenerated PSF counts as made only once the build is known to be in the graph (its threshold pass was prepared): a
		 * commit cannot tell which build the epoch runs, so until then the request is repeated each frame.
		 */
		class PhysicalGlareEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& glare = static_cast<PhysicalGlare&>(a_feature);
				if (glare.DebugViewShown())
					return "its debug view shows the D3D11 glare";
				a_key.push_back(PhysicalGlare::NormaliseFFTResolution(glare.settings.FFTResolution));
				a_key.push_back(TearFilmAnimated(glare.GetEffectiveSettings()) ? 1u : 0u);
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature& a_feature) override
			{
				auto& glare = static_cast<PhysicalGlare&>(a_feature);
				const uint size = PhysicalGlare::NormaliseFFTResolution(glare.settings.FFTResolution);
				const bool tearFilm = TearFilmAnimated(glare.GetEffectiveSettings());
				const auto& shape = a_builder.GetShape();

				if (!wrapSampler) {
					// PhysicalGlare::wrapSampler's description: linear, wrapped, every mip.
					wrapSampler = ShaderBindings::CreateSampler(
						{ .addressU = rhi::AddressMode::Wrap, .addressV = rhi::AddressMode::Wrap, .addressW = rhi::AddressMode::Wrap });
					if (!wrapSampler) {
						a_builder.Fail("its sampler could not be made");
						return;
					}
				}
				if (!CreatePersistent(size)) {
					a_builder.Fail("its PSF textures could not be made");
					return;
				}
				a_builder.Register(*apertureBase);
				for (const auto& texture : psfSpectrum)
					a_builder.Register(*texture);

				// Programs.
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
					return;

				// Images: the FFT ping-pong pair per channel, the packed result, and the output, as PhysicalGlare's textures.
				Image work[3][2];
				for (int channel = 0; channel < 3; ++channel)
					for (int side = 0; side < 2; ++side)
						work[channel][side] = a_builder.CreateImage(std::format("PostProcessing::PhysicalGlare::FFT{}{}", kChannels[channel], side), kComplexFormat,
							D3D11_BIND_UNORDERED_ACCESS, size, size);
				const auto packed = a_builder.CreateImage("PostProcessing::PhysicalGlare::Packed", kPackedFormat, D3D11_BIND_UNORDERED_ACCESS, size, size);
				// As PhysicalGlare::SetupResources: the output takes kMAIN_COPY's format.
				const auto output = a_builder.CreateImage(kOutputName, shape.mainCopyFormat, D3D11_BIND_UNORDERED_ACCESS);
				if (a_builder.Failed())
					return;

				constants = a_builder.Latch<PhysicalGlare::GlareCB>();
				apertureConstants = a_builder.Latch<PhysicalGlare::GlareCB>();
				apertureGroups = a_builder.Latch<Groups>();
				psfGroups = a_builder.Latch<Groups>();
				psfFftGroups = a_builder.Latch<Groups>();
				groups = (size + 7) >> 3;
				fftSize = size;

				const auto glareLatch = Bind::Latch(constants);
				const auto dispatchFft = [&](std::string a_name, const std::shared_ptr<const ComputeProgram>& a_program, const auto& a_input, const auto& a_output,
											 bool a_psf) {
					ComputeDispatch dispatch{ .name = std::move(a_name), .program = a_program, .bindings = { Bind::Srv(a_input), Bind::Uav(a_output) }, .groups = { size, 1, 1 } };
					if (a_psf)
						dispatch.indirect = psfFftGroups;
					a_builder.Dispatch(std::move(dispatch));
				};

				// The PSF, as GeneratePSF: the static aperture, the tear film over it, its FFT, the chromatic blur, then each
				// channel's FFT into the persistent spectrum.
				a_builder.Dispatch({ .name = "aperture",
					.program = aperture,
					.bindings = { Bind::Uav(apertureBase->Graph()), Bind::Latch(apertureConstants) },
					.indirect = apertureGroups });
				if (tearFilm) {
					a_builder.Dispatch({ .name = "tear-film",
						.program = tearFilmProgram,
						.bindings = { Bind::Srv(apertureBase->Graph()), Bind::Uav(work[0][0]), glareLatch },
						.indirect = psfGroups });
					dispatchFft("psf-aperture-row", rowForward, work[0][0], work[0][1], true);
				} else {
					dispatchFft("psf-aperture-row", rowForward, apertureBase->Graph(), work[0][1], true);
				}
				dispatchFft("psf-aperture-col", colForward, work[0][1], work[0][0], true);
				a_builder.Dispatch({ .name = "psf",
					.program = psf,
					.bindings = { Bind::Srv(work[0][0]), Bind::Uav(work[0][1]), Bind::Uav(work[1][1]), Bind::Uav(work[2][1]), Bind::Handle(wrapSampler.Index()), glareLatch },
					.indirect = psfGroups });
				for (int channel = 0; channel < 3; ++channel) {
					dispatchFft(std::format("psf-row-{}", kChannels[channel]), rowForward, work[channel][1], work[channel][0], true);
					dispatchFft(std::format("psf-col-{}", kChannels[channel]), colForward, work[channel][0], psfSpectrum[channel]->Graph(), true);
				}

				// The scene, as Draw: threshold into the FFT size, forward FFT, multiply by the PSF, inverse FFT, pack, composite.
				prepared = std::make_shared<std::atomic<bool>>(false);
				pending.reset();
				a_builder.Dispatch({ .name = "threshold",
					.program = threshold,
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Uav(work[0][0]), Bind::Uav(work[1][0]), Bind::Uav(work[2][0]), glareLatch },
					.groups = { groups, groups, 1 },
					.prepared = prepared });
				for (int channel = 0; channel < 3; ++channel) {
					dispatchFft(std::format("row-{}", kChannels[channel]), rowForward, work[channel][0], work[channel][1], false);
					dispatchFft(std::format("col-{}", kChannels[channel]), colForward, work[channel][1], work[channel][0], false);
				}
				a_builder.Dispatch({ .name = "multiply",
					.program = multiply,
					.bindings = { Bind::Srv(work[0][0]), Bind::Srv(psfSpectrum[0]->Graph()), Bind::Srv(work[1][0]), Bind::Srv(psfSpectrum[1]->Graph()), Bind::Srv(work[2][0]),
						Bind::Srv(psfSpectrum[2]->Graph()), Bind::Uav(work[0][1]), Bind::Uav(work[1][1]), Bind::Uav(work[2][1]), glareLatch },
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
					.bindings = { Bind::Srv(a_builder.Current()), Bind::Srv(packed), Bind::Uav(output), glareLatch },
					.groups = { (shape.width + 7) >> 3, (shape.height + 7) >> 3, 1 } });
				a_builder.Publish(kOutputName, output);
				width = static_cast<float>(shape.width);
				height = static_cast<float>(shape.height);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& glare = static_cast<PhysicalGlare&>(a_feature);
				const auto effective = glare.GetEffectiveSettings();
				const float deltaTime = *globals::game::deltaTime;
				// As Draw: the tear film's clock runs while it animates.
				if (TearFilmAnimated(effective))
					glare.tearFilmTimeAccum += deltaTime;
				const bool useAP1 = globals::features::linearLighting.IsACEScgActive();

				// The PSF the last commit asked for is made once the build is in the graph: its commit then reached the GPU.
				if (pending && prepared && prepared->load(std::memory_order_acquire)) {
					made = *pending;
					madeValid = true;
				}
				pending.reset();

				const bool remakeAperture = !madeValid || PhysicalGlare::ApertureStale(made, effective, fftSize);
				const bool remakePsf = remakeAperture || PhysicalGlare::PSFStale(made, effective, fftSize, useAP1);
				if (remakePsf)
					pending = PhysicalGlare::CapturePSFParams(effective, fftSize, useAP1);

				const auto data = glare.BuildConstants(effective, deltaTime, fftSize, width, height);
				a_commit.Write(constants, data);
				// As GeneratePSF: the static aperture leaves the tear film out; it is applied by its own pass.
				auto apertureData = data;
				apertureData.TearFilmStrength = 0.f;
				a_commit.Write(apertureConstants, apertureData);
				a_commit.Write(apertureGroups, remakeAperture ? Groups{ groups, groups, 1, 0 } : Groups{});
				a_commit.Write(psfGroups, remakePsf ? Groups{ groups, groups, 1, 0 } : Groups{});
				a_commit.Write(psfFftGroups, remakePsf ? Groups{ fftSize, 1, 1, 0 } : Groups{});
			}

		private:
			/** @brief The aperture and PSF spectrum at a_size, made again (and so the PSF too) when the size changed. */
			bool CreatePersistent(uint a_size)
			{
				if (apertureBase && apertureBase->Desc().Width == a_size)
					return true;
				madeValid = false;
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = desc.Height = a_size;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = kComplexFormat;
				desc.SampleDesc = { 1, 0 };
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::Persistent };
				apertureBase = std::make_unique<GraphResources::Texture2D>(desc, "PostProcessing::PhysicalGlare::ApertureBase", options);
				bool good = static_cast<bool>(*apertureBase);
				for (int channel = 0; channel < 3; ++channel) {
					psfSpectrum[channel] = std::make_unique<GraphResources::Texture2D>(desc, std::format("PostProcessing::PhysicalGlare::PSF_FFT_{}", kChannels[channel]), options);
					good = good && static_cast<bool>(*psfSpectrum[channel]);
				}
				if (!good)
					apertureBase.reset();
				return good;
			}

			// Persistent, at the build's FFT size.
			std::unique_ptr<GraphResources::Texture2D> apertureBase;
			std::array<std::unique_ptr<GraphResources::Texture2D>, 3> psfSpectrum;
			org::OwnedDescriptorBinding wrapSampler;

			// What the persistent PSF was made from, and what the last commit asked for.
			PhysicalGlare::PSFParams made{};
			bool madeValid = false;
			std::optional<PhysicalGlare::PSFParams> pending;
			std::shared_ptr<std::atomic<bool>> prepared;  // this build's: set once its threshold pass is prepared

			LatchRegion<PhysicalGlare::GlareCB> constants, apertureConstants;
			LatchRegion<Groups> apertureGroups, psfGroups, psfFftGroups;
			uint fftSize = PhysicalGlare::FFT_MIN;
			uint groups = 0;
			float width = 0.f, height = 0.f;
		};
	}

	std::unique_ptr<Effect> MakePhysicalGlare() { return std::make_unique<PhysicalGlareEffect>(); }
}

#endif
