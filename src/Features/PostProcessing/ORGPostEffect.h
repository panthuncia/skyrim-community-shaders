#pragma once

/**
* The framework each Post Processing sub-effect's ORG port plugs into, with CS_ORG_POST_PROCESSING=1.
*
* At the Tonemap anchor the chain copies kMAIN into an input image, runs each enabled sub-effect in ChainOrder, and copies
* the last image back into kMAIN and kMAIN_COPY. Chain images are transient, so the graph aliases them.
* The chain runs on the graph only when every listed sub-effect has an Effect here and none reports a reason not to run.
* Otherwise the whole chain runs on D3D11 that frame. There is never a mixed frame.
*
* Per effect: Update runs on the render thread every frame and builds the key, Build runs at a rebuild, Commit writes the
* frame's values in the epoch. Never put a per-frame value in the key, since a changed key rebuilds the chain.
* Includers must include <rhi_interop_vulkan.h> first.
*/

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Features/PostProcessing.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"

#	include <Render/RunRef.h>

#	include <array>
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

namespace RenderGraph
{
	class RunGate;
}

namespace ORGPostChain
{
	/** @brief For every effect together, per run. */
	inline constexpr std::uint32_t kLatchBytes = 16384;

	/** @brief Covers every mip of its texture. */
	inline constexpr std::uint32_t kAllMips = RenderGraph::AnchorPasses::kAllMips;

	/** @brief The draws' entry, one per distinct fixed dispatch size, and the effects' own. */
	inline constexpr std::uint32_t kGateEntries = 128;

	struct Image
	{
		FrameAnchors::Texture graph;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		std::uint32_t mip = kAllMips;  // kAllMips for the whole texture

		explicit operator bool() const { return static_cast<bool>(graph); }

		/** @brief A draw targets that mip, and a Bind::Srv reads a view of that mip alone. */
		Image Mip(std::uint32_t a_mip) const
		{
			Image image = *this;
			image.mip = a_mip;
			return image;
		}
	};

	/** @brief kMAIN's size and the main targets' formats. */
	struct Shape
	{
		UINT width = 0, height = 0;  // every chain image has kMAIN's size
		float dynamicWidth = 0.0f;   // the D3D11 passes' viewport size
		float dynamicHeight = 0.0f;
		DXGI_FORMAT mainFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT mainCopyFormat = DXGI_FORMAT_UNKNOWN;  // the format most D3D11 sub-effects give their output
	};

	template <class T>
	struct LatchRegion
	{
		std::uint32_t offset = 0;
	};

	/**
	* @brief Side outputs Composite reads by name. Composite treats a group with a missing entry as that contributor being off,
	* as the D3D11 Composite treats a missing SRV.
	*/
	namespace Published
	{
		// Mip 1 of the bloom chain, which Composite upsamples.
		inline constexpr std::string_view kBloomOutput = "PostProcessing::CODBloom::Bloom";
		// RGBA16F at half the chain's size, at least 1x1.
		inline constexpr std::string_view kLensFlareOutput = "PostProcessing::LensFlare::GhostHalo";
		inline constexpr std::string_view kGlare = "PostProcessing::PhysicalGlare::Output";
		// A structured buffer of one float, plus a HistogramAutoExposure::AutoExposureCB.
		inline constexpr std::string_view kAdaptation = "PostProcessing::AutoExposure::Adaptation";
		inline constexpr std::string_view kAutoExposureConstants = "PostProcessing::AutoExposure::Constants";
		// Mip 0 log luminance, the 3D luminance grid, the blurred base and a LocalExposure::LocalExposureCB.
		inline constexpr std::string_view kLogLuminance = "PostProcessing::LocalExposure::LogLuminance";
		inline constexpr std::string_view kLuminanceGrid = "PostProcessing::LocalExposure::LuminanceGrid";
		inline constexpr std::string_view kBlurredLuminance = "PostProcessing::LocalExposure::BlurredLuminance";
		inline constexpr std::string_view kLocalExposureConstants = "PostProcessing::LocalExposure::Constants";
		// DoF's smoothed focus distance in km, R32F 1x1, written before Physical Lens runs.
		inline constexpr std::string_view kDoFFocus = "PostProcessing::DoF::Focus";
		// Physical Lens's image mapping (RGBA32F array of 3) and its PhysicalLensImaging::DiffractionParameters region.
		inline constexpr std::string_view kPhysicalLensMapping = "PostProcessing::PhysicalLens::ChromaticMapping";
		inline constexpr std::string_view kPhysicalLensDiffraction = "PostProcessing::PhysicalLens::DiffractionParameters";
		// Physical Lens's ghosts (R32F array of 3, half the chain's size) and its diffraction outputs (Composite's t10 to t14).
		inline constexpr std::string_view kPhysicalLensGhosts = "PostProcessing::PhysicalLens::Ghosts";
		inline constexpr std::string_view kPhysicalLensWaveKernel = "PostProcessing::PhysicalLens::DiffractionCore";
		inline constexpr std::string_view kPhysicalLensWaveRegions = "PostProcessing::PhysicalLens::Regions";
		inline constexpr std::string_view kPhysicalLensWaveSources = "PostProcessing::PhysicalLens::DiffractionSources";
		inline constexpr std::string_view kPhysicalLensWaveTiles = "PostProcessing::PhysicalLens::DiffractionTiles";
		inline constexpr std::string_view kPhysicalLensWaveTail = "PostProcessing::PhysicalLens::DiffractionRadiance";
	}

