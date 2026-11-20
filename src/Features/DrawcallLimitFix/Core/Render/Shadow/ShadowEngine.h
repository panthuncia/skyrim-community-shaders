#pragma once

// The shadow engine holds all state for the shadow and occlusion views: resources, capability and catalog, placements,
// captured views, and the kept build state. It is the shadow revision participant, implemented in ShadowRevision.cpp.
// It reaches the rest of the implementation only through ShadowHost or parameters, never the whole Impl.
// Members are touched from different threads. Each says which.
// Included by Impl.h, before the struct.

namespace DCLF
{
	/** @brief Render thread. Lets slots whose views never draw in the same frame share draw arguments. */
	void ShareDrawArgs(ShadowResources& a_state, std::span<const ShadowViewLayout> a_placements);

	/** @brief What the shadow engine needs from the rest of the implementation. Used at setup and capture, never per object. */
	struct ShadowHost
	{
		/** @brief Render thread. Posts the revision inputs when anything changed. */
		virtual void PostRevisionInputs() = 0;
		/** @brief Render thread. The scene buffers, created once. Null when they have no device address. */
		virtual std::shared_ptr<SceneBuffers> ShadowSceneBuffers(rhi::Device a_device) = 0;
		/** @brief Render thread. The shadow resources were made, so the main resources can sample their maps. */
		virtual void ShadowResourcesMade(const std::shared_ptr<ShadowResources>& a_shadow) = 0;

	protected:
		~ShadowHost() = default;
	};

	struct ShadowEngine final : RevisionParticipant
	{
		explicit ShadowEngine(ShadowHost& a_host) :
			shadowHost(a_host)
		{}
		ShadowEngine(const ShadowEngine&) = delete;
		ShadowEngine& operator=(const ShadowEngine&) = delete;

		ShadowHost& shadowHost;

