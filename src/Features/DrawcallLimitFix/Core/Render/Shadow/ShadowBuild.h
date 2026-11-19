#pragma once

// Types and helpers for the shadow build. It covers caster classes, view bits and keys, and the shadow state kept
// across frames. The build runs on DCLF's thread pool. Internal.h includes this file in a fixed order.

namespace DCLF::Draws
{
	/**
	 * @brief Whether the object is drawn only by the sun's volumetric lighting views in this mode.
	 * Point lights treat volumetric-only casters as ordinary casters.
	 */
	inline bool VolumetricClass(std::uint32_t a_mode, std::uint32_t a_flags)
	{
		return !IsOcclusionMode(a_mode) && a_mode != kParabolicShadowMode && (a_flags & kObjectVolumetricOnly) != 0;
	}

	/** @brief The technique bits a mode adds to an object's base technique. Occlusion techniques are already complete. */
	inline std::uint32_t ModeBitsOf(std::uint32_t a_mode)
	{
		return IsOcclusionMode(a_mode) ? 0u : ShadowModeBits(PassCapture::kFirstShadowMode + a_mode);
	}

	/** @brief The technique the object draws with in this mode, or 0 if it does not draw. */
	inline std::uint32_t ModeTechnique(const SceneTables& a_tables, std::uint32_t a_mode, std::size_t a_object)
	{
		if (IsOcclusionMode(a_mode)) {
			const auto& column = a_tables.occlusionTechnique[OcclusionOfMode(a_mode)];
			return a_object < column.size() ? column[a_object] : 0u;
		}
		return (a_tables.objects[a_object].flags & kObjectNoShadow) ? 0u : (a_tables.shadowTechnique[a_object] | ModeBitsOf(a_mode));
	}

	/** @brief The kView* bits of every view that draws the given phase mask. */
	inline std::uint32_t ViewBitsOf(std::uint8_t a_phases)
	{
		return Extensions::ResolvedViewBits().viewBitsOfPhases[a_phases];
	}
	/** @brief An input's view mask from the object's phases, plus kViewMainCull for a main candidate. */
	inline std::uint32_t ViewMaskOf(const SceneTables& a_tables, std::size_t a_object, bool a_mainCandidate)
	{
		const std::uint8_t phases = a_object < a_tables.setPhases.size() ? a_tables.setPhases[a_object] : std::uint8_t{ 0 };
		return ViewBitsOf(phases) | (a_mainCandidate ? kViewMainCull : 0u);
	}
	/**
	 * @brief A main input's view mask. kViewMain is set only while the object is a member, so anything the depth
	 * segment draws, the colour segment draws too.
	 */
	inline std::uint32_t MainMaskOf(const SceneTables& a_tables, std::size_t a_object)
	{
		const std::uint32_t flags = a_object < a_tables.objects.size() ? a_tables.objects[a_object].flags : 0u;
		// Root-view members such as first-person arms get only their own view bit. They must stay out of the
		// world's depth segment and the HZB.
		if (a_object < a_tables.setPhases.size())
			if (const auto root = static_cast<std::uint8_t>(a_tables.setPhases[a_object] & Extensions::Views().rootPhases))
				return ViewBitsOf(root);
		if (ObjectDecalGroup(flags))
			return kViewDecal;
		// Forward objects never feed the depth segment. The engine's blended draws write no depth, so they occlude nothing.
		if (flags & kObjectForward) {
			const std::uint32_t pipeline = a_tables.objects[a_object].pipelineIndex;
			if (!(flags & kObjectNoBindings) && pipeline < a_tables.pipelines.size() && RasterRefraction(a_tables.pipelines[pipeline].rasterFlags))
				return kViewRefraction;
			return kViewForward;
		}
		// Only the main-side bits. The shadow build writes the shadow view bits.
		return (ViewMaskOf(a_tables, a_object, true) & MainSideBits() & ~kViewMain) | ((flags & kObjectMember) ? kViewMain : 0u);
	}
	/** @brief The view bits a shadow or occlusion view draws. */
	inline std::uint32_t ShadowViewBits(std::uint32_t a_mode, bool a_sun)
	{
		if (IsOcclusionMode(a_mode))
			return Extensions::OcclusionMapOf(OcclusionOfMode(a_mode)).viewBit;
		if (a_mode == kParabolicShadowMode)
			return kViewPointCaster;
		return a_sun ? kViewSunCaster : kViewSpotCaster;
	}
	/** @brief Every view bit a mode's views can draw. */
	inline std::uint32_t ModeViewBits(std::uint32_t a_mode) { return ShadowViewBits(a_mode, true) | ShadowViewBits(a_mode, false); }
	/** @brief Which key word a mode's views read, already shifted by kCullKeyShift. */
	inline std::uint32_t KeyWordOf(std::uint32_t a_mode)
	{
		return (IsOcclusionMode(a_mode) ? 2u + OcclusionOfMode(a_mode) : 1u) << kCullKeyShift;
	}
	/** @brief The key slot a mode's views draw a shadow input by. */
	inline std::uint32_t ShadowKeyOf(std::uint32_t a_mode, const DrawInput::ViewWords& a_view)
	{
		return IsOcclusionMode(a_mode) ? a_view.occlusionKey[OcclusionOfMode(a_mode)] : a_view.shadowKey;
	}
	inline std::uint32_t ShadowKeyOf(std::uint32_t a_mode, const DrawInput& a_input) { return ShadowKeyOf(a_mode, a_input.view); }

