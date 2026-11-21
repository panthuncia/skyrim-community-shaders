#pragma once

// IndirectDraws::Impl holds the state of IndirectDraws' threads and the functions over it.
// Each member says which thread owns it. Cohesive groups live in the Impl/ headers as bases.
// Shadow and occlusion view state belongs to ShadowEngine, which reaches Impl only through ShadowHost.

namespace DCLF
{
	using namespace Draws;
}

#include "Features/DrawcallLimitFix/Core/Render/Epochs/ExecutionOwners.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/SnapshotBuilder.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/Producer.h"
#include "Features/DrawcallLimitFix/Core/Render/Impl/Stats.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Participant.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEngine.h"
#include "Features/DrawcallLimitFix/Api/LightVolumes.h"

#include <Render/RunRef.h>

namespace DCLF
{
	namespace Draws
	{
		struct MainCommit;
	}

	struct IndirectDraws::Impl final : ImplSnapshotBuilder, ImplViews, ImplProducer, ImplStats, ShadowHost
	{
		std::shared_ptr<Resources> resources;
		// A change of formats rebuilds the resources and the graph.
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::optional<Draws::Capture> pending;

		/**
		 * @brief The main resources' parts sized by the display, made on the build executor for a new DisplaySize (phase 3,
		 * window resize). The render thread reads made only once done is set.
		 */
		struct Sizing
		{
			std::uint32_t width = 0, height = 0;
			std::shared_ptr<Resources> made;                       // the sized parts and the dispatch signatures, nothing else yet
			bool failed = false;
			std::atomic<bool> done{ false };
		};
		// Render thread. The job for the newest display size the builds do not have yet. Null when none.
		std::shared_ptr<Sizing> sizing;
		/**
		 * Render thread. The main resources for a new display size, handed to the builds and the snapshot builder. The frame runs
		 * resources until it adopts the first scene made for these (InstallScene), then these become resources.
		 */
		std::shared_ptr<Resources> pendingResources;
		/** @brief Render thread. The main resources the builds and the snapshot builder are for: the pending ones, else the frame's. */
		const std::shared_ptr<Resources>& AheadMain() const { return pendingResources ? pendingResources : resources; }
		/**
		 * @brief Render thread, in the adoption point. Follows the adopted DisplaySize: starts the sized parts for a new size on the
		 * build executor, and once they are made and no other new resources wait for their scene, hands the new main resources to
		 * the builds. Never waits.
		 */
		void FollowDisplaySize();
		/** @brief Render thread, at a scene's install. The frame now runs the pending main resources the scene was made for. */
		void AdoptPendingMain();

		std::unique_ptr<ShadowEngine> shadowEngine = std::make_unique<ShadowEngine>(*this);
		/**
		 * @brief Grows the main layout and its resources to the draw bound. A bound over the device's max sequence count or the sort's rank
		 * field is a hard failure.
		 */
		void RequireMain(GrowthBuild& a_build, Resources& a_resources, RevisionMake& a_make);
		/** @brief Grows the colour segment's state class sub-ranges and reserves the latch's class table. */
		void RequireMainClasses(GrowthBuild& a_build, Resources& a_resources, RevisionMake& a_make);
		/** @brief Makes room in the index pool for every geometry, relaying out a fragmented pool. */
		void RequireIndexPool(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_generation);
		// Grows the main latch's layout when it holds fewer entries than asked.
		void ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets = 0, std::uint32_t a_classes = 0,
			std::uint32_t a_classMapWords = 0);
		/** @brief Lays out every latch block. Call once per make, after every reserve. */
		void RequireLatches(GrowthBuild& a_build);

