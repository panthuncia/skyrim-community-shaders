#pragma once

/**
 * The ORG post-processing chain: the framework each sub-effect's ORG port plugs into (CS_ORG_POST_PROCESSING=1).
 *
 * How the chain runs
 * ------------------
 * PostProcessing::PreProcess runs the sub-effects (PostProcessFeature) at the ImageSpace tonemap, each reading the previous
 * one's output and writing its own texture, then copies the last output into kMAIN and kMAIN_COPY. On the graph the same
 * chain is a list of passes at the Tonemap frame anchor (FrameAnchors::Anchor::Tonemap), built by ORGPostProcessing.cpp:
 *
 *   input   copy.ps: kMAIN or kMAIN_COPY (picked per frame through the latch) into "PostProcessing::Input", RGBA16F,
 *           with the game gamma removed unless Linear Lighting is on (BeginLinearProcessing)
 *   effects each enabled sub-effect, in PostProcessing::ChainOrder, through its Effect below
 *   output  the last image into kMAIN and into kMAIN_COPY: a copy, or copy.ps when the gamut or gamma changes or the
 *           formats differ (PostProcessing::OutputConversion)
 *
 * Every chain image is a transient GraphResources::Texture2D, the size of kMAIN unless the effect names one (a half-resolution
 * or FFT image), so the graph aliases them: there is no ping-pong to write by hand. A sub-feature's D3D11 textures are made only
 * when the D3D11 path first draws it (PostProcessing::DrawFeature), so an effect must not read them; files the sub-feature
 * loads itself (LUT::UpdateTexture) are fine.
 *
 * The mixed rule
 * --------------
 * The chain runs on the graph only while every sub-effect ChainOrder lists has an Effect here and none reports a reason not
 * to run (Effect::Update). Otherwise the whole chain runs on D3D11 that frame, and the log names the sub-effects that keep
 * it there whenever that list changes ("[ORG] Post Processing: the chain runs on D3D11: ..."). A sub-effect that is off
 * costs nothing on either path. There is no mixed frame: a graph image is never handed to a D3D11 sub-effect.
 *
 * Rules for porting a sub-effect
 * ------------------------------
 * 1. Convert its shaders to dual-target first, one refactor commit each (OrgResources.hlsli; prove them with
 *    tools/verify-shader-refactor.ps1 and tools/check-dual-target-shaders.py, --stage/--entry for multi-entry files).
 *    Constants that change by frame are a LATCH entry; take the struct layout from the C++ CB the D3D11 path uploads.
 * 2. Move the D3D11 Draw's constant building into a method of the sub-feature (Vignette::Constants,
 *    ColorGrading::BuildConstants) and call it from both paths. Do not copy it.
 * 3. Write an Effect in src/Features/PostProcessing/ORG/ORG<Name>.cpp, declare its Make<Name>() below and add it to
 *    MakeEffect in ORGPostProcessing.cpp. Nothing else in the D3D11 sub-feature changes.
 * 4. Update (render thread, before the anchor, every frame the effect is in the chain): import what the passes read from
 *    D3D11 (FrameAnchors::D3D11Texture, EngineTarget, EngineDepth), push what the passes are built from into the key, and
 *    return a reason when the graph cannot run it as it is set now (a debug readback, a format it cannot import). A
 *    changed key rebuilds the whole chain, so never push a value that changes every frame: those go in the latch.
 * 5. Build (render thread, at a rebuild): read Builder::Current(), create the outputs with Builder::CreateImage, add the
 *    passes (Draw, Dispatch, Copy, or Add for a custom pass), reserve latch regions, then SetCurrent(output) when the
 *    effect replaces the image (WritesToMainTexture). An effect that only produces side data (bloom, exposure) leaves
 *    Current alone and Publishes its result by name, and its constants by PublishLatch; Composite Finds them under the
 *    names in ORGPostChain::Published. A mip chain is one CreateMipChain image, read and written a mip at a time through
 *    Image::Mip (the draw's viewport is then that mip's size); a published Image keeps its mip, so the reader's Bind::Srv
 *    sees only that mip. Programs come from Builder::Raster/Compute (RasterTargets for a target that blends), which cache
 *    them across builds (a first build blocks on the compile). A shader's SHARED entry is Bind::Shared:
 *    the chain then uploads CS's shared constants at the anchor (FrameBuffer, SharedData: frame count, depth params).
 * 6. Commit (in the epoch, every frame): write this frame's values into the regions Build reserved. Work that runs only
 *    on some frames (a LUT regenerated on a settings change) is an indirect dispatch whose group counts are in the latch
 *    (ComputeDispatch::indirect), so the recording stays the same.
 * 7. Resources that live across frames (history, adaptation, a baked LUT) belong to the Effect object, which lives for
 *    the session: create them as GraphResources with Lifetime::History or Persistent and Builder::Register them at each
 *    build. Everything per frame is a chain image.
 * 8. A D3D11-only side path stays D3D11 (Border::ClearMotionVectorsForFrameGen, ColorGrading's curve preview). If it
 *    needs the frame's image, make Update return a reason while it is active.
 * 9. Names: passes "cs.pp.<effect>.<step>" (Builder adds the prefix), images "PostProcessing::<Effect>::<Name>", as the
 *    D3D11 resources are named.
 *
 * Threads: Update and Build run on the render thread, Commit inside the epoch (render thread, before submission). The
 * passes' Prepare and Record run on the graph host's thread and read only what they were made with and the latch.
 *
 * Translation units that include this header include <rhi_interop_vulkan.h> first (volk before any Vulkan header).
 */

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Features/PostProcessing.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"