	/** @brief Dispatches through it run only on frames Commit opens it, with the group counts given there. */
	struct GateEntry
	{
		std::uint32_t index = 0;
	};

	/**
	* @brief Adds the chain's Image and LatchRegion forms. Unused is a handle the shader never reads on this pass.
	* Shared makes the client upload CS's shared constants at the anchor.
	*/
	struct Bind : RenderGraph::AnchorPasses::Bind
	{
		using Base = RenderGraph::AnchorPasses::Bind;

		static Bind Srv(const FrameAnchors::Texture& a_texture) { return { Base::Srv(a_texture) }; }
		static Bind Srv(const Image& a_image) { return { Base::Srv(a_image.graph, a_image.mip) }; }
		/** @brief The whole-array view, for a shader that declares Texture2DArray. */
		static Bind SrvArray(const Image& a_image) { return { Base::SrvArray(a_image.graph) }; }
		static Bind Uav(const FrameAnchors::Texture& a_texture) { return { Base::Uav(a_texture) }; }
		static Bind Uav(const Image& a_image) { return { Base::Uav(a_image.graph, a_image.mip) }; }
		static Bind Handle(std::uint32_t a_index) { return { Base::Handle(a_index) }; }
		static Bind Unused() { return { Base::Unused() }; }
		static Bind Shared() { return { Base::Shared() }; }
		template <class T>
		static Bind Latch(const LatchRegion<T>& a_region)
		{
			return { Base::Latch(a_region.offset) };
		}
	};

	struct FullscreenDraw
	{
		std::string name;  // the pass is "cs.pp.<effect>.<name>"
		std::shared_ptr<const RasterProgram> program;
		std::vector<Bind> bindings;
		Image target;                                     // or one mip of it
		std::optional<RasterProgram::Viewport> viewport;  // default is the whole target or its mip
	};

	/**
	* @brief Fixed group counts run every frame the chain runs. With gate it runs only on frames Commit opens.
	* once is a one-shot pass that runs at the anchor's next run whether the chain runs or not, so it reads no latch.
	*/
	struct ComputeDispatch
	{
		std::string name;
		std::shared_ptr<const ComputeProgram> program;
		std::vector<Bind> bindings;
		std::array<std::uint32_t, 3> groups{ 1, 1, 1 };
		std::optional<GateEntry> gate;
		bool once = false;
	};

	struct BuildState;
	struct Programs;

	/** @brief Render thread, during one rebuild. */
	class Builder
	{
	public:
		Builder(BuildState& a_state, Programs& a_programs, const Shape& a_shape, std::string_view a_effect);

		const Shape& GetShape() const { return shape; }

