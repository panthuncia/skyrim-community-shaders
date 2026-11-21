// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PostProcessing/ORGPostEffect.h"

#include "Features/PostProcessing/LensFlare.h"
#include "Globals.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"

#include <algorithm>
#include <array>
#include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/LensFlare/lensflare.cs.hlsl";
		constexpr const char* kFFTShader = "PostProcessing/LensFlare/lensflare_fft.cs.hlsl";
		constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // texThreshold, texGhostHalo, texBlurTemp
		constexpr DXGI_FORMAT kComplexFormat = DXGI_FORMAT_R32G32_FLOAT;     // the FFT textures

		using CB = LensFlare::LensFlareCB;

		// LENS_FLARE_RESOURCES, then the LensFlareFrame latch.
		struct FlareBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle inputTexture, colorSampler, borderSampler, outputTexture;
			ShaderBindings::Latch frame;
		};

		// LENS_FLARE_FFT_RESOURCES, then the LensFlareFrame latch.
		struct FFTBlock
		{
			ShaderBindings::Handle inputTexture, flareTexture, bokehSampler, borderSampler, texComplexA, texComplexB, rwTexComplex, rwTexColor;
			ShaderBindings::Latch frame;
		};

		constexpr std::uint32_t Tiles(std::uint32_t a_size) { return (a_size + 7) >> 3; }

		/** @brief As LensFlare::PrepareBokehFFT compares it. */
		struct KernelKey
		{
			float kernelScale = 0.f;
			float apertureSize = 0.f;
			float apertureRotation = 0.f;
			int apertureBlades = 0;

			bool operator==(const KernelKey&) const = default;
		};

		/** @brief The pipeline's instance now, since SyncPipelineResources may replace it. */
		LensFlare& Live() { return *globals::features::postProcessing.GetPipelineFeature<LensFlare>(PostProcessing::FeaturePipelineIndex::LensFlare); }

		bool FFTMode(int a_ghostMode)
		{
			const auto mode = static_cast<LensFlare::GhostMode>(a_ghostMode);
			return mode == LensFlare::GhostMode::Quality || mode == LensFlare::GhostMode::Ultra;
		}

		/** @brief What one build's passes were made for. */
		struct PassShape
		{
			bool fft = false;
			std::uint32_t fftSize = 0;     // FFT path: N
			std::uint32_t groups = 0;      // FFT path: kernel groups
			std::uint32_t blurPasses = 0;  // Fast path: Kawase down and up pairs
		};

		/** @brief One kernel group of one build: its latch copy, its bake gates, and what its spectrum holds. */
		struct Kernel
		{
			const RenderGraph::Frame::DeclaredTexture2D* spectrum = nullptr;  // the effect's, for the session
			KernelKey key;
			bool baked = false;    // the spectrum holds key's kernel
			bool pending = false;  // this frame's commit bakes it, until Executed
			const void* version = nullptr;  // the spectrum version this frame's commit bakes
			LatchRegion<CB> constants;
			GateEntry prepareGate, fftGate;
		};

		/** @brief As the D3D11 path sets the sizes before a dispatch. */
		CB Sized(CB a_data, std::uint32_t a_outWidth, std::uint32_t a_outHeight, std::uint32_t a_inWidth, std::uint32_t a_inHeight)
		{
			a_data.OutputWidth = static_cast<float>(a_outWidth);
			a_data.OutputHeight = static_cast<float>(a_outHeight);
			a_data.InputWidth = static_cast<float>(a_inWidth);
			a_data.InputHeight = static_cast<float>(a_inHeight);
			return a_data;
		}

		/** @brief A build made while a debug switch is on: it has no passes, so it commits nothing. */
		class NoFlareRun final : public Run
		{
		public:
			void Commit(const CommitContext&, PostProcessFeature&) override {}
		};

		/** @brief One build's latch regions, sizes and kernels. A new build bakes its kernels again. */
		class LensFlareRun final : public Run
		{
		public:
			PassShape shape;
			std::uint32_t fullWidth = 1, fullHeight = 1, halfWidth = 1, halfHeight = 1, quarterWidth = 1, quarterHeight = 1;
			LatchRegion<CB> thresholdConstants, ghostConstants, downConstants, upConstants;
			std::vector<Kernel> kernels;

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				auto& flare = static_cast<LensFlare&>(a_feature);
				const CB data = flare.BuildConstants(fullWidth, fullHeight);
				a_commit.Write(thresholdConstants, Sized(data, halfWidth, halfHeight, fullWidth, fullHeight));
				if (!shape.fft) {
					a_commit.Write(ghostConstants, Sized(data, halfWidth, halfHeight, halfWidth, halfHeight));
					a_commit.Write(downConstants, Sized(data, quarterWidth, quarterHeight, halfWidth, halfHeight));
					a_commit.Write(upConstants, Sized(data, halfWidth, halfHeight, quarterWidth, quarterHeight));
					return;
				}

				// As DrawQuality. The halo is in the first group only.
				const auto groups = flare.KernelGroups(data.ActiveGhostMask);
				const std::uint32_t n = shape.fftSize;
				const auto& adopted = RenderGraph::Frame::Revision::Adopted();
				for (std::size_t i = 0; i < kernels.size(); ++i) {
					auto& kernel = kernels[i];
					CB group = Sized(data, halfWidth, halfHeight, n, n);
					group.FFTResolution = n;
					// The observed words hold the group count, so groups has an entry for every kernel.
					if (i < groups.size()) {
						group.KernelScale = groups[i].kernelScale;
						group.ActiveGhostMask = groups[i].ghostMask;
					} else {
						group.ActiveGhostMask = 0;
					}
					if (i > 0)
						group.HaloStrength = 0.f;
					a_commit.Write(kernel.constants, group);

					// A new version of the spectrum holds only its birth until a bake of it was submitted.
					if (!adopted.Written(*kernel.spectrum))
						kernel.baked = false;
					kernel.version = adopted.Version(*kernel.spectrum);
					const KernelKey key{ group.KernelScale, group.ApertureSize, group.ApertureRotation, group.ApertureBlades };
					if (key != kernel.key) {
						kernel.key = key;
						kernel.baked = false;
					}
					kernel.pending = !kernel.baked;
					if (kernel.pending) {
						a_commit.Open(kernel.prepareGate, Tiles(n), Tiles(n));
						a_commit.Open(kernel.fftGate, n);
					}
				}
			}

			void Executed(PostProcessFeature&) override
			{
				for (auto& kernel : kernels) {
					if (kernel.pending)
						RenderGraph::Frame::NoteWritten(*kernel.spectrum, kernel.version);
					kernel.baked = kernel.baked || kernel.pending;
					kernel.pending = false;
				}
			}
		};

		/**
		* @brief LensFlare::Draw on the graph: threshold, then the Fast path or the FFT path, into the published ghosts-and-halo image.
		* Every pass has its own latch copy of LensFlareCB, because the D3D11 path changes it between dispatches.
		* The kernel spectra are persistent and declared on the FFT size and the ghost mode. Their bake is gated, and Commit keeps
		* opening it until a baking frame reached the GPU with this build.
		*/
		class LensFlareEffect final : public Effect
		{
		public:
			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies& a_dependencies) override
			{
				using namespace RenderGraph::Frame;
				auto& flare = static_cast<LensFlare&>(a_feature);
				a_dependencies.push_back(DependencyOf(flare.settings.FFTResolution));
				a_dependencies.push_back(DependencyOf(flare.settings.GhostModeInt));

				// The group count is no variable yet, so every group a mode can use is declared: the first in both FFT modes, the
				// rest in Ultra only. One texel otherwise, and while the effect is off.
				const auto dependencies = DependsOn(flare.settings.FFTResolution);
				for (int i = 0; i < LensFlare::MAX_KERNEL_GROUPS; ++i) {
					// The existence rule: the effect on and a mode that uses the group.
					const auto used = ExistsWhen(DependsOn(flare.settings.GhostModeInt, flare.enabled), [i](const Inputs& a_in) {
						const auto& live = Live();
						const int mode = a_in.Get(live.settings.GhostModeInt);
						return a_in.Get(live.enabled) && FFTMode(mode) && (i == 0 || static_cast<LensFlare::GhostMode>(mode) == LensFlare::GhostMode::Ultra);
					});
					spectra[static_cast<std::size_t>(i)] = DeclareTexture2D(std::format("PostProcessing::LensFlare::BokehFFT{}", i), dependencies,
						[](const Inputs& a_in) {
							D3D11_TEXTURE2D_DESC desc{};
							desc.Width = desc.Height = LensFlare::NormaliseFFTResolution(a_in.Get(Live().settings.FFTResolution));
							desc.MipLevels = 1;
							desc.ArraySize = 1;
							desc.Format = kComplexFormat;
							desc.SampleDesc = { 1, 0 };
							desc.Usage = D3D11_USAGE_DEFAULT;
							desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
							return desc;
						},
						GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent }, used);
				}
			}

			void Key(const RenderGraph::Frame::Inputs& a_values, PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				const auto& flare = static_cast<LensFlare&>(a_feature);
				const int mode = a_values.Get(flare.settings.GhostModeInt);
				a_key.push_back(static_cast<std::uint64_t>(static_cast<std::uint32_t>(mode)));
				// The FFT size matters on the FFT path only.
				a_key.push_back(FFTMode(mode) ? LensFlare::NormaliseFFTResolution(a_values.Get(flare.settings.FFTResolution)) : 0u);
			}

			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_observed) override
			{
				auto& flare = static_cast<LensFlare&>(a_feature);
				// Disable Threshold or Disable Ghosts: the graph builds no flare, so Composite sees it off. D3D11 keeps last frame's
				// textures instead, which a transient chain image cannot do. A debug switch is no variable, so it is observed.
				observedDebugOff = flare.debugsettings.disableThreshold || flare.debugsettings.disableGhosts;
				a_observed.push_back(observedDebugOff ? 1u : 0u);
				// The ghosts and the debug blur are no variables yet.
				observedGroups = LiveGroups(flare);
				observedBlurPasses = LiveBlurPasses(flare);
				a_observed.push_back(observedGroups);
				a_observed.push_back(observedBlurPasses);
				return nullptr;
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t> a_key) override
			{
				// The build fails on another key.
				if (a_key.size() != 2 || observedDebugOff)
					return;
				LoadPrograms(a_programs, ShapeFor(a_key));
				a_programs.Publishes(Published::kLensFlareOutput);
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t> a_key) override
			{
				if (a_key.size() != 2) {
					a_builder.Fail("its key is not the one Key gives");
					return nullptr;
				}
				// A debug switch builds no passes and publishes nothing.
				if (observedDebugOff)
					return std::make_shared<NoFlareRun>();
				auto run = std::make_shared<LensFlareRun>();
				run->shape = ShapeFor(a_key);
				const auto& shape = run->shape;
				const auto programs = LoadPrograms(a_builder, shape);
				run->fullWidth = a_builder.GetShape().width;
				run->fullHeight = a_builder.GetShape().height;
				run->halfWidth = (std::max)(run->fullWidth / 2, 1u);
				run->halfHeight = (std::max)(run->fullHeight / 2, 1u);
				run->quarterWidth = (std::max)(run->fullWidth / 4, 1u);
				run->quarterHeight = (std::max)(run->fullHeight / 4, 1u);

				if (!MakeSamplers()) {
					a_builder.Fail("its samplers could not be made");
					return nullptr;
				}

				constexpr UINT uav = D3D11_BIND_UNORDERED_ACCESS;
				const auto threshold = a_builder.CreateImage("PostProcessing::LensFlare::Threshold", kColorFormat, uav, run->halfWidth, run->halfHeight);
				const auto ghostHalo = a_builder.CreateImage("PostProcessing::LensFlare::GhostHalo", kColorFormat, uav, run->halfWidth, run->halfHeight);
				run->thresholdConstants = a_builder.Latch<CB>();
				a_builder.Dispatch({ .name = "threshold",
					.program = programs.threshold,
					.bindings = FlareBindings(Bind::Srv(a_builder.Current()), Bind::Uav(threshold), run->thresholdConstants),
					.groups = { Tiles(run->halfWidth), Tiles(run->halfHeight), 1 } });

				if (shape.fft)
					BuildFFT(a_builder, programs, *run, threshold, ghostHalo);
				else
					BuildFast(a_builder, programs, *run, threshold, ghostHalo);

				a_builder.Publish(Published::kLensFlareOutput, ghostHalo);
				if (a_builder.Failed())
					return nullptr;
				return run;
			}

		private:
			/** @brief The programs one build's passes use. Those of the other path stay null. */
			struct FlarePrograms
			{
				std::shared_ptr<const ComputeProgram> threshold;
				// Fast path. Down and up only with blur passes.
				std::shared_ptr<const ComputeProgram> ghostHalo, down, up;
				// FFT path.
				std::shared_ptr<const ComputeProgram> fftThreshold, rowForward, colForward, rowInverse, colInverse, multiply, prepare, compose, composeFirst;
			};

			/** @brief The build's passes from its key (2 words) and this frame's Update. */
			PassShape ShapeFor(std::span<const std::uint64_t> a_key) const
			{
				PassShape shape;
				const int mode = static_cast<int>(static_cast<std::uint32_t>(a_key[0]));
				shape.fft = FFTMode(mode);
				if (shape.fft) {
					shape.fftSize = static_cast<std::uint32_t>(a_key[1]);
					// Quality has one group. Ultra's count is observed by Update.
					shape.groups = static_cast<LensFlare::GhostMode>(mode) == LensFlare::GhostMode::Ultra ? observedGroups : 1u;
				} else {
					shape.blurPasses = observedBlurPasses;
				}
				return shape;
			}

			/** @brief Listed in Programs and built on the programs lane, for the path a_shape takes. */
			static FlarePrograms LoadPrograms(ProgramSource& a_programs, const PassShape& a_shape)
			{
				FlarePrograms programs;
				programs.threshold = a_programs.Compute(kShader, ShaderBindings::Words<FlareBlock>(), {}, L"CSThreshold");
				if (!a_shape.fft) {
					constexpr std::uint32_t words = ShaderBindings::Words<FlareBlock>();
					programs.ghostHalo = a_programs.Compute(kShader, words, {}, L"CSGhostHalo");
					if (a_shape.blurPasses > 0) {
						programs.down = a_programs.Compute(kShader, words, {}, L"CSFlareDown");
						programs.up = a_programs.Compute(kShader, words, {}, L"CSFlareUp");
					}
					return programs;
				}
				const std::uint32_t n = a_shape.fftSize;
				constexpr std::uint32_t words = ShaderBindings::Words<FFTBlock>();
				const auto program = [&](const wchar_t* a_entry, std::vector<std::pair<std::string, std::string>> a_defines = {}) {
					return a_programs.Compute(kFFTShader, words, std::move(a_defines), a_entry);
				};
				const auto transform = [&](const char* a_axis, const char* a_direction) {
					return program(L"CS_FFT", { { a_axis, "" }, { a_direction, "" }, { "FFT_SIZE", std::to_string(n) } });
				};
				programs.rowForward = transform("ROW_PASS", "FORWARD");
				programs.colForward = transform("COL_PASS", "FORWARD");
				programs.rowInverse = transform("ROW_PASS", "INVERSE");
				programs.colInverse = transform("COL_PASS", "INVERSE");
				programs.multiply = program(L"CS_Multiply");
				programs.prepare = program(L"CSBokehPrepare");
				programs.compose = program(L"CSFFTGhostCompose");
				programs.composeFirst = program(L"CSFFTGhostCompose", { { "FIRST_GROUP", "" } });
				programs.fftThreshold = program(L"CSFFTThreshold");
				return programs;
			}

			/** @brief The kernel groups the live ghosts ask for, at least one and at most MAX_KERNEL_GROUPS. */
			static std::uint32_t LiveGroups(const LensFlare& a_flare)
			{
				// The size does not change the ghost mask.
				const auto groups = a_flare.KernelGroups(a_flare.BuildConstants(1, 1).ActiveGhostMask).size();
				return static_cast<std::uint32_t>(std::clamp<std::size_t>(groups, 1, LensFlare::MAX_KERNEL_GROUPS));
			}

			/** @brief The Fast path's Kawase down and up pairs. */
			static std::uint32_t LiveBlurPasses(const LensFlare& a_flare)
			{
				if (a_flare.debugsettings.disableBlur)
					return 0;
				// The debug slider's range.
				return static_cast<std::uint32_t>(std::clamp(a_flare.debugsettings.blurIterations, 0, 4));
			}

			/** @brief Linear, clamped or with a transparent black border. */
			bool MakeSamplers()
			{
				if (!colorSampler)
					colorSampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp);
				if (!borderSampler)
					borderSampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearBorder);
				return colorSampler && borderSampler;
			}

			std::vector<Bind> FlareBindings(Bind a_input, Bind a_output, const LatchRegion<CB>& a_constants) const
			{
				return { Bind::Shared(), std::move(a_input), Bind::Handle(colorSampler.Index()), Bind::Handle(borderSampler.Index()), std::move(a_output), Bind::Latch(a_constants) };
			}

			/** @brief Entries left out are unused by that entry point. */
			std::vector<Bind> FFTBindings(const LatchRegion<CB>& a_constants, Bind a_input, Bind a_complexA, Bind a_complexB, Bind a_complexOut,
				Bind a_colorOut) const
			{
				return { std::move(a_input), Bind::Unused(), Bind::Handle(colorSampler.Index()), Bind::Handle(borderSampler.Index()), std::move(a_complexA),
					std::move(a_complexB), std::move(a_complexOut), std::move(a_colorOut), Bind::Latch(a_constants) };
			}

			/** @brief DrawFast: ghosts and halo, then Kawase blurs through a quarter-resolution image. */
			void BuildFast(Builder& a_builder, const FlarePrograms& a_programs, LensFlareRun& a_run, const Image& a_threshold, const Image& a_ghostHalo)
			{
				a_run.ghostConstants = a_builder.Latch<CB>();
				a_run.downConstants = a_builder.Latch<CB>();
				a_run.upConstants = a_builder.Latch<CB>();
				a_builder.Dispatch({ .name = "ghost-halo",
					.program = a_programs.ghostHalo,
					.bindings = FlareBindings(Bind::Srv(a_threshold), Bind::Uav(a_ghostHalo), a_run.ghostConstants),
					.groups = { Tiles(a_run.halfWidth), Tiles(a_run.halfHeight), 1 } });
				if (a_run.shape.blurPasses == 0)
					return;
				const auto blur = a_builder.CreateImage("PostProcessing::LensFlare::BlurTemp", kColorFormat, D3D11_BIND_UNORDERED_ACCESS, a_run.quarterWidth, a_run.quarterHeight);
				const auto& down = a_programs.down;
				const auto& up = a_programs.up;
				for (std::uint32_t pass = 0; pass < a_run.shape.blurPasses; ++pass) {
					a_builder.Dispatch({ .name = std::format("blur-down-{}", pass),
						.program = down,
						.bindings = FlareBindings(Bind::Srv(a_ghostHalo), Bind::Uav(blur), a_run.downConstants),
						.groups = { Tiles(a_run.quarterWidth), Tiles(a_run.quarterHeight), 1 } });
					a_builder.Dispatch({ .name = std::format("blur-up-{}", pass),
						.program = up,
						.bindings = FlareBindings(Bind::Srv(blur), Bind::Uav(a_ghostHalo), a_run.upConstants),
						.groups = { Tiles(a_run.halfWidth), Tiles(a_run.halfHeight), 1 } });
				}
			}

			/** @brief DrawQuality: the scene's spectrum once, then per kernel group the convolution and compose. */
			void BuildFFT(Builder& a_builder, const FlarePrograms& a_programs, LensFlareRun& a_run, const Image& a_threshold, const Image& a_ghostHalo)
			{
				const std::uint32_t n = a_run.shape.fftSize;
				const auto& rowForward = a_programs.rowForward;
				const auto& colForward = a_programs.colForward;
				const auto& rowInverse = a_programs.rowInverse;
				const auto& colInverse = a_programs.colInverse;
				const auto& multiply = a_programs.multiply;
				const auto& prepare = a_programs.prepare;
				const auto& compose = a_programs.compose;
				const auto& composeFirst = a_programs.composeFirst;

				// Per frame, so chain images.
				constexpr UINT uav = D3D11_BIND_UNORDERED_ACCESS;
				const auto fftA = a_builder.CreateImage("PostProcessing::LensFlare::FFT0", kComplexFormat, uav, n, n);
				const auto fftB = a_builder.CreateImage("PostProcessing::LensFlare::FFT1", kComplexFormat, uav, n, n);
				const auto scene = a_builder.CreateImage("PostProcessing::LensFlare::SceneFFT", kComplexFormat, uav, n, n);

				auto& kernels = a_run.kernels;
				kernels.resize(a_run.shape.groups);
				for (std::size_t i = 0; i < kernels.size(); ++i) {
					auto& kernel = kernels[i];
					kernel.spectrum = &spectra[i];
					if (!*kernel.spectrum || !**kernel.spectrum) {
						a_builder.Fail("its kernel spectra could not be created");
						return;
					}
					a_builder.Register(**kernel.spectrum);
					kernel.constants = a_builder.Latch<CB>();
					kernel.prepareGate = a_builder.Gate();
					kernel.fftGate = a_builder.Gate();
				}
				if (kernels.empty()) {
					a_builder.Fail("it has no kernel group");
					return;
				}

				const auto none = Bind::Unused();
				const std::array<std::uint32_t, 3> tiles{ Tiles(n), Tiles(n), 1 };
				const std::array<std::uint32_t, 3> lines{ n, 1, 1 };
				const auto& first = kernels.front().constants;
				a_builder.Dispatch({ .name = "fft-threshold", .program = a_programs.fftThreshold,
					.bindings = FFTBindings(first, Bind::Srv(a_threshold), none, none, Bind::Uav(fftA), none), .groups = tiles });
				a_builder.Dispatch({ .name = "scene-rows", .program = rowForward,
					.bindings = FFTBindings(first, none, Bind::Srv(fftA), none, Bind::Uav(fftB), none), .groups = lines });
				a_builder.Dispatch({ .name = "scene-columns", .program = colForward,
					.bindings = FFTBindings(first, none, Bind::Srv(fftB), none, Bind::Uav(scene), none), .groups = lines });

				for (std::size_t i = 0; i < kernels.size(); ++i) {
					const auto& kernel = kernels[i];
					const auto& spectrum = **kernel.spectrum;
					const auto step = [i](const char* a_name) { return std::format("group{}-{}", i, a_name); };
					// Group counts are zero except on the frames the kernel is baked.
					a_builder.Dispatch({ .name = step("bokeh"), .program = prepare,
						.bindings = FFTBindings(kernel.constants, none, none, none, Bind::Uav(fftA), none), .gate = kernel.prepareGate });
					a_builder.Dispatch({ .name = step("bokeh-rows"), .program = rowForward,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftA), none, Bind::Uav(fftB), none), .gate = kernel.fftGate });
					a_builder.Dispatch({ .name = step("bokeh-columns"), .program = colForward,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftB), none, Bind::Uav(spectrum), none), .gate = kernel.fftGate });
					a_builder.Dispatch({ .name = step("multiply"), .program = multiply,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(scene), Bind::Srv(spectrum), Bind::Uav(fftB), none), .groups = tiles });
					a_builder.Dispatch({ .name = step("inverse-rows"), .program = rowInverse,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftB), none, Bind::Uav(fftA), none), .groups = lines });
					a_builder.Dispatch({ .name = step("inverse-columns"), .program = colInverse,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftA), none, Bind::Uav(fftB), none), .groups = lines });
					// The first group writes the image, the others add to it.
					a_builder.Dispatch({ .name = step("compose"), .program = i == 0 ? composeFirst : compose,
						.bindings = FFTBindings(kernel.constants, Bind::Srv(fftB), none, none, none, Bind::Uav(a_ghostHalo)),
						.groups = { Tiles(a_run.halfWidth), Tiles(a_run.halfHeight), 1 } });
				}
			}

			org::OwnedDescriptorBinding colorSampler, borderSampler;
			// Declared once. A build uses the first of them, one per kernel group.
			std::array<RenderGraph::Frame::DeclaredTexture2D, LensFlare::MAX_KERNEL_GROUPS> spectra;
			// This frame's, from Update.
			std::uint32_t observedGroups = 1;
			std::uint32_t observedBlurPasses = 0;
			bool observedDebugOff = false;  // Disable Threshold or Disable Ghosts, from Update
		};
	}

	std::unique_ptr<Effect> MakeLensFlare() { return std::make_unique<LensFlareEffect>(); }
}

