#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/LensFlare.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <algorithm>
#	include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/LensFlare/lensflare.cs.hlsl";
		constexpr const char* kFFTShader = "PostProcessing/LensFlare/lensflare_fft.cs.hlsl";
		constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // texThreshold's, texGhostHalo's, texBlurTemp's
		constexpr DXGI_FORMAT kComplexFormat = DXGI_FORMAT_R32G32_FLOAT;     // the FFT textures'
		// A kernel is baked in this many frames after it changes. The epoch run right after a rebuild may still be the old
		// build's, so one frame is not enough; the bake is deterministic, so baking again is harmless.
		constexpr std::uint32_t kBakeFrames = 3;

		using CB = LensFlare::LensFlareCB;
		using Groups = std::array<std::uint32_t, 4>;

		// lensflare.cs.hlsl's push block: LENS_FLARE_RESOURCES, then the LensFlareFrame latch.
		struct FlareBlock
		{
			ShaderBindings::Handle inputTexture, colorSampler, borderSampler, outputTexture;
			ShaderBindings::Latch frame;
		};

		// lensflare_fft.cs.hlsl's push block: LENS_FLARE_FFT_RESOURCES, then the LensFlareFrame latch.
		struct FFTBlock
		{
			ShaderBindings::Handle inputTexture, flareTexture, bokehSampler, borderSampler, texComplexA, texComplexB, rwTexComplex, rwTexColor;
			ShaderBindings::Latch frame;
		};

		constexpr std::uint32_t Tiles(std::uint32_t a_size) { return (a_size + 7) >> 3; }

		/** @brief What a baked bokeh kernel depends on, as LensFlare::PrepareBokehFFT compares it. */
		struct KernelKey
		{
			float kernelScale = 0.f;
			float apertureSize = 0.f;
			float apertureRotation = 0.f;
			int apertureBlades = 0;

			bool operator==(const KernelKey&) const = default;
		};

		/** @brief A kernel group's persistent bokeh kernel spectrum and when it is baked. */
		struct Kernel
		{
			std::unique_ptr<GraphResources::Texture2D> spectrum;
			KernelKey key;
			std::uint32_t bakeFrames = 0;
			LatchRegion<CB> constants;
			LatchRegion<Groups> prepareGroups, fftGroups;
		};

		/**
		 * @brief LensFlare::Draw on the graph. The threshold pass, then either the Fast path (ghosts and halo, then the Kawase
		 * blur) or the FFT path (Quality, Ultra): the thresholded scene's spectrum, then per kernel group the bokeh kernel's
		 * spectrum, the product, the inverse transform and the ghost compose, added into the ghosts-and-halo image. That image
		 * is published for Composite (kLensFlareOutput); the main image is left alone, as on D3D11.
		 *
		 * Every pass has its own latch copy of LensFlareCB, since the D3D11 path changes the per-pass sizes, the kernel scale,
		 * the ghost mask and the halo strength between dispatches. What sets the passes (mode, FFT size, kernel group count,
		 * blur passes) is the key; the settings and the camera's aperture are in the latch. The kernel spectra are persistent
		 * and owned here, one per group; their bake is an indirect dispatch that runs only after a kernel's inputs change.
		 * Every other image is a transient chain image.
		 */
		class LensFlareEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& flare = static_cast<LensFlare&>(a_feature);
				// These leave the D3D11 textures as the last frame wrote them, which a transient chain image cannot do.
				if (flare.debugsettings.disableThreshold || flare.debugsettings.disableGhosts)
					return "a debug switch (Disable Threshold or Disable Ghosts) is on";
				const auto wanted = ShapeOf(flare);
				a_key.push_back(static_cast<std::uint64_t>(wanted.fft));
				a_key.push_back(wanted.fftSize);
				a_key.push_back(wanted.groups);
				a_key.push_back(wanted.blurPasses);
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature& a_feature) override
			{
				auto& flare = static_cast<LensFlare&>(a_feature);
				shape = ShapeOf(flare);
				fullWidth = a_builder.GetShape().width;
				fullHeight = a_builder.GetShape().height;
				halfWidth = (std::max)(fullWidth / 2, 1u);
				halfHeight = (std::max)(fullHeight / 2, 1u);
				quarterWidth = (std::max)(fullWidth / 4, 1u);
				quarterHeight = (std::max)(fullHeight / 4, 1u);

				if (!MakeSamplers()) {
					a_builder.Fail("its samplers could not be made");
					return;
				}

				constexpr UINT uav = D3D11_BIND_UNORDERED_ACCESS;
				const auto threshold = a_builder.CreateImage("PostProcessing::LensFlare::Threshold", kColorFormat, uav, halfWidth, halfHeight);
				const auto ghostHalo = a_builder.CreateImage("PostProcessing::LensFlare::GhostHalo", kColorFormat, uav, halfWidth, halfHeight);
				thresholdConstants = a_builder.Latch<CB>();
				a_builder.Dispatch({ .name = "threshold",
					.program = a_builder.Compute(kShader, ShaderBindings::Words<FlareBlock>(), {}, L"CSThreshold"),
					.bindings = FlareBindings(Bind::Srv(a_builder.Current()), Bind::Uav(threshold), thresholdConstants),
					.groups = { Tiles(halfWidth), Tiles(halfHeight), 1 } });

				if (shape.fft)
					BuildFFT(a_builder, threshold, ghostHalo);
				else
					BuildFast(a_builder, threshold, ghostHalo);

				a_builder.Publish(kLensFlareOutput, ghostHalo);
			}

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

				// As DrawQuality: each group's kernel scale and ghosts, the halo in the first group only.
				const auto groups = flare.KernelGroups(data.ActiveGhostMask);
				const std::uint32_t n = shape.fftSize;
				for (std::size_t i = 0; i < kernels.size(); ++i) {
					auto& kernel = kernels[i];
					CB group = Sized(data, halfWidth, halfHeight, n, n);
					group.FFTResolution = n;
					// The key holds the group count, so groups has an entry for every kernel.
					if (i < groups.size()) {
						group.KernelScale = groups[i].kernelScale;
						group.ActiveGhostMask = groups[i].ghostMask;
					} else {
						group.ActiveGhostMask = 0;
					}
					if (i > 0)
						group.HaloStrength = 0.f;
					a_commit.Write(kernel.constants, group);

					const KernelKey key{ group.KernelScale, group.ApertureSize, group.ApertureRotation, group.ApertureBlades };
					if (key != kernel.key) {
						kernel.key = key;
						kernel.bakeFrames = kBakeFrames;
					}
					const bool bake = kernel.bakeFrames > 0;
					if (bake)
						--kernel.bakeFrames;
					a_commit.Write(kernel.prepareGroups, bake ? Groups{ Tiles(n), Tiles(n), 1, 0 } : Groups{});
					a_commit.Write(kernel.fftGroups, bake ? Groups{ n, 1, 1, 0 } : Groups{});
				}
			}

		private:
			/** @brief What the passes are built from: the key. */
			struct PassShape
			{
				bool fft = false;
				std::uint32_t fftSize = 0;     // FFT path: N
				std::uint32_t groups = 0;      // FFT path: kernel groups
				std::uint32_t blurPasses = 0;  // Fast path: Kawase down and up pairs
			};

			static PassShape ShapeOf(const LensFlare& a_flare)
			{
				PassShape result;
				const auto mode = static_cast<LensFlare::GhostMode>(a_flare.settings.GhostModeInt);
				result.fft = mode == LensFlare::GhostMode::Quality || mode == LensFlare::GhostMode::Ultra;
				if (result.fft) {
					result.fftSize = LensFlare::NormaliseFFTResolution(a_flare.settings.FFTResolution);
					// The size does not change the ghost mask.
					result.groups = static_cast<std::uint32_t>(a_flare.KernelGroups(a_flare.BuildConstants(1, 1).ActiveGhostMask).size());
				} else if (!a_flare.debugsettings.disableBlur) {
					// The debug slider's range; a larger value read from elsewhere is held there.
					result.blurPasses = static_cast<std::uint32_t>(std::clamp(a_flare.debugsettings.blurIterations, 0, 4));
				}
				return result;
			}

			/** @brief a_data with one pass's output and input sizes, as the D3D11 path sets them before a dispatch. */
			static CB Sized(CB a_data, std::uint32_t a_outWidth, std::uint32_t a_outHeight, std::uint32_t a_inWidth, std::uint32_t a_inHeight)
			{
				a_data.OutputWidth = static_cast<float>(a_outWidth);
				a_data.OutputHeight = static_cast<float>(a_outHeight);
				a_data.InputWidth = static_cast<float>(a_inWidth);
				a_data.InputHeight = static_cast<float>(a_inHeight);
				return a_data;
			}

			/** @brief LensFlare::colorSampler and borderSampler: linear, clamped or with a transparent black border. */
			bool MakeSamplers()
			{
				if (!colorSampler)
					colorSampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{});
				if (!borderSampler) {
					rhi::SamplerDesc border{};
					border.addressU = border.addressV = border.addressW = rhi::AddressMode::Border;
					border.borderPreset = rhi::BorderPreset::TransparentBlack;
					borderSampler = ShaderBindings::CreateSampler(border);
				}
				return colorSampler && borderSampler;
			}

			std::vector<Bind> FlareBindings(Bind a_input, Bind a_output, const LatchRegion<CB>& a_constants) const
			{
				return { std::move(a_input), Bind::Handle(colorSampler.Index()), Bind::Handle(borderSampler.Index()), std::move(a_output), Bind::Latch(a_constants) };
			}

			/** @brief An FFT-shader pass's bindings; the entries a_* leave out are unused by that entry point. */
			std::vector<Bind> FFTBindings(const LatchRegion<CB>& a_constants, Bind a_input, Bind a_complexA, Bind a_complexB, Bind a_complexOut,
				Bind a_colorOut) const
			{
				return { std::move(a_input), Bind::Unused(), Bind::Handle(colorSampler.Index()), Bind::Handle(borderSampler.Index()), std::move(a_complexA),
					std::move(a_complexB), std::move(a_complexOut), std::move(a_colorOut), Bind::Latch(a_constants) };
			}

			/** @brief DrawFast: ghosts and halo from the threshold, then the Kawase blur passes through a quarter-resolution image. */
			void BuildFast(Builder& a_builder, const Image& a_threshold, const Image& a_ghostHalo)
			{
				kernels.clear();
				constexpr std::uint32_t words = ShaderBindings::Words<FlareBlock>();
				ghostConstants = a_builder.Latch<CB>();
				downConstants = a_builder.Latch<CB>();
				upConstants = a_builder.Latch<CB>();
				a_builder.Dispatch({ .name = "ghost-halo",
					.program = a_builder.Compute(kShader, words, {}, L"CSGhostHalo"),
					.bindings = FlareBindings(Bind::Srv(a_threshold), Bind::Uav(a_ghostHalo), ghostConstants),
					.groups = { Tiles(halfWidth), Tiles(halfHeight), 1 } });
				if (shape.blurPasses == 0)
					return;
				const auto blur = a_builder.CreateImage("PostProcessing::LensFlare::BlurTemp", kColorFormat, D3D11_BIND_UNORDERED_ACCESS, quarterWidth, quarterHeight);
				const auto down = a_builder.Compute(kShader, words, {}, L"CSFlareDown");
				const auto up = a_builder.Compute(kShader, words, {}, L"CSFlareUp");
				for (std::uint32_t pass = 0; pass < shape.blurPasses; ++pass) {
					a_builder.Dispatch({ .name = std::format("blur-down-{}", pass),
						.program = down,
						.bindings = FlareBindings(Bind::Srv(a_ghostHalo), Bind::Uav(blur), downConstants),
						.groups = { Tiles(quarterWidth), Tiles(quarterHeight), 1 } });
					a_builder.Dispatch({ .name = std::format("blur-up-{}", pass),
						.program = up,
						.bindings = FlareBindings(Bind::Srv(blur), Bind::Uav(a_ghostHalo), upConstants),
						.groups = { Tiles(halfWidth), Tiles(halfHeight), 1 } });
				}
			}

			/** @brief DrawQuality: the scene's spectrum once, then per kernel group the convolution and the compose. */
			void BuildFFT(Builder& a_builder, const Image& a_threshold, const Image& a_ghostHalo)
			{
				const std::uint32_t n = shape.fftSize;
				constexpr std::uint32_t words = ShaderBindings::Words<FFTBlock>();
				const auto program = [&](const wchar_t* a_entry, std::vector<std::pair<std::string, std::string>> a_defines = {}) {
					return a_builder.Compute(kFFTShader, words, std::move(a_defines), a_entry);
				};
				const auto transform = [&](const char* a_axis, const char* a_direction) {
					return program(L"CS_FFT", { { a_axis, "" }, { a_direction, "" }, { "FFT_SIZE", std::to_string(n) } });
				};
				const auto rowForward = transform("ROW_PASS", "FORWARD");
				const auto colForward = transform("COL_PASS", "FORWARD");
				const auto rowInverse = transform("ROW_PASS", "INVERSE");
				const auto colInverse = transform("COL_PASS", "INVERSE");
				const auto multiply = program(L"CS_Multiply");
				const auto prepare = program(L"CSBokehPrepare");
				const auto compose = program(L"CSFFTGhostCompose");
				const auto composeFirst = program(L"CSFFTGhostCompose", { { "FIRST_GROUP", "" } });

				// The FFT ping-pong pair and the scene's spectrum: per frame, so chain images.
				constexpr UINT uav = D3D11_BIND_UNORDERED_ACCESS;
				const auto fftA = a_builder.CreateImage("PostProcessing::LensFlare::FFT0", kComplexFormat, uav, n, n);
				const auto fftB = a_builder.CreateImage("PostProcessing::LensFlare::FFT1", kComplexFormat, uav, n, n);
				const auto scene = a_builder.CreateImage("PostProcessing::LensFlare::SceneFFT", kComplexFormat, uav, n, n);

				if (!MakeKernels(n)) {
					a_builder.Fail("its kernel spectra could not be created");
					return;
				}
				for (auto& kernel : kernels) {
					a_builder.Register(*kernel.spectrum);
					kernel.constants = a_builder.Latch<CB>();
					kernel.prepareGroups = a_builder.Latch<Groups>();
					kernel.fftGroups = a_builder.Latch<Groups>();
					// A new build may have new programs: bake again.
					kernel.bakeFrames = kBakeFrames;
				}

				const auto none = Bind::Unused();
				const std::array<std::uint32_t, 3> tiles{ Tiles(n), Tiles(n), 1 };
				const std::array<std::uint32_t, 3> lines{ n, 1, 1 };
				const auto& first = kernels.front().constants;
				a_builder.Dispatch({ .name = "fft-threshold", .program = program(L"CSFFTThreshold"),
					.bindings = FFTBindings(first, Bind::Srv(a_threshold), none, none, Bind::Uav(fftA), none), .groups = tiles });
				a_builder.Dispatch({ .name = "scene-rows", .program = rowForward,
					.bindings = FFTBindings(first, none, Bind::Srv(fftA), none, Bind::Uav(fftB), none), .groups = lines });
				a_builder.Dispatch({ .name = "scene-columns", .program = colForward,
					.bindings = FFTBindings(first, none, Bind::Srv(fftB), none, Bind::Uav(scene), none), .groups = lines });

				for (std::size_t i = 0; i < kernels.size(); ++i) {
					const auto& kernel = kernels[i];
					const auto spectrum = kernel.spectrum->Graph();
					const auto step = [i](const char* a_name) { return std::format("group{}-{}", i, a_name); };
					// PrepareBokehFFT: only on the frames the kernel is baked (the group counts are zero otherwise).
					a_builder.Dispatch({ .name = step("bokeh"), .program = prepare,
						.bindings = FFTBindings(kernel.constants, none, none, none, Bind::Uav(fftA), none), .indirect = kernel.prepareGroups });
					a_builder.Dispatch({ .name = step("bokeh-rows"), .program = rowForward,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftA), none, Bind::Uav(fftB), none), .indirect = kernel.fftGroups });
					a_builder.Dispatch({ .name = step("bokeh-columns"), .program = colForward,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftB), none, Bind::Uav(spectrum), none), .indirect = kernel.fftGroups });
					a_builder.Dispatch({ .name = step("multiply"), .program = multiply,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(scene), Bind::Srv(spectrum), Bind::Uav(fftB), none), .groups = tiles });
					a_builder.Dispatch({ .name = step("inverse-rows"), .program = rowInverse,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftB), none, Bind::Uav(fftA), none), .groups = lines });
					a_builder.Dispatch({ .name = step("inverse-columns"), .program = colInverse,
						.bindings = FFTBindings(kernel.constants, none, Bind::Srv(fftA), none, Bind::Uav(fftB), none), .groups = lines });
					// The first group writes the image, the others add to it.
					a_builder.Dispatch({ .name = step("compose"), .program = i == 0 ? composeFirst : compose,
						.bindings = FFTBindings(kernel.constants, Bind::Srv(fftB), none, none, none, Bind::Uav(a_ghostHalo)),
						.groups = { Tiles(halfWidth), Tiles(halfHeight), 1 } });
				}
			}

			/** @brief One persistent kernel spectrum per group, N x N, made again when N changes. */
			bool MakeKernels(std::uint32_t a_size)
			{
				kernels.resize(shape.groups);
				for (std::size_t i = 0; i < kernels.size(); ++i) {
					auto& kernel = kernels[i];
					if (kernel.spectrum && kernel.spectrum->Desc().Width == a_size)
						continue;
					D3D11_TEXTURE2D_DESC desc{};
					desc.Width = desc.Height = a_size;
					desc.MipLevels = 1;
					desc.ArraySize = 1;
					desc.Format = kComplexFormat;
					desc.SampleDesc = { 1, 0 };
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					kernel.spectrum = std::make_unique<GraphResources::Texture2D>(desc, std::format("PostProcessing::LensFlare::BokehFFT{}", i),
						GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
					kernel.key = {};
					if (!*kernel.spectrum)
						return false;
				}
				return !kernels.empty();
			}

			PassShape shape;
			std::uint32_t fullWidth = 1, fullHeight = 1, halfWidth = 1, halfHeight = 1, quarterWidth = 1, quarterHeight = 1;
			org::OwnedDescriptorBinding colorSampler, borderSampler;
			LatchRegion<CB> thresholdConstants, ghostConstants, downConstants, upConstants;
			std::vector<Kernel> kernels;
		};
	}

	std::unique_ptr<Effect> MakeLensFlare() { return std::make_unique<LensFlareEffect>(); }
}

#endif