		// A t16+ input keeps its import only while its register still names that SRV. This bounds reuse without an age-based cache.
		struct FrameTextureBinding
		{
			ID3D11ShaderResourceView* view = nullptr;
			GpuTextures::Binding binding;
		};
		std::array<FrameTextureBinding, kTextureRegisters> frameTextureBindings{};
		std::uint32_t frameTextureGeneration = ~0u;
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> commitStagedPool;
		BuildCache mainCache;
		void PostFit();
		/** @brief Render thread. Posts the ahead context and the fits as they are now. */
		void PostAhead(std::unique_ptr<AheadPost> a_post, bool a_fit);
		/**
		 * @brief Render thread. Makes the ahead context from the frame as it is now. False without main resources, and then no fit is posted.
		 */
		bool MakeAheadContext(AheadContext& a_out);
		/**
		 * @brief Posts the context for main resources made at this frame's Setup straight to the builder, so the next frame's wait
		 * for a snapshot does not need a coordinator publication.
		 */
		void PostResourcesAhead();
		/**
		 * @brief Builder. Swaps in the render thread's context when a work item was made for older main resources. True when its draws must be rebuilt.
		 */
		bool RefreshWorkContext();
		/**
		 * @brief Render thread. Whether the adopted snapshot is current: a revision, complete draws for the main resources the frame
		 * runs, a ready payload, and every epoch's recordings of its attached build.
		 */
		bool AdoptedCurrent() const;
		/** @brief Render thread, at a scene's install. Warns once per epoch recording that binds a version the frame does not read. */
		void CheckSceneVersions(const SceneSnapshot& a_snapshot);

