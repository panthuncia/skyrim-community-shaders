#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDrawStats.h"
#include "RenderGraph/Frame/Revision.h"

struct ID3D11Texture2D;

namespace RE
{
	class BSGeometry;
	class NiFrustum;
	class NiPoint3;
	class NiTransform;
}

namespace org::runtime
{
	class IUploadService;
}

namespace DCLF
{
	namespace Diagnostics
	{
		struct DrawsView;
	}
	struct Lookups;
	struct LookupsResolveState;
	struct FamilyPipelines;
	struct FamilyRenderState;
	struct PipelineCatalog;
	struct SceneTables;
	struct ForwardTargets;
	struct TargetFormats;
	struct ToggleSet;
	struct ViewEpochState;
	namespace Draws
	{
		struct MainPayload;
		struct PassFrame;
		struct Resources;
		struct SceneBuffers;
		struct ViewFace;
	}
	namespace TreeInstances
	{
		struct DrawRow;
	}
	namespace Api
	{
		enum class DrawFamily : std::uint8_t;
	}

	/** @brief Counters for an epoch that shares another's shape and is submitted by its owner. */
	struct SharedEpochStats
	{
		std::uint64_t reached = 0, epochs = 0, noSharedPayload = 0, noRecording = 0, empty = 0, failed = 0;
		std::uint32_t submittedFrame = ~0u;
	};
	/** @brief The owner's hooks for submitting a shared epoch. prepare runs on the render thread right before the execution. */
	struct SharedEpochSubmit
	{
		bool (*prepare)(void* a_context, const Draws::Resources* a_resources, const Draws::PassFrame* a_shape) = nullptr;
		void* context = nullptr;
		// True when the shape gives the epoch nothing to draw. The epoch is then skipped entirely, boundary included.
		bool (*empty)(const Draws::PassFrame& a_shape) = nullptr;
		// The epoch holds the CPU pipeline's forward site, so the frame's forward run is handed to it (Direct::ForwardReached).
		bool forwardSite = false;
		// The CPU pipeline's water site of deferred range waterSite - 1 (0: none), drawing target set waterVariant (Direct::WaterReached).
		std::uint32_t waterSite = 0;
		std::uint32_t waterVariant = 0;
		// Runs first inside the epoch's commit, where the host's current run is the run its passes recorded at execution see.
		void (*commit)(void* a_context) = nullptr;
	};
	/** @brief Whether a shared epoch was submitted, and whether its optional part draws. */
	struct SharedEpochResult
	{
		bool submitted = false;
		bool optional = false;
	};

	/**
	 * @brief DCLF's draws. The render graph draws the installed scene with GPU-culled indirect draws, and the engine skips what DCLF claims.
	 *
	 * Each DCLF draw point is an epoch: Z-prepass, colour, forward, shadow views, reflection faces and first person. An epoch
	 * submits a recording made ahead of time and only writes per-frame data into it. Nothing is built during the frame.
	 *
	 * Threads: the scene lane resolves lookups and readiness, the snapshot builder makes payloads and revisions, the main thread
	 * writes the cull side effects at Main::Update's entry, and the render thread does the rest. They hand off through
	 * latest-wins slots and atomics.
	 */
	class IndirectDraws
	{
	public:
		using Skip = DrawSkip;
		using Stats = IndirectDrawStats;
		using ShadowNotReady = ShadowViewNotReady;
		using ShadowStats = ShadowDrawStats;

		void ResetCommitTimings()
		{
			stats.commitUs = {};
			stats.commitEpochs = 0;
		}

		static IndirectDraws& Get();