#	include <array>
#	include <atomic>
#	include <cstdint>
#	include <functional>
#	include <memory>
#	include <optional>
#	include <string>
#	include <string_view>
#	include <utility>
#	include <vector>

namespace org
{
	class LatchBlock;
	class RenderGraph;
	class RenderPass;
	class Resource;
}

namespace GraphResources
{
	class Resource;
}

namespace ORGPostChain
{
	/** @brief The latch bytes one build may reserve for every effect together, per frame slot. */
	inline constexpr std::uint32_t kLatchBytes = 16384;

	/** @brief An Image or Bind that covers every mip of its texture (the texture's default view). */
	inline constexpr std::uint32_t kAllMips = UINT32_MAX;

	/** @brief A texture of the chain: the graph resource and its format, or one mip of it (Mip). */
	struct Image
	{
		std::shared_ptr<org::Resource> graph;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		std::uint32_t mip = kAllMips;  // kAllMips: the whole texture; else a pass reads or writes only this mip

		explicit operator bool() const { return graph != nullptr; }

		/** @brief The same texture, one mip only: a draw targets that mip, a Bind::Srv reads a view of that mip alone. */
		Image Mip(std::uint32_t a_mip) const
		{
			Image image = *this;
			image.mip = a_mip;
			return image;
		}
	};

	/** @brief What a build is made for: kMAIN's size and the main targets' formats. */
	struct Shape
	{
		UINT width = 0, height = 0;  // kMAIN's size, which every chain image has
		float dynamicWidth = 0.0f;   // Util::ConvertToDynamic of it: the D3D11 passes' viewport size
		float dynamicHeight = 0.0f;
		DXGI_FORMAT mainFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT mainCopyFormat = DXGI_FORMAT_UNKNOWN;  // the format most D3D11 sub-effects give their output
	};

	/** @brief A region of the chain's latch block holding a T, written by Commit. */
	template <class T>
	struct LatchRegion
	{
		std::uint32_t offset = 0;
	};

