#pragma once

/**
* The framework each Post Processing sub-effect's ORG port plugs into, in graph mode.
*
* At the Tonemap anchor the chain copies kMAIN into an input image, runs each enabled sub-effect in ChainOrder, and copies
* the last image back into kMAIN and kMAIN_COPY. Chain images are transient, so the graph aliases them.
* Every listed sub-effect has an Effect here. A debug switch or a missing file is a key choice (the effect builds without that
* part), never a hold, and an effect whose inputs cannot be read is fatal. D3D11 never runs the chain in graph mode.
*
* The chain is one declared client (Frame/Declare.h). Its key is what each effect's passes depend on: its settings and frame
* variables (Key, from the revision's values) and what is no variable yet (Update, observed each frame). A changed key builds
* the chain when the next revision starts, and the frame keeps running the build it has until the revision is adopted. So an
* effect keeps what one build's commits need in that build's Run, never in the effect. Never put a per-frame value in the key.
* Includers must include <rhi_interop_vulkan.h> first.
*/

#include "Features/PostProcessing.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RasterProgram.h"
#include <Render/RunReadback.h>

#include <Render/RunRef.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

	/**
	* @brief kMAIN's size and the main targets' formats. The dynamic-resolution part is not in it: a ratio is a per-frame value
	* (CommitContext::DynamicSize), so it never builds the chain.
	*/
	struct Shape
	{
		UINT width = 0, height = 0;  // every chain image has kMAIN's size
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
		/** @brief A kept twin: its slot when it is declared (versioned), so a new version needs no build. */
		static Bind Srv(const GraphResources::Resource& a_twin, std::uint32_t a_mip = kAllMips) { return { Base::Srv(a_twin, a_mip) }; }
		static Bind Uav(const GraphResources::Resource& a_twin, std::uint32_t a_mip = kAllMips) { return { Base::Uav(a_twin, a_mip) }; }
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
	*/
	struct ComputeDispatch
	{
		std::string name;
		std::shared_ptr<const ComputeProgram> program;
		std::vector<Bind> bindings;
		std::array<std::uint32_t, 3> groups{ 1, 1, 1 };
		std::optional<GateEntry> gate;
	};

	struct BuildState;
	struct ChainPrograms;

	/**
	* @brief What a build asks of the chain besides images: its shape, linear lighting, what earlier effects published, and
	* programs. Builder gives them to a build; a plan gives them to Effect::Programs, where a program is only listed.
	* Render thread.
	*/
	class ProgramSource
	{
	public:
		virtual ~ProgramSource() = default;

		const Shape& GetShape() const { return shape; }

		/** @brief Whether linear lighting is on for this chain: the scene is stored pre-exposed. */
		bool Linear() const { return linear; }

		/** @brief Whether an earlier effect of this chain published an image or a latch region under a_name. */
		virtual bool Published(std::string_view a_name) const = 0;

		/**
		* @brief In Effect::Programs: this effect publishes a_name in its build, so a later effect's Published sees it. A build
		* does nothing here (Builder::Publish and PublishLatch do it).
		*/
		virtual void Publishes(std::string_view a_name) { (void)a_name; }

		/**
		* @brief A program. Common and stage defines are added as Util::CompileShader adds them. In a build, a program its
		* effect's Programs did not list fails the build and returns null. In a plan it is listed, and the result is null.
		*/
		std::shared_ptr<const RasterProgram> Raster(std::string_view a_source, std::uint32_t a_words, std::vector<DXGI_FORMAT> a_targets,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main");
		/** @brief For an accumulating draw. A vertex source replaces the fullscreen triangle; both stages get the defines. */
		std::shared_ptr<const RasterProgram> RasterTargets(std::string_view a_source, std::uint32_t a_words, std::vector<RasterProgram::Target> a_targets,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main", std::string_view a_vertexSource = {},
			std::wstring a_vertexEntry = L"main");
		std::shared_ptr<const ComputeProgram> Compute(std::string_view a_source, std::uint32_t a_words,
			std::vector<std::pair<std::string, std::string>> a_defines = {}, std::wstring a_entry = L"main");

	protected:
		ProgramSource(const Shape& a_shape, bool a_linear) :
			shape(a_shape), linear(a_linear) {}

		/** @brief a_key names the program by its desc, so equal keys are one program. */
		virtual std::shared_ptr<const RasterProgram> TakeRaster(std::string a_key, RasterProgram::Desc a_desc) = 0;
		virtual std::shared_ptr<const ComputeProgram> TakeCompute(std::string a_key, ComputeProgram::Desc a_desc) = 0;

		Shape shape;
		bool linear = false;
	};

	/** @brief Render thread, during one build, in the chain client's make function. */
	class Builder final : public ProgramSource
	{
	public:
		Builder(BuildState& a_state, const ChainPrograms& a_programs, const Shape& a_shape, bool a_linear, std::string_view a_effect);

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

		/**
		* @brief An effect-owned history or persistent twin. Call at every build. A declared twin registers its slot, so passes
		* bind it with Bind::Srv(twin) or ImageOf, never through Graph(). Any other twin is held through its graph resource.
		*/
		void Register(const GraphResources::Resource& a_resource);
		/** @brief An effect-owned graph resource that is not a twin, such as a constant buffer, by name. Call at every build. */
		void Register(std::string_view a_name, std::shared_ptr<org::Resource> a_resource);

		/** @brief Find returns it, or a null image. */
		void Publish(std::string_view a_name, const Image& a_image);
		Image Find(std::string_view a_name) const;

		bool Published(std::string_view a_name) const override;

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

		/** @brief The whole of kMAIN. */
		RasterProgram::Viewport FullViewport() const;
		/**
		* @brief For draws the D3D11 path limits to the dynamic-resolution part. The whole of kMAIN: the chain runs after the
		* upscale, where Upscaling locks dynamic resolution, so the dynamic part is the whole image. The ratio is a per-frame
		* value, so the effect's constants carry CommitContext::DynamicSize and nothing rebuilds on it (frame revision plan, step 8).
		*/
		RasterProgram::Viewport DynamicViewport() const;
		/** @brief As a D3D11 viewport of width >> a_mip. */
		RasterProgram::Viewport MipViewport(std::uint32_t a_mip) const;

		/** @brief Push words must match the program's, or the build fails. */
		void Draw(FullscreenDraw a_draw);
		void Dispatch(ComputeDispatch a_dispatch);

		/** @brief make is called at each graph build. It has no gate and runs on every anchor run, so it decides in Prepare whether it has work. */
		void Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make);

		/** @brief The reason is logged and the build fails, which is fatal. */
		void Fail(std::string_view a_reason);

		bool Failed() const;

	protected:
		std::shared_ptr<const RasterProgram> TakeRaster(std::string a_key, RasterProgram::Desc a_desc) override;
		std::shared_ptr<const ComputeProgram> TakeCompute(std::string a_key, ComputeProgram::Desc a_desc) override;

	private:
		std::uint32_t Reserve(std::uint32_t a_bytes);
		std::string Step(std::string_view a_step) const;
		std::optional<std::uint32_t> FixedEntry(const std::array<std::uint32_t, 3>& a_groups);
		void PublishLatchBytes(std::string_view a_name, std::uint32_t a_offset, std::uint32_t a_size);
		std::optional<std::uint32_t> FindLatchBytes(std::string_view a_name, std::uint32_t a_size);

		BuildState& state;
		const ChainPrograms& programs;
		std::string effect;
	};

	/** @brief An image of a kept twin, its slot when it is declared. */
	Image ImageOf(const GraphResources::Resource& a_twin, DXGI_FORMAT a_format);

	class CommitContext
	{
	public:
		CommitContext(const org::LatchBlock& a_latch, const org::RunRef& a_run, const RenderGraph::RunGate& a_gate, float2 a_dynamicSize) :
			latch(a_latch), run(a_run), gate(a_gate), dynamicSize(a_dynamicSize) {}

		template <class T>
		void Write(const LatchRegion<T>& a_region, const T& a_value) const
		{
			WriteBytes(a_region.offset, &a_value, sizeof(T));
		}

		/** @brief They stay closed on every other frame. */
		void Open(GateEntry a_entry, std::uint32_t a_x, std::uint32_t a_y = 1, std::uint32_t a_z = 1) const;

		/** @brief The run being committed, for an effect's own latch block. */
		const org::RunRef& Run() const { return run; }

		/** @brief This frame's dynamic-resolution part of kMAIN in pixels, as the D3D11 viewports cover it. A per-frame value. */
		float2 DynamicSize() const { return dynamicSize; }

	private:
		void WriteBytes(std::uint32_t a_offset, const void* a_data, std::size_t a_size) const;

		const org::LatchBlock& latch;
		org::RunRef run;
		const RenderGraph::RunGate& gate;
		float2 dynamicSize;
	};

	/**
	* @brief A small copy from the GPU to the CPU that a kept recording can replay: org::RunReadback (RunReadback.h) on the
	* chain's latch. Make one per effect and keep it across builds. Render thread, except the passes' recording.
	*/
	class RunReadback
	{
	public:
		/** @brief One part per copy, a_parts[i] bytes each. Its errors are fatal (RenderGraph::FatalHandler). */
		static std::shared_ptr<RunReadback> Create(std::string_view a_name, std::vector<std::uint32_t> a_parts);

		/** @brief At every build, before AddBuffer and AddTexture. Returns the build's stamp region, for its Run's Commit. */
		LatchRegion<org::RunReadback::Stamp> BuildStamp(Builder& a_builder);
		/** @brief A pass that copies the first bytes of buffer a_source into part a_part. */
		void AddBuffer(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source);
		/** @brief A pass that copies mip 0 of a small 2D texture into part a_part. Rows are 256 bytes apart. */
		void AddTexture(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source,
			std::uint32_t a_width, std::uint32_t a_height);

		/**
		* @brief In the Run's Commit, before Take. Reads the lease's last run, stamps this run and writes the stamp into a_stamp, the
		* region BuildStamp gave the build being committed.
		* @return This run's stamp. Stamps grow by one each commit.
		*/
		std::uint32_t Commit(const CommitContext& a_commit, const LatchRegion<org::RunReadback::Stamp>& a_stamp);
		/** @brief In the effect's Executed: the run last committed was submitted. */
		void Executed();
		/** @brief The newest copy of part a_part read since the last Take, and its run's stamp. Empty when none arrived. */
		std::vector<std::byte> Take(std::uint32_t a_part, std::uint32_t* a_stamp = nullptr);

		explicit RunReadback(std::shared_ptr<org::RunReadback> a_readback) :
			readback(std::move(a_readback)) {}

	private:
		std::shared_ptr<org::RunReadback> readback;
	};

	/**
	* @brief What one build's commits use: its latch regions, gate entries and what it was built for. Made by the effect's
	* BuildRun and held by the registration of that build, so a frame that still runs an older build commits that build's Run.
	*/
	class Run
	{
	public:
		virtual ~Run() = default;

		/** @brief In the epoch, each frame this build runs. */
		virtual void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) = 0;

		/** @brief After a frame whose commit reached the GPU with this build. Render thread. */
		virtual void Executed(PostProcessFeature& a_feature) { (void)a_feature; }
	};

	/** @brief One per pipeline slot for the session. Never keep the sub-feature, since SyncPipelineResources may replace it. */
	class Effect
	{
	public:
		virtual ~Effect() = default;

		/**
		* @brief Whether the port reads what the sub-feature's D3D11 SetupResources makes. Only then does the chain set the
		* sub-feature up in graph mode.
		*/
		virtual bool ReadsD3D11Resources() const { return false; }

		/**
		* @brief Render thread, once, when the chain is set up. Adds the settings and frame variables Key reads to a_dependencies,
		* and declares the effect's kept textures (Frame::DeclareTexture2D/3D). The settings are bound by then.
		*/
		virtual void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies& a_dependencies)
		{
			(void)a_feature;
			(void)a_dependencies;
		}

		/**
		* @brief When a revision starts, render thread. Appends what the passes depend on among the revision's values (a_values) to
		* a_key. BuildRun gets these words back, so a build reads its settings from them, never from the live settings.
		*/
		virtual void Key(const RenderGraph::Frame::Inputs& a_values, PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key)
		{
			(void)a_values;
			(void)a_feature;
			(void)a_key;
		}

		/**
		* @brief Render thread, each frame the effect is in the chain. Refreshes imports and appends what the passes depend on that
		* is no variable yet (an import's identity, a loaded file, a debug switch) to a_observed. A change builds the chain at the
		* next revision.
		* @return Null, else why its inputs cannot be read, which is fatal.
		*/
		virtual const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_observed)
		{
			(void)a_feature;
			(void)a_observed;
			return nullptr;
		}

		/**
		* @brief Render thread, when a revision starts, before BuildRun for the same key words. Lists every program BuildRun will
		* ask a_programs for, through the same Raster, RasterTargets and Compute calls, and with Publishes every name BuildRun will
		* publish. The programs lane builds the chain's list before the revision starts. A program BuildRun asks for that is not
		* listed fails the build, which is fatal. Read only what BuildRun reads: a_key, the shape, and what Update observed.
		*/
		virtual void Programs(ProgramSource& a_programs, PostProcessFeature& a_feature, std::span<const std::uint64_t> a_key)
		{
			(void)a_programs;
			(void)a_feature;
			(void)a_key;
		}

		/**
		* @brief Render thread, when a revision builds the chain, in the chain client's make function. a_key holds the words Key
		* appended for that revision. Programs come from a_builder, never from a compile.
		* @return The build's Run. Null only after failing the builder, which is fatal.
		*/
		virtual std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature& a_feature, std::span<const std::uint64_t> a_key) = 0;
	};

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