		// Revision participant steps, in call order.
		void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) override;
		void RequireLatched(RevisionMake& a_make, GrowthBuild& a_build) override;
		void NoteSealed(RevisionMake& a_make) override;
		void MixShapesKey(const RevisionMake& a_make, Draws::ShapesKeyMixer& a_mixer) const override;
		void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) override;
		void PostInputs(RevisionInputs& a_next) override;
		void PrepareAhead(ImplSnapshotBuilder::AheadContext& a_context) override;
		/** @brief Builds the shadow payload's shared half. It needs no region, so it can run beside the main build. */
		void BuildPayloadShared(const AheadBuild& a_build) override;
		/** @brief Builds the shadow payload and its exclusions. Runs the shared half first if it has not run for this build. */
		void BuildPayload(const AheadBuild& a_build) override;
		/** @brief Snapshot builder only. The build ahead's shadow payload between its two halves. */
		struct AheadShadow
		{
			std::shared_ptr<ShadowPayload> payload;
			ShadowInputs in;
			ShadowBuildSplit split;
			bool sharedRegion = false;            // the region half writes into the main build's region
			const void* publication = nullptr;    // the build whose shared half ran
			std::uint32_t frame = 0;
		} aheadShadow;
		/** @brief Snapshot builder only, per make. */
		struct RevisionScratch
		{
			ShadowIndirectState indirect;
			std::vector<LatchedCopy> latched;
		} revisionScratch;
		std::shared_ptr<const ShadowSlotBuffers> postedShadowSlots;  // render thread
		/** @brief Render thread. The slots' buffers and shadow maps as they are now, or null without shadows. */
		std::shared_ptr<const ShadowSlotBuffers> SlotBuffers();

		std::shared_ptr<ShadowResources> shadow;
		bool shadowSetupFailed = false;
		std::uint32_t shadowCullEpochs = 0;
		std::uint32_t shadowLoggedTargets = 0;
		void ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats);
		std::vector<PendingView> pendingViews;
		// Bits by slot - kFirstShadowViewSlot. The revisions record a variant for each recent set, and an epoch uses the narrowest that covers it.
		std::uint64_t lastActiveViews = 0;
		struct OcclusionState
		{
			PendingView view;
			std::uint32_t capturedFrame = ~0u;
			std::uint32_t rasterState = 0;
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			std::uint32_t inputs = 0, skipped = 0;
		};
		std::array<OcclusionState, kOcclusionViews> occlusion;
		/** @brief The maps in a_maps whose view was set up for this frame. */
		std::uint32_t OcclusionSetUp(std::uint32_t a_maps, std::uint32_t a_frame) const
		{
			std::uint32_t out = 0;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				out |= ((a_maps >> v) & 1) && occlusion[v].capturedFrame == a_frame ? 1u << v : 0u;
			return out;
		}
		/** @brief The occlusion views' depth formats. */
		std::array<DXGI_FORMAT, kOcclusionViews> OcclusionFormats() const
		{
			std::array<DXGI_FORMAT, kOcclusionViews> viewFormats{};
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				viewFormats[v] = occlusion[v].dsvFormat;
			return viewFormats;
		}
		/** @brief Counts a frame whose shadow views were not drawn while their casters were withheld from the engine. */
		void ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats);
		// The shadow capability is the set of modes DCLF draws and the state catalog its pipelines cover. Render thread.
		// It changes only on a toggle, a failure, an occlusion map's first capture, or a defect.
		std::array<bool, kShadowModeCount> readyModes{};
		std::array<ModeRasterStates, kShadowModeCount> readyStates{};
		std::atomic<std::uint8_t> shadowCapability{ 0 };
		bool shadowCatalogBuilt = false;
		DXGI_FORMAT shadowTargetFormat = DXGI_FORMAT_UNKNOWN;
		// The shadow pipelines' depth format. Taken from the engine's shadow map once it exists, before DCLF draws, and fixed from then on.
		DXGI_FORMAT pipelineShadowFormat = DXGI_FORMAT_UNKNOWN;
		/** @brief Fills the catalog with every solid-fill engine state a shadow view can use, plus the cascades' clones. */
		void BuildShadowCatalog();
		/** @brief The capability changed, so every caster is decided again. a_defect marks a view the catalog did not foresee. */
		void NoteCapability(bool a_defect, std::string a_cause);
		// Counters since startup.
		struct CapabilityStats
		{
			std::uint32_t changes = 0;
			std::uint32_t defects = 0;
			std::string lastCause;
		} capabilityStats;
		/** @brief The payload the last shadow epoch committed, kept alive until the next commit. */
		std::shared_ptr<ShadowPayload> committedShadow;
		std::vector<std::shared_ptr<ShadowPayload>> shadowPayloadPool;  // snapshot builder only
		std::shared_ptr<ShadowPayload> AcquireShadowPayload();
		std::shared_ptr<const void> shadowExecutionOwner;  // kept alive until the next execution
		/** @brief The modes, states and target format the builds ahead and the shadow lookups were made for. */
		struct LastShadow
		{
			std::array<bool, kShadowModeCount> modes{};
			std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			bool known = false;
		} lastShadow;
		// Inputs for the scene lane's shadow pipelines, latest wins. postedShadowLookups is the render thread's copy.
		std::atomic<std::shared_ptr<const ShadowLookupInputs>> shadowLookupInputs;
		ShadowLookupInputs postedShadowLookups;
		/**
		 * @brief Every place the engine's shadow maps can put a view, built at setup. The shadow shape covers all of them.
		 * A view that matches no placement is a defect and is flagged.
		 */
		std::vector<ShadowViewLayout> shadowPlacements;
		bool BuildShadowPlacements();
		/** @brief The placement a captured view drew at, or SIZE_MAX. */
		std::size_t PlacementOf(const PendingView& a_view) const;
		std::uint32_t placementDefectsLogged = 0;
		/**
		 * @brief The occlusion maps' layouts as last set up, filtered by a_toggles.
		 * Null toggles returns every set-up map, which is what the revision input uses.
		 */
		std::vector<ShadowViewLayout> PredictedOcclusion(const ToggleSet* a_toggles) const;
		// How many recent sets of drawn views get their own variant.
		static constexpr std::size_t kRecentActiveViews = 4;
		/** @brief The buckets of a rasterizer state's map row, cached. */
		static const RowBuckets& ShadowRowBuckets(RowBucketsCache& a_cache, std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups,
			const ShadowIndirectState& a_indirect);
		// Revision producer steps. Snapshot builder only.
		/** @brief Requires every shadow view slot's sequences to hold the scene's draw bound. */
		static void RequireShadowSequences(ImplProducer& a_producer, GrowthBuild& a_build);
		/** @brief Grows view slots and map rows as needed. More slots rebuild the graph, and any growth means a new latch layout. */
		static void ReserveShadowLatch(ImplProducer& a_producer, GrowthBuild& a_build, ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys,
			std::uint32_t a_rasterStates, std::uint32_t a_sunProcesses);
		/** @brief Makes the shadow shape and its variants. The placements come first, then the occlusion map views. */
		static void MakeShadowShapes(ImplProducer& a_producer, const NamedVersions& a_sealed, const ShadowIndirectState& a_indirect, const Lookups& a_lookups,
			const std::vector<LatchedCopy>& a_latched);
		/**
		 * @brief Whether the last shadow shapes can hold these bounds. Capacities only grow,
		 * so a shape remade from bounds it already holds would be the same shape.
		 */
		static bool ShadowShapesHold(const ImplProducer& a_producer, const ShadowBounds& a_bounds);
		ShadowInputs PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const;
		void NoteShadowViewModes(std::array<bool, kShadowModeCount>& a_modeUsed, std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates);
		void NoteShadowEpochDrawn(const ShadowPayload& a_payload, const std::array<bool, kShadowModeCount>& a_modeUsed, std::uint32_t a_frame,
			std::uint32_t a_mapsDrawn, double a_occlusionMs, IndirectDraws::ShadowStats& a_stats);
		std::uint32_t shadowLoggedReasons = 0;
		bool ShadowNotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((shadowLoggedReasons >> a_reason) & 1)) {
				shadowLoggedReasons |= 1u << a_reason;
				logger::info("[DCLF] shadow views not ready: {}", a_what);
			}
			return false;
		}
		bool SetupShadow();
		bool ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target);
		/**
		 * @brief Imports a texture as depth target a_index. For a graph-owned target, pass a_graph
		 * so the same image is not imported twice.
		 */
		bool ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label,
			std::shared_ptr<org::ExternalTextureResource> a_graph = nullptr);

		ShadowKept shadowKept;
		ShadowKept* ShadowKeptState() { return &shadowKept; }
	};
}