	/**
	 * @brief The side outputs Composite reads, under the names their effects publish them (Builder::Publish, PublishLatch).
	 *
	 * An effect that is on publishes every entry of its group; Composite treats a group with an entry missing as that
	 * contributor off, as the D3D11 Composite treats a missing SRV (the effect off, or its output not ready). Composite reads
	 * each image through Bind::Srv, so publish an image that views the level named here. A latch entry is
	 * a region holding the C++ struct the D3D11 path uploads, written by the publisher's Commit.
	 */
	namespace Published
	{
		// CODBloom: the accumulated bloom, mip 1 of its chain (a published Image of that mip), which Composite upsamples.
		inline constexpr std::string_view kBloomOutput = "PostProcessing::CODBloom::Bloom";
		// LensFlare: the ghosts and halo, RGBA16F, at max(width / 2, 1) x max(height / 2, 1) of the chain's Shape
		// (LensFlare::texGhostHalo's size, Composite's FlareSize).
		inline constexpr std::string_view kLensFlareOutput = "PostProcessing::LensFlare::GhostHalo";
		// PhysicalGlare: the glare, the size of the chain's images.
		inline constexpr std::string_view kGlare = "PostProcessing::PhysicalGlare::Output";
		// HistogramAutoExposure: the adapted luminance (a structured buffer of one float) and a HistogramAutoExposure::AutoExposureCB.
		inline constexpr std::string_view kAdaptation = "PostProcessing::AutoExposure::Adaptation";
		inline constexpr std::string_view kAutoExposureConstants = "PostProcessing::AutoExposure::Constants";
		// LocalExposure: the scene's log luminance (mip 0), the luminance grid (3D), the blurred base and a LocalExposure::LocalExposureCB.
		inline constexpr std::string_view kLogLuminance = "PostProcessing::LocalExposure::LogLuminance";
		inline constexpr std::string_view kLuminanceGrid = "PostProcessing::LocalExposure::LuminanceGrid";
		inline constexpr std::string_view kBlurredLuminance = "PostProcessing::LocalExposure::BlurredLuminance";
		inline constexpr std::string_view kLocalExposureConstants = "PostProcessing::LocalExposure::Constants";
	}

	/**
	 * @brief One entry of a pass's push block, in the shader's resource list order (ShaderBindings.h): SRV and UAV entries
	 * are declared resources, Handle a sampler or another descriptor, Latch a LATCH entry's two words, Unused a handle the
	 * shader never reads on this pass (another entry point's resource, the LUT type not loaded), Shared a SHARED entry's four
	 * words (CS's shared constant buffers, SharedConstants.h; the chain's client then uploads them at the anchor).
	 */
	struct Bind
	{
		enum class Kind : std::uint8_t
		{
			Srv,
			Uav,
			Handle,
			Latch,
			Shared,
		};

		Kind kind = Kind::Handle;
		std::shared_ptr<org::Resource> resource;
		std::uint32_t value = 0;       // Handle: the descriptor index; Latch: the region's offset
		std::uint32_t mip = kAllMips;  // Srv and Uav: the one mip the view covers, or the default view

		static Bind Srv(std::shared_ptr<org::Resource> a_resource) { return { Kind::Srv, std::move(a_resource), 0 }; }
		static Bind Srv(const Image& a_image) { return { Kind::Srv, a_image.graph, 0, a_image.mip }; }
		static Bind Uav(std::shared_ptr<org::Resource> a_resource) { return { Kind::Uav, std::move(a_resource), 0 }; }
		static Bind Uav(const Image& a_image) { return { Kind::Uav, a_image.graph, 0, a_image.mip }; }
		static Bind Handle(std::uint32_t a_index) { return { Kind::Handle, nullptr, a_index }; }
		static Bind Unused() { return Handle(0); }
		static Bind Shared() { return { Kind::Shared, nullptr, 0 }; }
		template <class T>
		static Bind Latch(const LatchRegion<T>& a_region)
		{
			return { Kind::Latch, nullptr, a_region.offset };
		}
	};

	/** @brief A fullscreen raster pass: a_program's pixel stage over the viewport, into one target. */
	struct FullscreenDraw
	{
		std::string name;  // the step: the pass is "cs.pp.<effect>.<name>"
		std::shared_ptr<const RasterProgram> program;
		std::vector<Bind> bindings;
		Image target;                                     // or one mip of it (Image::Mip)
		std::optional<RasterProgram::Viewport> viewport;  // default: the whole target or its mip (Builder::MipViewport)
	};