		/** @brief The shadow phases DCLF can draw. Changes only at setup, on a toggle or a failure. Readable from any thread. */
		std::uint8_t ShadowCapability() const;
		/** @brief Render thread, frame start. Sets up and publishes the shadow capability. */
		void UpdateShadowCapability();
		/** @brief The report line for the shadow capability and its changes since startup. */
		std::string ShadowCapabilityReport() const;
		/** @brief How many shadow views DCLF draws. UINT32_MAX once placements are set up, 0 before that or after a failure. */
		std::uint32_t ShadowViewCapacity();
		/**
		 * @brief Decides whether this frame's shadow views are DCLF's. Until a revision has recorded the shape, the engine draws all of them.
		 */
		void DecideShadowCoverage();
		/**
		 * @brief Scene lane. Whether the object's shadow pipelines and alpha-tested diffuse are ready for a phase.
		 * a_why gets SetStats::waitingBy's index (7 shadow pipeline, 12 past the scene buffers, 13 diffuse not imported).
		 */
		bool PhaseReady(const void* a_tables, const Lookups& a_lookups, std::uint32_t a_slot, std::uint8_t a_phase, std::uint32_t* a_why = nullptr) const;
		/**
		 * @brief Whether the object fits the scene buffers the next builds use. One that does not would be drawn by nobody, so it waits.
		 * Reads the coordinator's copy of the fits, never the render thread's.
		 */
		bool FitsScene(const void* a_tables, std::uint32_t a_slot, bool a_shadow) const;
		/** @brief Changes when the scene buffers grow, so slots waiting for room are retried. */
		std::uint64_t SceneFitSerial() const;
		/** @brief Coordinator, at each pass start. Takes the latest ahead context and fits. */
		void TakeAheadContext();

		/** @brief Captures the main pass's bindings and viewport for the colour epoch. */
		void CaptureMainPass();

		/**
		 * @brief Runs the Z-prepass epoch inside the native depth pass, writing DCLF's depth into the engine's main depth.
		 *
		 * It runs here because everything later that tests or reads depth uses the buffer as it is at this point.
		 * a_hostedWork runs inside the depth epoch's execution so hosted view epochs commit into it.
		 */
		void CaptureDepthPass(const std::function<void()>& a_hostedWork = {});

		/** @brief Runs the colour epoch at the end of the main pass's opaque batches. */
		void ExecuteColour();

		/**
		 * @brief Submits a shared epoch at its owner's hook, at most once a frame. Only valid after this frame's commit of the epoch it shares with.
		 */
		SharedEpochResult SubmitSharedEpoch(std::uint32_t a_epoch, const SharedEpochSubmit& a_submit);
		/** @brief Returns and resets a shared epoch's counters since the last report. */
		SharedEpochStats TakeSharedEpochStats(std::uint32_t a_epoch);
		/** @brief The shared epochs' report lines. */
		std::string SharedEpochReport();

		/** @brief Render thread, frame start. Posts the frame-only inputs that the scene lane's lookups need. */
		void PostLookupInputs();
		/** @brief Scene lane. Resolves pipeline entries from the tables and catalog alone. Reads no engine or render-thread state. */
		void ResolveLookups(const void* a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, LookupsResolveState& a_state);
		/** @brief Hands a publication to the snapshot builder, latest wins. */
		void PostSnapshotWork(std::shared_ptr<const void> a_publication);
		/**
		 * @brief Marks frame a_frame's scene pass as done. The builder publishes a payload only for a snapshot the frame will adopt.
		 */
		void NoteScenePassDone(std::uint32_t a_frame);
		/** @brief Any thread. The frame of the newest finished scene pass, 0 before the first. */
		std::uint32_t ScenePassDoneFrame() const;
		/**
		 * @brief Any thread. The sequence of the newest publication the builder has taken, 0 for none and ~0 without a builder.
		 * The coordinator waits for this before publishing again, since a newer post would just replace the old one.
		 */
		std::uint64_t SnapshotWorkTaken() const;
		/** @brief Records the age of the oldest event an installed publication carried, for the snapshot report. */
		void NoteEventAdoption(double a_ms, std::uint32_t a_frames);
		/**
		 * @brief Render thread, main thread, at Begin. The frame revision took a_scene, a snapshot the builder posted (plan step 10b),
		 * whether it adopts it or not. Opens the builder's publication gate.
		 */
		void SceneTaken(const std::shared_ptr<const void>& a_scene);
		/**
		 * @brief Render thread, inside the adoption point: the frame revision adopts a_scene, alone or with the frame revision it was
		 * recorded against, after its staged builds attached. The scene fragment's install.
		 */
		void InstallScene(const std::shared_ptr<const void>& a_scene);
		/** @brief Render thread. The frame revision did not adopt a_scene. It is released on the builder, which makes a new one. */
		void DropScene(const std::shared_ptr<const void>& a_scene, RenderGraph::Frame::Revision::FragmentDrop a_why, std::string_view a_text,
			std::span<const std::uint32_t> a_epochs);
		/**
		 * @brief Any thread. The frames whose coverage ran at least one DCLF epoch, in the session. While it moves, a frame revision with
		 * DCLF epochs needs a scene recorded for it.
		 */
		std::uint64_t SceneRecordedFrames() const;
		/**
		 * @brief Render thread, after the frame revision's scene step at every main-thread Begin. Updates the graph conditions and counts a
		 * Begin that took no scene. It never waits: a shader reload's rebuilt graphs come with a frame revision (RenderGraph/PipelineVersions.h).
		 */
		void AfterSceneStep();
		/** @brief Render thread. The adopted snapshot's ScenePublication, or null before the first. */
		std::shared_ptr<const void> AdoptedPublication() const;
		/** @brief The publication a posted scene fragment's value carries. Any thread. */
		static std::shared_ptr<const void> PublicationOf(const std::shared_ptr<const void>& a_scene);
		/** @brief Render thread, at Present. Ties the frame's pooled versions to its retirement point so they are not reused early. */
		void BindAdopted();
		/**
		 * @brief Render thread. What DCLF's graphs and snapshots are built for changed after DCLF first drew (plan step 10a-0).
		 * No revision carries such a change, so it is a tap error (TapError::ClientInputChanged), fatal in development builds.
		 */
		static void NoteInputChanged(std::string_view a_what);
		/** @brief Render thread, frame start. Builds the frame's payload from the installed snapshot. False without one. */
		bool MakeFramePayload();
		/** @brief Render thread, frame start. Posts the resources and fits the next builds need, latest wins. */
		void PostAheadContext();