		/** @brief Fills the buffer addresses, SRV indices and capacities a build embeds. Other fields of a_out are left alone. */
		static void AddressesOf(const NamedVersions& a_sealed, const MainLayout& a_layout, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out);
		static void AddressesOf(const NamedVersions& a_sealed, const ShadowResources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out);
		/** @brief The snapshot builder's pump. A function-local static, never destroyed. */
		org::async::SerializedTaskPump& SnapshotPump();
		void SnapshotPass();
		// True when the view batches for the payload are made or not wanted.
		bool ViewBatchesReady();
		// True when the payload is made, failed or not wanted, so a snapshot can be posted.
		bool PayloadSettled();
		/** @brief Builder. Builds the work item's draws and embeds the buffer addresses of the sealed versions. */
		void BuildSnapshotDraws(const NamedVersions* a_sealed, const MainLayout* a_layout);
		/**
		 * @brief Builder. Makes and posts the payload for its draws once every part is GPU ready. Only one source is in flight at a time.
		 * A payload is published only for a snapshot the frame will adopt, so it can wait for the frame's take and the scene pass's end.
		 */
		void PublishPayload();
		/** @brief Builder. True when its draws have something to draw and so need a payload. */
		bool PayloadWanted() const;
		/** @brief Builder. Posts the snapshot of its work item with the completed revision. */
		void PostSnapshot(org::async::RevisionAssembler::Lease a_revision);
		/** @brief The builder's report lines. Resets its counters. */
		std::string ProducerReport();
		/** @brief Report lines for the stores the builds write. Resets its counters. */
		std::string StoresReport();
		/** @brief The snapshot report lines. */
		std::string SnapshotReport();
		/** @brief Builder. Makes a publication's stream views and payloads, in publication order. */
		std::shared_ptr<const DrawPublication> RunAhead(std::shared_ptr<const SceneTables> a_tables, const AheadContext& a_context, const Lookups& a_lookups,
			std::uint32_t a_generation, std::uint32_t a_frame, std::shared_ptr<const EntrySpace> a_sunCandidates, std::shared_ptr<const EntrySpace> a_lightCandidates);
		/** @brief Depth commit. Hands the last visibility events to the decode task and re-arms the readback range. */
		void ArmVisibilityReadback(SceneBuffers& a_scene, CommitUploads& a_uploads);
		std::shared_ptr<const DrawPublication> installedDraws;
		/** @brief The installed tables' pipeline slots. A covering revision was made after their commit, so its latch holds them. */
		std::uint32_t InstalledPipelineSlots() const
		{
			return installedDraws && installedDraws->tables ? static_cast<std::uint32_t>(installedDraws->tables->pipelines.size()) : 0u;
		}
		// Builds ahead run one at a time in publication order, so they can share these buffers.
		std::shared_ptr<SceneBuffers> scene;
		// This frame's draw family decisions.
		std::array<bool, Extensions::kDrawFamilies> familyOwned{};
		// Families whose last decision waited for pipelines. Their passes are withheld and not drawn.
		std::array<bool, Extensions::kDrawFamilies> familyWaiting{};
		/** @brief Render thread. Creates a view's graph resources once its targets are known. */
		bool SetupView(ViewKind a_kind);
		/** @brief Grows a view's latch layout and buffers. */
		void RequireView(GrowthBuild& a_build, ViewResources& a_view, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws, std::uint32_t a_classMapWords);
		/** @brief Imports a DepthTarget view's target, kPOST_ZPREPASS_COPY, again whenever it changes. */
		bool ImportViewDepthTarget(ViewKind a_kind);
		/** @brief Whether a LodTechniques view has the object's forward pipeline ready. */
		bool LodPhaseReady(std::uint8_t a_phase, const SceneTables& a_tables, std::uint32_t a_slot) const;
		/** @brief Imports a ColourWithOwnDepth view's engine target with DCLF's own depth, again whenever it changes. */
		bool ImportViewColourTarget(ViewKind a_kind, ID3D11Texture2D* a_texture);
		/** @brief A trees view's family rows for a commit. Each is empty unless the faces' family is DCLF's. */
		ViewFamilyRows ViewFaceRows(ViewKind a_kind, const ViewFrame& a_shape, std::vector<std::shared_ptr<const void>>& a_owners);
		/** @brief Render thread. Writes a view's faces and family rows into the shape's latch. Returns the faces drawn. */
		std::uint32_t CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const ViewFamilyRows* a_rows);
		// CPU mode. A hosted epoch's committed view is posted for the depth site to draw.
		void PostHostedView(ViewKind a_view, std::uint32_t a_epoch, std::span<const ViewFace> a_faces);
		IndirectDraws::ViewSubmit SubmitDirectViewEpoch(std::uint32_t a_epoch, RenderGraphRuntime::Segment a_segment, const IndirectDraws::ViewListed& a_listed,
			std::span<const ViewFace> a_faces);
		/**
		 * @brief Commits a hosted view epoch into its host's current commit. Only valid from inside the host's commit.
		 * Returns NoRevision anywhere else or when uncovered.
		 */
		IndirectDraws::ViewSubmit CommitHostedView(ViewKind a_view, std::uint32_t a_epoch, const IndirectDraws::ViewListed& a_listed, std::span<const ViewFace> a_faces,
			std::uint32_t* a_drawn, std::array<std::uint64_t, 2>* a_nullTextures);
		/** @brief What an epoch drawing a_payloads must keep alive until it retires. False without a cleanup lane. */
		ExecutionOwners EpochOwners(std::span<const MainPayload* const> a_payloads);
		/** @brief Render thread. Creates the scene buffers once. False when they have no device address. */
		bool EnsureSceneBuffers(rhi::Device a_device);
		std::shared_ptr<SceneBuffers> ShadowSceneBuffers(rhi::Device a_device) override { return EnsureSceneBuffers(a_device) ? scene : nullptr; }
		void ShadowResourcesMade(const std::shared_ptr<ShadowResources>& a_shadow) override
		{
			if (resources)
				resources->shadow = a_shadow;
			if (pendingResources)
				pendingResources->shadow = a_shadow;
		}
		/** @brief Grows the scene's tables and per-object buffers to hold a_tables. */
		void RequireSceneTables(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables);
		ObjectRecordStore objectStore;
		ExtrasStore extrasStore;
		GeometryStore geometryStore;
		std::shared_ptr<MainPayload> AcquirePayload()
		{
			// An idle payload must hold nothing, or it would keep that publication's retirement chain alive.
			std::shared_ptr<MainPayload> free;
			for (auto& payload : payloadPool)
				if (payload.use_count() == 1) {
					payload->Reset();
					if (!free)
						free = payload;
				}
			return free ? free : payloadPool.emplace_back(std::make_shared<MainPayload>());
		}
		// The payloads this frame's main epochs committed. Forward and first person draw the colour one's.
		std::array<const MainPayload*, 2> committedPayload{};
		std::array<std::uint32_t, 2> committedFrame{};
		MainRows mainRows;
		std::array<std::uint32_t, 5> decalWords{};  // count buffer words 19-22, then 27

		/** @brief Render thread. Packs an epoch's per-frame constant blocks from the capture. */
		void PackFrameBlocks(const Draws::Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out);
		// Slots the capture lacks are filled from the last capture.
		void MirrorFrameBlocks(const Draws::Capture& a_capture, bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out);
		void CarryMissingBlocks(bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out);
		// Per main epoch, each frame slot's last captured block, for a frame that lacks it.
		struct CarriedBlocks
		{
			std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
		};
		std::array<CarriedBlocks, 2> carriedBlocks;
		std::uint32_t carriedLogged = 0;  // bit: slot, +8 colour, +16 pixel
		/** @brief Render thread. Snapshots the build's inputs. Without a capture the replayed eye is used. */
		MainInputs PrepareMainInputs(bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask);

		// Per main epoch, the frame slots it last supplied. The builds ahead are made for these.
		struct EpochMasks
		{
			std::uint32_t vsMask = 0, psMask = 0;
			bool known = false;
		};
		std::array<EpochMasks, 2> epochMasks;
		/** @brief Writes the frame textures, blocks and uploads for an epoch. */
		bool CommitMainPayload(const Draws::Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
			const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats,
			std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision);
		void CommitShadowInputs(bool a_depthOnly, const PassFrame& a_shape, std::uint32_t a_revisionEpoch);
		void ResolveFrameTextures(const MainCommit& a_commit, std::array<std::uint32_t, kTextureRegisters>& a_frameTextures);
		void UploadSceneStreams(const MainCommit& a_commit);
		void ZeroCommitCounts(const MainCommit& a_commit);
		std::uint32_t WriteDepthBuckets(const MainCommit& a_commit, const org::RunRef& a_latchRun, BuildDrawsLatch& a_latch);
		void WriteShadowLatch(const MainCommit& a_commit, const org::RunRef& a_latchRun, BuildDrawsLatch& a_latch);
		void WriteClassRegion(const MainCommit& a_commit, const org::RunRef& a_latchRun, BuildDrawsLatch& a_latch);

		// The engine rewrites the per-frame constants between the depth pass and the composite.
		// The colour pass reuses these bytes so its EQUAL test matches the Z-prepass.
		std::array<std::vector<std::byte>, kConstantBufferRegisters> prepassVS;
		// The Z-prepass uses the main pass's viewport depth range so the main pass's EQUAL test matches.
		float mainMinDepth = 0.0f, mainMaxDepth = 0.0f;
		bool prepassInputs = false;

		// Whether the last main pass bound the engine's kMAIN depth, checked against that frame's version. Empty before the first
		// main pass. A slot keeps its field handle across a resize, so this answer does not change with the version.
		std::optional<bool> mainPassBindsMainDepth;
		bool loggedDepthMismatch = false;
		// Colour commits whose sun test ran without the sun's cascades.
		Reports::Window sunTestWindow;
		std::uint32_t sunTestEmpty = 0;
		bool loggedFrameConstants = false;
		// FrameParams.z at the blended draw. It gates Lighting's forward fog. The engine holds 1 there before the first blended draw.
		float blendedFrameParamsZ = 1.0f;

		std::vector<GpuCascade> sunCascades;
		// Unclamped, as the build parity's reference. The latch takes a count clamped to the revision's layout.
		std::vector<GpuShadowVolume> shadowVolumes;
		std::uint32_t shadowVolumesLatched = 0;
		ZBucketPlan zBucketPlan;
		std::vector<std::uint32_t> zBucketZeros;
		std::vector<std::uint32_t> classCountZeros;

		void ServeRevisionRequests();
		/** @brief Render thread. Serves the revision code's requests, polls the build manager, and posts moved revision inputs. */
		void BuildIfAsked();
		/** @brief Render thread. What DCLF's epoch graphs are built from now. A new value only when something changed. */
		std::shared_ptr<const BuildState> CurrentBuildState();
		std::shared_ptr<const BuildState> buildState;
		/** @brief Render thread, after each build manager poll. The newest build state is the built one once no DCLF epoch is pending a build. */
		void SettleBuildState();
		/** @brief Render thread. Builds DCLF's epoch graphs on the scene worker. They are staged and attach with a snapshot at a Begin. */
		void RegisterGraphBuilds();
		bool graphBuildsRegistered = false;
		/** @brief Render thread, in the adoption point. Registers DCLF's graph builds and updates the graph conditions for the adopted variables. */
		void UpdateConditionsAtAdoption();
		/** @brief Render thread, after the scene step. Counts a Begin that took no scene (snapshotFramesKept). */
		void CountSceneKept();
		/**
		 * @brief The sum of DCLF's own epochs' build generations. Other epochs' builds do not invalidate DCLF's recordings.
		 */
		static std::uint64_t GraphBuildGeneration(const org::GraphHost& a_host);
		/** @brief Render thread. Gives back ready tickets of epochs not in a_submitted while recordings are outstanding. */
		void ReleaseTicketsForRecordings(org::GraphHost& a_host, std::uint64_t a_submitted);
		/** @brief Render thread. Posts the revision inputs when anything moved. */
		void PostRevisionInputs() override;
		/** @brief Builder. Makes and assembles a scene revision from the publication and the producer's inputs alone. */
		void MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame);
		void RequireRevisionSet(RevisionMake& a_make, GrowthBuild& a_build);
		void RequireRevisionViews(RevisionMake& a_make, GrowthBuild& a_build);
		void RequireRevisionLatched(RevisionMake& a_make, GrowthBuild& a_build);
		void MakeMainShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout);
		void MakeViewShapes(const RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout);
		/** @brief Registers the latch handles once and lays out each latch once its resources exist. */
		void EnsureRevisionLatches();

		/** @brief Builder. Seals the revision. Does nothing while a needed build is pending. */
		void AssembleRevision(std::uint32_t a_frame);
		/**
		 * @brief The selected revision's shape and recordings for a_epoch. Empty only when coverage let a frame through without them,
		 * and the epoch is then not submitted.
		 */
		EpochRevision RevisionOf(std::uint32_t a_epoch);
		/** @brief Whether the selected revision covers a_epoch and its graph still runs. True without revisions. */
		bool EpochCovered(std::uint32_t a_epoch) const;
		void DecideEpochCoverage(org::GraphHost* a_host);
		void DecideViewCoverage(ViewKind a_kind, org::GraphHost* a_host, bool a_main, std::uint32_t a_frame);
		void SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index);
		std::string RevisionReport();
		using FamilyPipelinesSet = std::array<const FamilyPipelines*, Extensions::kDrawFamilies>;
		/** @brief The key the make's shapes are built from. */
		ShapesKey RevisionShapesKey(const NamedVersions& a_sealed, const RevisionMake& a_make, std::size_t a_pipelines) const;
		void MixMainShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const;
		void MixViewShapesKey(ShapesKeyMixer& a_mixer, const FamilyPipelinesSet& a_families) const;
		// Registered in ShapeKind order, which every loop over the participants relies on.
		struct MainParticipant final : RevisionParticipant
		{
			explicit MainParticipant(Impl& a_owner) :
				owner(a_owner)
			{}
			Impl& owner;
			void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) override { owner.RequireRevisionSet(a_make, a_build); }
			void RequireLatched(RevisionMake& a_make, GrowthBuild& a_build) override { owner.RequireRevisionLatched(a_make, a_build); }
			void MixShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const override { owner.MixMainShapesKey(a_make, a_mixer); }
			void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) override { owner.MakeMainShapes(a_make, a_sealed, a_layout); }
		};
		struct ViewParticipant final : RevisionParticipant
		{
			explicit ViewParticipant(Impl& a_owner) :
				owner(a_owner)
			{}
			Impl& owner;
			void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) override { owner.RequireRevisionViews(a_make, a_build); }
			void MixShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const override;
			void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) override { owner.MakeViewShapes(a_make, a_sealed, a_layout); }
		};
		MainParticipant mainParticipant{ *this };
		ViewParticipant viewParticipant{ *this };
		std::array<RevisionParticipant*, 3> participants{ &mainParticipant, shadowEngine.get(), &viewParticipant };

		std::uint32_t loggedReasons = 0;

		bool NotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((loggedReasons >> a_reason) & 1)) {
				loggedReasons |= 1u << a_reason;
				logger::info("[DCLF] Main-pass draws not ready: {}", a_what);
			}
			return false;
		}

		bool Setup(const Draws::Capture& a_capture, bool a_depthOnly = false);
		bool CreateMainBuffers(Resources& a_state, org::GraphHost& a_host, rhi::Device a_device);
		bool ImportMainTargets(Resources& a_state, const Draws::Capture& a_capture, rhi::Device a_device, const D3D11_TEXTURE2D_DESC& a_target,
			const TargetFormats& a_targets);
	};
}