	/** @brief A compute pass: fixed group counts, or (indirect) three uints at a latch offset that Commit writes. */
	struct ComputeDispatch
	{
		std::string name;
		std::shared_ptr<const ComputeProgram> program;
		std::vector<Bind> bindings;
		std::array<std::uint32_t, 3> groups{ 1, 1, 1 };
		std::optional<LatchRegion<std::array<std::uint32_t, 4>>> indirect;
		// Set when the pass is prepared to run (the chain on): the build is then in the graph, and every commit after the one
		// before that preparation reaches the GPU. For work an effect counts done only once it ran (a PSF regenerated on a change).
		std::shared_ptr<std::atomic<bool>> prepared;
	};

	struct BuildState;
	struct Programs;

	/** @brief What an effect's Build adds its passes and resources through. Render thread, during one rebuild. */
	class Builder
	{
	public:
		Builder(BuildState& a_state, Programs& a_programs, const Shape& a_shape, std::string_view a_effect);

		const Shape& GetShape() const { return shape; }

		/** @brief The image the effect reads: the last one an earlier stage wrote. */
		const Image& Current() const;

		/** @brief Makes a_image the input of the next stage (an effect that writes the main image). */
		void SetCurrent(const Image& a_image);

		/**
		 * @brief A transient image the size of kMAIN, aliasable, registered with the graph. a_bindFlags: D3D11_BIND_RENDER_TARGET
		 * for a raster output, D3D11_BIND_UNORDERED_ACCESS for a compute one (the SRV is always there). Null image, and the build
		 * fails, when it cannot be made.
		 */
		Image CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags);