		/** @brief The last image an earlier stage wrote. */
		const Image& Current() const;

		/** @brief For an effect that writes the main image. */
		void SetCurrent(const Image& a_image);

		/** @brief A transient kMAIN-sized image. The SRV is always there. A null image fails the build. */
		Image CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags);

		/** @brief A draw into one mip of an image not at kMAIN's size must give its own viewport, since MipViewport assumes kMAIN's size. */
		Image CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_width, UINT a_height, UINT a_mips = 1);

		/** @brief Each mip is read and written through Image::Mip. */
		Image CreateMipChain(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_mips);

		/** @brief An effect-owned history or persistent resource. Call at every build. */
		void Register(const GraphResources::Resource& a_resource);
		/** @brief An effect-owned graph resource that is not a twin, such as a constant buffer, by name. Call at every build. */
		void Register(std::string_view a_name, std::shared_ptr<org::Resource> a_resource);

		/** @brief Find returns it, or a null image. */
		void Publish(std::string_view a_name, const Image& a_image);
		Image Find(std::string_view a_name) const;

		/** @brief FindLatch returns it. A region published for another type fails the build. */
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

		GateEntry Gate();

		/** @brief The chain's gate, for an effect's own passes (Add) that skip closed runs. */
		std::shared_ptr<const RenderGraph::RunGate> RunGate() const;

		/** @brief 16-byte aligned, at most kLatchBytes for the whole build. */
		template <class T>
		LatchRegion<T> Latch()
		{
			static_assert(sizeof(T) % 4 == 0, "a latch region is whole 32-bit words");
			return { Reserve(static_cast<std::uint32_t>(sizeof(T))) };
		}

		/** @brief The regions are offsets into a run's region of it. */
		std::shared_ptr<const org::LatchBlock> LatchBlock() const;

		/** @brief Cached across builds. Common and stage defines are added as Util::CompileShader adds them. Null fails the build. */
		std::shared_ptr<const RasterProgram> Raster(std::string_view a_source, std::uint32_t a_words, std::vector<DXGI_FORMAT> a_targets,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main");
		/** @brief For an accumulating draw. A vertex source replaces the fullscreen triangle; both stages get the defines. */
		std::shared_ptr<const RasterProgram> RasterTargets(std::string_view a_source, std::uint32_t a_words, std::vector<RasterProgram::Target> a_targets,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main", std::string_view a_vertexSource = {},
			std::wstring a_vertexEntry = L"main");
		std::shared_ptr<const ComputeProgram> Compute(std::string_view a_source, std::uint32_t a_words,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main");

		/** @brief The whole of kMAIN, and the dynamic-resolution part the D3D11 viewports cover. */
		RasterProgram::Viewport FullViewport() const;
		RasterProgram::Viewport DynamicViewport() const;
		/** @brief As a D3D11 viewport of width >> a_mip. */
		RasterProgram::Viewport MipViewport(std::uint32_t a_mip) const;

		/** @brief Push words must match the program's, or the build fails. */
		void Draw(FullscreenDraw a_draw);
		void Dispatch(ComputeDispatch a_dispatch);

		/** @brief make is called at each graph build. It has no gate and runs on every anchor run, so it decides in Prepare whether it has work. */
		void Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make);

		/** @brief The reason is logged. */
		void Fail(std::string_view a_reason);

		bool Failed() const;

	private:
		std::uint32_t Reserve(std::uint32_t a_bytes);
		std::string Step(std::string_view a_step) const;
		std::optional<std::uint32_t> FixedEntry(const std::array<std::uint32_t, 3>& a_groups);
		void PublishLatchBytes(std::string_view a_name, std::uint32_t a_offset, std::uint32_t a_size);
		std::optional<std::uint32_t> FindLatchBytes(std::string_view a_name, std::uint32_t a_size);

		BuildState& state;
		Programs& programs;
		Shape shape;
		std::string effect;
	};

	class CommitContext
	{
	public:
		CommitContext(const org::LatchBlock& a_latch, const org::RunRef& a_run, const RenderGraph::RunGate& a_gate) :
			latch(a_latch), run(a_run), gate(a_gate) {}

		template <class T>
		void Write(const LatchRegion<T>& a_region, const T& a_value) const
		{
			WriteBytes(a_region.offset, &a_value, sizeof(T));
		}

		/** @brief They stay closed on every other frame. */
		void Open(GateEntry a_entry, std::uint32_t a_x, std::uint32_t a_y = 1, std::uint32_t a_z = 1) const;

		/** @brief The run being committed, for an effect's own latch block. */
		const org::RunRef& Run() const { return run; }

	private:
		void WriteBytes(std::uint32_t a_offset, const void* a_data, std::size_t a_size) const;

		const org::LatchBlock& latch;
		org::RunRef run;
		const RenderGraph::RunGate& gate;
	};

	/**
	* @brief A small copy from the GPU to the CPU that a kept recording can replay.
	* The post stage is recorded ahead and its recording is submitted again and again, so its passes may not use the readback
	* service. Each run copies into its own region of a host buffer, picked by the run's lease index. A region is read when
	* the lease's next run is committed, since the lease's last run is done by then. Nothing waits.
	* Each copy also copies a stamp from the run's latch, so a run the chain did not commit is never read as one it did.
	* Make one per effect and keep it across builds. Render thread, except the passes' recording.
	*/
	class RunReadback
	{
	public:
		/** @brief One part per copy, a_parts[i] bytes each. Null and logged when the buffer cannot be made. */
		static std::shared_ptr<RunReadback> Create(std::string_view a_name, std::vector<std::uint32_t> a_parts);

		/** @brief At every build, before AddBuffer and AddTexture. */
		void Build(Builder& a_builder);
		/** @brief A pass that copies the first bytes of buffer a_source into part a_part. */
		void AddBuffer(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source);
		/** @brief A pass that copies mip 0 of a small 2D texture into part a_part. Rows are 256 bytes apart. */
		void AddTexture(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source,
			std::uint32_t a_width, std::uint32_t a_height);

		/**
		* @brief In the effect's Commit, before Take. Reads the lease's last run and stamps this run.
		* @return This run's stamp. Stamps grow by one each commit.
		*/
		std::uint32_t Commit(const CommitContext& a_commit);
		/** @brief In the effect's Executed: the run last committed was submitted. */
		void Executed();
		/** @brief The newest copy of part a_part read since the last Take, and its run's stamp. Empty when none arrived. */
		std::vector<std::byte> Take(std::uint32_t a_part, std::uint32_t* a_stamp = nullptr);

		struct State;
		explicit RunReadback(std::shared_ptr<State> a_state) :
			state(std::move(a_state)) {}

	private:
		std::shared_ptr<State> state;
	};

	/** @brief One per pipeline slot for the session. Never keep the sub-feature, since SyncPipelineResources may replace it. */
	class Effect
	{
	public:
		virtual ~Effect() = default;

		/**
		* @brief Render thread, each frame the effect is in the chain. Refreshes imports and appends what the passes depend on to a_key.
		* @return Null when the graph can run it this frame, else why not.
		*/
		virtual const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key)
		{
			(void)a_feature;
			(void)a_key;
			return nullptr;
		}

		/** @brief Render thread, at a rebuild. Fail the builder when it cannot. */
		virtual void Build(Builder& a_builder, PostProcessFeature& a_feature) = 0;

		/** @brief In the epoch, each frame. */
		virtual void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) = 0;

		/** @brief After a frame whose Commit reached the GPU with this build's passes. Not called when the epoch ran an older build. Render thread. */
		virtual void Executed(PostProcessFeature& a_feature) { (void)a_feature; }
	};

	/**
	* @brief CS_ORG_PP_LOG=1, read once. Every 120 frames the chain logs which main target it reads and the exposure it applies.
	* Render thread.
	*/
	bool LogOn();

	/** @brief With LogOn, whether this frame logs. Render thread. */
	bool LogFrame();

	// Add a port's factory here and in MakeEffect.
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
	std::unique_ptr<Effect> MakePhysicalLens();
}

#endif