	/** @brief The technique the object's key slot names, without mode bits so all caster modes share one slot. 0 means none. */
	inline std::uint32_t BaseTechnique(const SceneTables& a_tables, std::uint32_t a_mode, std::size_t a_object)
	{
		if (IsOcclusionMode(a_mode)) {
			const auto& column = a_tables.occlusionTechnique[OcclusionOfMode(a_mode)];
			return a_object < column.size() ? column[a_object] : 0u;
		}
		return (a_tables.objects[a_object].flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[a_object];
	}

	/** @brief The ORed techniques of every used occlusion view the object draws into. */
	inline std::uint32_t OcclusionTechniques(const SceneTables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed, std::size_t a_object)
	{
		std::uint32_t techniques = 0;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (a_modeUsed[OcclusionModeOf(v)] && a_object < a_tables.occlusionTechnique[v].size())
				techniques |= a_tables.occlusionTechnique[v][a_object];
		return techniques;
	}

	// Whether the sun's cascade culls never reach this object. Diagnostics only, on the render thread.
	// The shadow checks compare it with the GPU-side test made on the placement row.
	inline bool OutsideSunEntry(const ShadowInputs& a_in, const SceneTables& a_tables, std::size_t a_object)
	{
		const auto* node = a_object < a_tables.listedRoot.size() ? a_tables.listedRoot[a_object] : nullptr;
		if (!node || node->worldBound.radius < 0.0f)
			return false;
		const auto& bound = node->worldBound;
		const float entry[4]{ bound.center.x, bound.center.y, bound.center.z, bound.radius };
		return OutsideSunEntryProcesses(a_in.sunEntryProcesses, entry);
	}

	/**
	 * @brief Shadow inputs and material rows kept across frames, updated only from the change log and from waits that resolve.
	 * Each alpha-tested material holds a row slot. A slot past the table's capacity waits a frame for the table to grow.
	 */
	struct ShadowKept
	{
		static constexpr std::uint32_t kWaiting = ~0u - 1;  // an object's record while its material's texture is not resolved
		static constexpr std::uint32_t kNoRecord = ~0u;
		LogCursor cursor;
		const void* identity = nullptr;
		SceneFit fit;  // scene buffer sizes at the last build. Growth forces every object to be read again.
		std::uint64_t build = 0;
		KeptArray<ShadowMaterialRow> rows;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> slotOf;
		std::vector<const RE::BSShaderMaterial*> slotMaterial;
		std::vector<ID3D11ShaderResourceView*> slotDiffuse;
		std::vector<std::uint32_t> slotRefs;
		std::vector<std::uint8_t> slotReady;
		std::vector<std::shared_ptr<const void>> slotOwner;  // keeps the diffuse texture alive while the row names it
		std::vector<std::uint32_t> freeSlots;  // min-heap, so the table stays dense
		// Rows are rewritten only on events. A texture transform stays watched for 2 builds after its last change,
		// so both row buffers catch up.
		std::vector<std::uint32_t> rowDirty;
		std::vector<std::uint8_t> rowDirtyMark;
		std::vector<std::uint32_t> transformWatch;
		std::vector<std::uint64_t> transformWatchBuild;  // build of the slot's last event, 0 when unwatched
		std::uint64_t lookupsGeneration = ~0ull;
		std::shared_ptr<const std::vector<std::shared_ptr<const void>>> owners;
		bool ownersChanged = true;
		std::vector<std::uint32_t> objectRecord;  // material row, 0 for the plain row, or kNoRecord or kWaiting
		std::vector<const RE::BSShaderMaterial*> objectMaterial;
		struct Mode
		{
			bool active = false;
			ModeRasterStates rasterStates;
			std::vector<std::uint32_t> waiting;  // objects waiting for a pipeline or a texture
			std::vector<std::uint8_t> waitingMark;
			std::vector<std::uint32_t> faces;    // objects the per-frame list writes
			std::vector<std::uint8_t> faceMark;
			// The build at which an object last joined or left this mode. The sun exclusion reuse reads it.
			std::uint64_t membership = 0;
			/** @brief Empty, for every object to be read again. */
			void Reset() { *this = Mode{}; }
		};
		std::array<Mode, kShadowModeCount> modes;
		// One shadow list for all modes. Each entry's mask holds the views of the modes that hold the object.
		KeptRegion scene;
		std::vector<std::uint8_t> heldModes;  // bit m set when mode m holds the object
		std::vector<std::uint8_t> touchedMark;
		std::uint64_t sceneResets = ~0ull;
		/**
		 * @brief Per-mode draw counts of the scene region, kept across builds.
		 * Each build recounts only the entries changed since the last one.
		 */
		struct ModeCounts
		{
			struct Entry
			{
				DrawInput::ViewWords view;  // what the entry was counted as. A zero mask counts nothing.
				std::uint32_t partitions = 0;
			};
			const void* source = nullptr;
			std::uint64_t contentEpoch = 0;  // 0 means the region is counted whole every build
			std::array<std::uint32_t, kShadowModeCount> modeBits{};
			std::vector<Entry> entries;
			std::array<std::uint64_t, kShadowModeCount> draws{};
			std::array<std::uint32_t, kShadowModeCount> inputs{};
			std::array<std::vector<std::uint32_t>, kShadowModeCount> keyDraws, keyInputs;  // per mode, by key slot
		} modeCounts;
		bool Holds(std::uint32_t a_mode, std::uint32_t a_object) const { return a_object < heldModes.size() && ((heldModes[a_object] >> a_mode) & 1); }
		/** @brief Empty, for every object to be read again. The journals and report counters carry on. */
		void Reset()
		{
			auto keptRows = std::move(rows);
			auto keptInputs = std::move(scene.inputs);
			const auto counters = std::tuple{ build, builds, entriesWritten, rowsWritten, resyncs, entriesCounted };
			*this = ShadowKept{};
			rows = std::move(keptRows);
			rows.Clear();
			scene.inputs = std::move(keptInputs);
			scene.inputs.Clear();
			std::tie(build, builds, entriesWritten, rowsWritten, resyncs, entriesCounted) = counters;
		}
		// Counters since the last report.
		std::uint64_t builds = 0, entriesWritten = 0, rowsWritten = 0, resyncs = 0, entriesCounted = 0;
	};

	/**
	 * @brief Builds the shadow payload's shared part and per-mode inputs. Per-view blocks are written at commit,
	 * because the engine captures the views while it draws them, after this build may have started.
	 */
	void BuildShadowPayload(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		std::shared_ptr<const StreamViews> a_streams = nullptr, ShadowKept* a_kept = nullptr, ResidentRegion* a_scene = nullptr);

	/** @brief What the shared half of the shadow build passes to the region half. The vectors are reused across builds. */
	struct ShadowBuildSplit
	{
		std::vector<std::uint32_t> changed;
		std::vector<std::uint32_t> touched;  // in touch order, which sets the region's entry order
		std::uint64_t build = 0;
		std::uint32_t transformBuffer = 0;
		bool resync = false;   // the kept state was emptied, so the region half clears the shadow words first
		bool pending = false;  // the shared half ran and the region half has not
	};

	/**
	 * @brief BuildShadowPayload in two halves, run in order. The shared half never touches the main build's region,
	 * so it can run beside the main build. The region half then writes the shadow words into that region.
	 */
	void BuildShadowPayloadShared(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		std::shared_ptr<const StreamViews> a_streams, ShadowKept* a_kept, bool a_sharedRegion, ShadowBuildSplit& a_split);
	void BuildShadowPayloadRegion(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept* a_kept,
		ResidentRegion* a_scene, ShadowBuildSplit& a_split);

	/**
	 * @brief The frame-side inputs the shadow pipelines depend on. Posted when they change and read by the scene lane, latest wins.
	 * While `enabled` is false the shadow pipelines are left as they are.
	 */
	struct ShadowLookupInputs
	{
		bool enabled = false;
		std::array<bool, kShadowModeCount> modes{};
		std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
		std::array<DXGI_FORMAT, kOcclusionViews> occlusionFormats{};

		bool operator==(const ShadowLookupInputs&) const = default;
	};

	/** @brief Fills the pipeline lookups for every pipeline slot the tables use. Writes only entries that changed. */
	void ResolvePipelineLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups);

	/** @brief Resolves the shadow pipelines of the modes in use, from this catalog only. The catalog is published with the lookups. */
	void ResolveShadowLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, const ShadowLookupInputs& a_inputs, LookupsResolveState& a_state,
		Lookups& a_lookups);

	// Resync reasons for the resident region, in order: log, segment, shrunk, scope, fit.
	constexpr std::size_t kResyncReasons = 5;
	constexpr std::size_t kAsyncColour = 0;
	constexpr std::size_t kAsyncZPrepass = 1;
	constexpr std::size_t kAsyncShadow = 2;
}