		/**
		 * @brief The scene's tables, null until EnsureSceneBuffers. Read on the render thread or on the main thread at Main::Update's entry.
		 */
		const Draws::SceneBuffers* Scene() const;
		/** @brief Render thread, frame start. Hands visibility readbacks to the decode task, which is never joined. Writes no engine memory. */
		void KickVisibilityReadback();
		/**
		 * @brief Main thread at Main::Update's entry, while no cull or job list runs. Writes the decoder's kAccumulated results with atomics.
		 * kAccumulated is the engine's cull side effect that DCLF must reproduce where it stands in for the culls.
		 */
		void WriteAccumulated(const std::function<bool(const void*)>& a_stoodIn);
		/** @brief Main thread. DCLF's last verdict for a stood-in root, visible if not written yet. Reads no engine memory. */
		bool AccumulatedVerdict(const void* a_root) const;
		/** @brief Main thread. DCLF's verdict for any root it lists, false when it has none. Reads no engine memory. */
		bool AccumulatedVerdictOf(const void* a_root, bool& a_visible) const;
		/** @brief Render thread. After a camera cut every stood-in root counts as visible until a batch from a_frame or later. */
		void NoteCameraCut(std::uint32_t a_frame);
		/** @brief Any thread. Makes the next decode write every stood-in root, because the engine overwrote them. */
		void RequestAccumulatedRefresh();
		/** @brief The visibility readback's report line. */
		std::string VisibilityReport();
		/**
		 * @brief Render thread, before the main camera's cull. Decides which draw families DCLF owns this frame.
		 * The engine's passes for a family stay withheld while it waits for pipelines, so the engine never draws it meanwhile.
		 */
		void DecideDrawFamilies(std::uint32_t a_frame);
		/** @brief Render thread. Whether a_family's draws are DCLF's this frame. */
		bool DrawFamilyOwned(Api::DrawFamily a_family) const;
		/** @brief Render thread. The draw families whose decision this frame waited for their pipelines. */
		std::uint32_t DrawFamiliesWaiting() const;
		/**
		 * @brief The family's pipelines in the adopted revision, or null. A frame owns a family only while these match its catalog,
		 * so withholding the engine's draws always means a recording draws the family.
		 */
		std::shared_ptr<const FamilyPipelines> AdoptedFamilyPipelines(Api::DrawFamily a_family) const;
		/**
		 * @brief Render owner, after each adoption: publishes what the families' decisions read of the render side (the adopted
		 * revision's family pipelines). The game half reads the last one published and never waits for a newer one.
		 */
		void PublishFamilyRenderState();
		/** @brief Game half, inside DecideDrawFamilies: the render state as last published, or null before the first. */
		const FamilyRenderState* FamilyInputs() const;
		// View engine calls. Render thread unless noted, and nothing exists before the implementation.
		/** @brief The view's frame state, with its targets, resources and coverage. */
		const ViewEpochState* ViewState(Api::EngineView a_view) const;
		/** @brief The main pass's captured formats, which view targets follow. */
		const TargetFormats* MainTargetFormats() const;
		/** @brief Sets the view's targets and publishes them for the scene lane. True when they changed. */
		bool SetViewTargets(Api::EngineView a_view, const ForwardTargets& a_targets);
		/** @brief Posts the frame inputs a scene revision reads. */
		void PostViewInputs();
		/** @brief Sets the engine's depth range for the view. A positive change is posted as a revision input. */
		void SetViewDepthRange(Api::EngineView a_view, float a_range);
		/** @brief The last depth range set for the view, else its shape's default. */
		float ViewDepthRange(Api::EngineView a_view) const;
		/** @brief Publishes whether the main forward pipelines can draw into the view, returning the previous value. */
		bool ExchangeViewForwardCompatible(Api::EngineView a_view, bool a_compatible);
		/** @brief Counts from ResolveViewPipelines. */
		struct ViewPipelineCounts
		{
			std::uint32_t slots = 0, ready = 0, programs = 0;
			bool waited = false;
		};
		/** @brief Requests and reads the view's forward pipelines for the wanted slots. An empty a_wanted means every slot. */
		ViewPipelineCounts ResolveViewPipelines(Api::EngineView a_view, const SceneTables& a_tables, const PipelineCatalog* a_catalog, std::span<const std::uint8_t> a_wanted,
			bool a_programs = false);
		/** @brief Drops the view's slot pipelines when the view goes off. */
		void DropViewPipelines(Api::EngineView a_view);
		/**
		 * @brief Resolves the forward pipelines of the draw families a trees view draws per face. True while one is still pending.
		 * Call once a frame after DecideDrawFamilies.
		 */
		bool ResolveViewFaces(Api::EngineView a_view, bool a_on, const PipelineCatalog* a_catalog);
		void DecideViewFaces(Api::EngineView a_view);
		/**
		 * @brief At a cube camera update's start. Returns the LOD roots the view's faces draw in the engine's place, whose add-roots
		 * are skipped so the engine neither culls nor registers them.
		 */
		std::uint32_t ViewLodRootsOwned(Api::EngineView a_view, bool a_plain);
		/** @brief Creates the view's graph resources once the main resources exist. True when it has them. */
		bool SetUpView(Api::EngineView a_view);
		/** @brief Imports the view's target into the graph as its entry describes. True when imported. */
		bool ImportViewTarget(Api::EngineView a_view, ID3D11Texture2D* a_texture = nullptr);
		/** @brief The payloads and scene list inputs a view's epoch draws from. */
		struct ViewListed
		{
			std::array<const Draws::MainPayload*, 2> owners{};
			std::uint32_t ownerCount = 0;
			std::uint32_t inputs = 0;
		};
		/**
		 * @brief Which of the frame's commits a view's epoch draws from. DepthThisFrameTargetAfter is for views whose target is only
		 * known at capture, such as the reflection cube.
		 */
		enum class ViewInputsFrom : std::uint8_t
		{
			DepthThisFrame,
			DepthThisFrameTargetAfter,
			ColourThisFrame,
		};
		enum class ViewInputsTaken : std::uint8_t
		{
			Taken,
			Stale,     // the commit is not the frame's, or nothing is listed
			NoTarget,  // the view's resources or its imported target are missing (DepthThisFrame)
		};
		ViewInputsTaken TakeViewInputs(Api::EngineView a_view, ViewInputsFrom a_from, ViewListed& a_listed);
		enum class ViewSubmit : std::uint8_t
		{
			Submitted,
			NoRevision,  // the adopted revision has no recording or shape for the epoch
			NoOwners,    // the payloads it draws are not held
			Failed,      // the graph refused the epoch
		};
		/**
		 * @brief Commits a_faces into the view's epoch and submits its recording. A hosted epoch can only be committed from inside its host's
		 * commit and returns NoRevision anywhere else. a_nullTextures has a bit per frame texture register (t16 up) left null.
		 */
		ViewSubmit SubmitViewEpoch(Api::EngineView a_view, std::uint32_t a_epoch, const ViewListed& a_listed, std::span<const Draws::ViewFace> a_faces,
			std::uint32_t* a_drawn = nullptr, std::array<std::uint64_t, 2>* a_nullTextures = nullptr);
		/** @brief Any thread. Whether the view's epochs draw its members this frame. */
		bool ViewCovered(Api::EngineView a_view) const;
		/** @brief Whether the view's phase can be drawn, meaning a feature took the view and its targets are published. */
		bool ViewDrawable(Api::EngineView a_view) const;
		/**
		 * @brief Scene lane. Whether a_slot's pipelines for the view are in a_catalog. Reads nothing the render thread writes per frame.
		 * a_why: 1 no catalog or targets, 2 forward key not drawable, 3 no key, 4 not in catalog, 5 no Z-prepass group pipeline.
		 */
		bool ViewSlotReady(Api::EngineView a_view, const void* a_tables, const void* a_lookups, const PipelineCatalog* a_catalog, std::uint32_t a_slot,
			std::uint32_t* a_why = nullptr) const;