		/**
		 * @brief As CreateImage, at a_width x a_height instead of kMAIN's size (a reduced-resolution or FFT image), with a_mips
		 * mips. A draw into one mip of an image that is not kMAIN's size gives its viewport (MipViewport assumes kMAIN's size).
		 */
		Image CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_width, UINT a_height, UINT a_mips = 1);

		/** @brief A kMAIN-sized image with a_mips mips, each read and written through Image::Mip (a bloom pyramid). */
		Image CreateMipChain(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_mips);

		/** @brief Registers an effect-owned resource (history, persistent) with the graph at this build. */
		void Register(const GraphResources::Resource& a_resource);

		/** @brief Publishes a side output (bloom, exposure) under a name for later effects; Find returns it, or a null image. */
		void Publish(std::string_view a_name, const Image& a_image);
		Image Find(std::string_view a_name) const;

		/**
		 * @brief Publishes a latch region (an effect's constants, which a later effect binds too) under a name. FindLatch returns it,
		 * or nothing when none was published; a region published for another type fails the build.
		 */
		template <class T>
		void PublishLatch(std::string_view a_name, const LatchRegion<T>& a_region)
		{
			PublishLatchBytes(a_name, a_region.offset, static_cast<std::uint32_t>(sizeof(T)));
		}
		template <class T>
		std::optional<LatchRegion<T>> FindLatch(std::string_view a_name)
		{
			const auto offset = FindLatchBytes(a_name, static_cast<std::uint32_t>(sizeof(T)));
			return offset ? std::optional<LatchRegion<T>>(LatchRegion<T>{ *offset }) : std::nullopt;
		}

		/** @brief Reserves a latch region for a T: 16-byte aligned, at most kLatchBytes for the whole build. */
		template <class T>
		LatchRegion<T> Latch()
		{
			static_assert(sizeof(T) % 4 == 0, "a latch region is whole 32-bit words");
			return { Reserve(static_cast<std::uint32_t>(sizeof(T))) };
		}

		/** @brief The build's latch block, for a custom pass (Add); the regions are slot-relative offsets into it. */
		std::shared_ptr<const org::LatchBlock> LatchBlock() const;

		/**
		 * @brief Programs, cached across builds by source, entry, defines, push words and targets. Common defines and the
		 * stage define are added as Util::CompileShader adds them. Null, and the build fails, when one does not build.
		 */
		std::shared_ptr<const RasterProgram> Raster(std::string_view a_source, std::uint32_t a_words, std::vector<DXGI_FORMAT> a_targets,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main");
		/** @brief As Raster, with each target's blending and write mask (RasterProgram::Target): an accumulating draw. */
		std::shared_ptr<const RasterProgram> RasterTargets(std::string_view a_source, std::uint32_t a_words, std::vector<RasterProgram::Target> a_targets,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main");
		std::shared_ptr<const ComputeProgram> Compute(std::string_view a_source, std::uint32_t a_words,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main");

		/** @brief The whole of kMAIN, and the dynamic-resolution part the D3D11 viewports cover (Shape::dynamicWidth). */
		RasterProgram::Viewport FullViewport() const;
		RasterProgram::Viewport DynamicViewport() const;
		/** @brief The whole of a_mip of a kMAIN-sized image (FullViewport for kAllMips), as a D3D11 viewport of width >> a_mip. */
		RasterProgram::Viewport MipViewport(std::uint32_t a_mip) const;

		/** @brief Adds a pass. Its push words must match the program's (logged, and the build fails, otherwise). */
		void Draw(FullscreenDraw a_draw);
		void Dispatch(ComputeDispatch a_dispatch);

		/** @brief A copy of a whole image into another of the same size and format (whole images: no Image::Mip). */
		void Copy(std::string_view a_name, const Image& a_source, const Image& a_target);

		/** @brief A custom pass: make is called at each graph build of the anchor, as FrameAnchors::PassDesc::make. */
		void Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make);

		/** @brief Marks the build failed (an effect that cannot build); the reason is logged. */
		void Fail(std::string_view a_reason);

		bool Failed() const;

	private:
		std::uint32_t Reserve(std::uint32_t a_bytes);
		std::string PassName(std::string_view a_step) const;
		void PublishLatchBytes(std::string_view a_name, std::uint32_t a_offset, std::uint32_t a_size);
		std::optional<std::uint32_t> FindLatchBytes(std::string_view a_name, std::uint32_t a_size);

		BuildState& state;
		Programs& programs;
		Shape shape;
		std::string effect;
	};

	/** @brief What Commit writes through: the build's latch at this execution's frame slot. */
	class CommitContext
	{
	public:
		CommitContext(const org::LatchBlock& a_latch, std::uint32_t a_slot) :
			latch(a_latch), slot(a_slot) {}

		template <class T>
		void Write(const LatchRegion<T>& a_region, const T& a_value) const
		{
			WriteBytes(a_region.offset, &a_value, sizeof(T));
		}

	private:
		void WriteBytes(std::uint32_t a_offset, const void* a_data, std::size_t a_size) const;

		const org::LatchBlock& latch;
		std::uint32_t slot;
	};

	/**
	 * @brief A sub-effect's ORG implementation. One object per pipeline slot for the session; it reads the D3D11 sub-feature
	 * (passed in, never kept: SyncPipelineResources may replace it) for its settings.
	 */
	class Effect
	{
	public:
		virtual ~Effect() = default;

		/**
		 * @brief Render thread, before the anchor, each frame the effect is in the chain: imports up to date, what the passes
		 * are built from appended to a_key. @return Null when the graph can run the effect this frame, else why not (the chain
		 * then runs on D3D11).
		 */
		virtual const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key)
		{
			(void)a_feature;
			(void)a_key;
			return nullptr;
		}

		/** @brief Render thread, at a rebuild: the effect's passes and resources. Fail the builder when it cannot. */
		virtual void Build(Builder& a_builder, PostProcessFeature& a_feature) = 0;

		/** @brief In the epoch, each frame: this frame's values into the regions Build reserved. */
		virtual void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) = 0;
	};

	// The ported sub-effects (src/Features/PostProcessing/ORG/). Add a port's factory here and in MakeEffect.
	std::unique_ptr<Effect> MakeVignette();
	std::unique_ptr<Effect> MakeColorGrading();
	std::unique_ptr<Effect> MakeLUT();
	std::unique_ptr<Effect> MakeBorder();
	std::unique_ptr<Effect> MakeLensFlare();
	std::unique_ptr<Effect> MakeCODBloom();
	std::unique_ptr<Effect> MakePhysicalGlare();
	std::unique_ptr<Effect> MakeDoF();
	std::unique_ptr<Effect> MakeCamera();
	std::unique_ptr<Effect> MakeComposite();
	std::unique_ptr<Effect> MakeMotionBlur();
	std::unique_ptr<Effect> MakeLocalExposure();
	std::unique_ptr<Effect> MakeHistogramAutoExposure();
}

#endif