		/** @brief Posts the revision inputs when they moved. Call where such a value is captured outside IndirectDraws. */
		void NoteRevisionInputs();
		/**
		 * @brief Render thread, first thing each frame. The only point where the graph is built, so a frame's recordings stay valid
		 * for that whole frame.
		 */
		void BuildPoint();
		/**
		 * @brief Whether the adopted snapshot covers the main epochs. False only while DCLF does not draw (FrameView::NotDrawingYet) or
		 * under a load screen. Otherwise false is a tap error. Each other uncovered epoch gets an EngineState; an epoch the recipe has
		 * DCLF cover (Recipes::Table::CoveredSegment) with none is a tap error too (TapError::Uncovered).
		 */
		bool DecideCoverage();
		/** @brief The coverage inputs and verdicts as text, for a defect report. */
		std::string CoverageState() const;

		/** @brief The async report lines. Resets their counters. */
		std::string AsyncReport();

		/** @brief Starts the shadow frame. Shadow views are captured from here on. */
		void BeginShadowFrame();

		/** @brief Captures a shadow view after its native draws, for the shadow epoch. Draws nothing. */
		void CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode);
		/**
		 * @brief Captures a covered view the engine did not draw (Shadows/Engine's native shadow maps). It reads no renderer state: the
		 * camera is the view's shadow source's and the rasterizer state its placement's. A view no source owns is not captured.
		 * A paraboloid's view passes its ParabolaParam (radius, side) in a_parabola; other views read the engine's globals as the engine's
		 * capture does. Returns whether it was captured.
		 */
		bool CaptureNativeShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode, const float* a_parabola);
		/** @brief How many views this frame's shadow epoch has captured so far. */
		std::size_t CapturedShadowViews() const;
		/** @brief Forgets the views captured from a_mark on, so the engine draws their light after all. */
		void DropShadowViewsFrom(std::size_t a_mark);

		/**
		 * @brief Runs one epoch that culls casters into every captured shadow view and draws the occlusion maps. Returns the maps drawn.
		 * One epoch instead of one per view saves CPU, and is safe because the slices are only read after Main_RenderShadowMaps.
		 */
		std::uint32_t ExecuteShadowFrame(std::uint32_t a_maps);

		/**
		 * @brief Occlusion maps such as Skylighting's sky map and the precipitation mask. When the shadow epoch cannot draw a map,
		 * the engine's SetupMask registers its scene at its own occlusion point.
		 */
		void OcclusionView(std::uint32_t a_view, const RE::NiTransform& a_world, const RE::NiFrustum& a_frustum, const std::array<float, 4>& a_port);
		bool OcclusionReady(std::uint32_t a_view) const;

		const ShadowStats& GetShadowStats() const { return shadowStats; }
		void ResetShadowStats() { shadowStats = {}; }

		const Stats& GetStats() const { return stats; }

		~IndirectDraws();

	private:
		friend struct Diagnostics::DrawsView;
		IndirectDraws();

		/** @brief Commits and submits one main epoch, Z-prepass or colour. */
		void RunEpoch(bool a_depthOnly);
		bool CaptureView(std::uint32_t a_viewId, std::uint32_t a_renderMode, const float* a_parabola, bool a_native);

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
		ShadowStats shadowStats;
	};

}
