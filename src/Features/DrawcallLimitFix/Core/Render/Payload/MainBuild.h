#pragma once

// The main build's types, the scene list and the build cache.
// The builds run on DCLF's pool, one at a time, driven by the snapshot builder.

namespace DCLF::Draws
{
	/**
	 * @brief A material and pipeline pair's verdict for one build. Its rows, or why it cannot draw.
	 * Skip counters are per draw, so every draw of a failing pair raises its reason.
	 */
	struct ResolvedBindings
	{
		std::uint32_t recordIndex = kNoRecord;  // RowsOf(pipeline, material)
		std::uint32_t skipReason = kNoSkip;
		bool deferred = false;
		std::uint32_t missingTexture = 0;  // the register that failed, for the skip sample
	};

	// The PerGeometry group, packed once per pipeline. Objects on a pipeline differ in only five variables,
	// so each object copies this and rewrites those instead of walking the variable table twice.
	struct GeometryTemplate
	{
		std::vector<std::byte> vs;
		std::vector<std::byte> ps;
		GeometryPatchOffsets offsets;
	};

	/**
	 * @brief A pipeline's PerGeometry template. Under DCLF_BINDLESS, values the draws never read from the block pack as zero,
	 * so the bytes change only with the pipeline's own values.
	 */
	inline void PackGeometryTemplate(const GeometryConstants& a_constants, std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable, bool a_bindless,
		GeometryTemplate& a_out)
	{
		const auto vsSize = ConstantGroupSize(LightingVSLayout(), a_vsTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry]);
		const auto psSize = ConstantGroupSize(LightingPSLayout(), a_psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry]);
		a_out.vs.assign(std::max<std::size_t>(vsSize, 16), std::byte{});
		a_out.ps.assign(std::max<std::size_t>(psSize, 16), std::byte{});
		const std::uint64_t vsUnread = a_bindless ? kVSBindlessGeometryUnread : 0, psUnread = a_bindless ? kPSBindlessGeometryUnread : 0;
		PackConstantGroup(a_constants.vs, LightingVSLayout(), a_vsTable, kVSGroups[kPerGeometry] & ~vsUnread, kVSFirstVariable[kPerGeometry], a_out.vs);
		PackConstantGroup(a_constants.ps, LightingPSLayout(), a_psTable, kPSGroups[kPerGeometry] & ~psUnread, kPSFirstVariable[kPerGeometry], a_out.ps);
		a_out.offsets = GeometryPatchOffsetsOf(a_vsTable, a_psTable);
		a_out.vs.resize(vsSize);
		a_out.ps.resize(psSize);
	}

	constexpr std::uint32_t kNoRegion = ~0u;
	// A drawable decal's sequence is its group's slot, not a draw.
	constexpr std::uint8_t kRegionDecal = 0x80;
	constexpr std::uint8_t RegionDraws(std::uint8_t a_draws) { return a_draws & 0x7F; }
	constexpr std::uint64_t kNoPair = ~0ull;  // a region entry without bindings (a cull-only candidate)

	// ---- The scene list. One entry per object, whose main part the main build writes and whose shadow words the shadow build writes.
	// A main view bit missing here is dropped when an entry's parts merge, and BuildDraws then skips that view's input entirely.
	constexpr std::uint32_t kMainCoreSideBits = kViewMainCull | kViewMain | kViewDecal | kViewForward | kViewRefraction | kViewWater;
	inline std::uint32_t MainSideBits() { return kMainCoreSideBits | Extensions::Views().viewBits; }
	constexpr std::uint8_t kSideMain = 1;
	constexpr std::uint8_t kSideShadow = 2;
	/** @brief Sets a scene entry's main part and keeps its shadow words. */
	inline void SetMainPart(DrawInput& a_entry, const DrawInput& a_main)
	{
		const DrawInput::ViewWords view = a_entry.view;
		const std::uint32_t shadowRow = a_entry.shadowRow;
		const std::uint32_t mainSide = MainSideBits();
		a_entry = a_main;
		a_entry.view = view;
		a_entry.view.mask = (a_main.view.mask & mainSide) | (view.mask & ~mainSide);
		a_entry.shadowRow = shadowRow;
	}
	/** @brief Clears the main build's part of a scene entry. The object's fields stay. */
	inline void ClearMainPart(DrawInput& a_entry)
	{
		a_entry.pipelineIndex = a_entry.recordIndex = a_entry.decalOrdinal = 0;
		a_entry.fadeDistance = 0.0f;
		a_entry.flags &= ~kInputDrawable;
		a_entry.view.mask &= ~MainSideBits();
	}
	/** @brief Sets a scene entry's shadow words and keeps its main part. Without a main part, the object fields come from a_shadow too. */
	inline void SetShadowPart(DrawInput& a_entry, const DrawInput& a_shadow, bool a_mainHeld)
	{
		if (!a_mainHeld) {
			a_entry = a_shadow;
			ClearMainPart(a_entry);
			a_entry.view.mask = a_shadow.view.mask & ~MainSideBits();
			return;
		}
		const std::uint32_t mask = a_entry.view.mask;
		const std::uint32_t mainSide = MainSideBits();
		a_entry.view = a_shadow.view;
		a_entry.view.mask = (mask & mainSide) | (a_shadow.view.mask & ~mainSide);
		a_entry.shadowRow = a_shadow.shadowRow;
	}
	/** @brief Clears the shadow build's part of a scene entry. */
	inline void ClearShadowPart(DrawInput& a_entry)
	{
		a_entry.view.shadowKey = a_entry.view.occlusionKey[0] = a_entry.view.occlusionKey[1] = ~0u;
		a_entry.shadowRow = 0;
		a_entry.view.mask &= MainSideBits();
	}
	/** @brief A shadow input stripped of the main part and row number, for comparing two builds, which number rows differently. */
	inline DrawInput ShadowPartOf(DrawInput a_input)
	{
		a_input.pipelineIndex = a_input.recordIndex = a_input.decalOrdinal = a_input.shadowRow = 0;
		a_input.fadeDistance = 0.0f;
		a_input.flags &= ~(kInputDrawable | (ObjectFocusOnly(a_input.flags) ? 0u : kObjectDecal) | kObjectForward);
		a_input.view.mask &= ~MainSideBits();
		return a_input;
	}

	/** @brief Draw inputs kept densely by object slot at the head of an input buffer. Removing an entry moves the last one into its place. */
	struct KeptRegion
	{
		KeptArray<DrawInput> inputs;
		std::vector<std::uint32_t> indexOf;  // per object slot: its entry, or kNoRegion

		std::uint32_t EntryOf(std::size_t a_object) const { return a_object < indexOf.size() ? indexOf[a_object] : kNoRegion; }
		bool Holds(std::size_t a_object) const { return EntryOf(a_object) != kNoRegion; }
		void Cover(std::size_t a_objects)
		{
			if (indexOf.size() < a_objects)
				indexOf.resize(a_objects, kNoRegion);
		}
		/** @brief Adds a marked entry for a_object at the end and returns its index. */
		std::uint32_t Add(std::uint32_t a_object, const DrawInput& a_input = {})
		{
			Cover(std::size_t(a_object) + 1);
			auto& list = inputs.Mutable();
			const auto i = static_cast<std::uint32_t>(list.size());
			list.push_back(a_input);
			indexOf[a_object] = i;
			inputs.Mark(i);
			return i;
		}
		/** @brief The result of removing an entry. The last entry moves into its place. kNoRegion when a_object had none. */
		struct Removal
		{
			std::uint32_t at = kNoRegion, from = kNoRegion;
		};
		Removal Remove(std::uint32_t a_object)
		{
			const std::uint32_t i = EntryOf(a_object);
			if (i == kNoRegion)
				return {};
			auto& list = inputs.Mutable();
			const auto tail = static_cast<std::uint32_t>(list.size() - 1);
			if (i != tail) {
				list[i] = list[tail];
				indexOf[list[i].objectIndex] = i;
				inputs.Mark(i);
			}
			list.pop_back();
			indexOf[a_object] = kNoRegion;
			return { i, tail };
		}
	};

	/**
	 * @brief One main segment's resident draws, dense at the head of its input buffer.
	 * Only the main build writes it, from the tables' change feed. Payloads share the inputs copy-on-write.
	 */
	struct ResidentRegion : KeptRegion
	{
		LogCursor cursor;               // the change log, and the tables generation it was read from
		std::uint8_t segments = 0;      // the segments its pairs' verdicts are of (MainBuild::Segments)
		std::vector<std::uint64_t> pairOf;   // per entry: its (material, pipeline)
		// Per entry, its sequences, or 0 when it cannot draw this frame. A drawable decal is kRegionDecal.
		std::vector<std::uint8_t> drawsOf;
		struct Pair
		{
			std::uint32_t slot = 0;   // its rows (RowsOf), once it can draw
			// The objects using this pair, and its place in its material's and pipeline's lists so removal needs no search.
			std::vector<std::uint32_t> objects;
			std::uint32_t materialAt = 0, pipelineAt = 0;
			bool ok = true;           // this build found its rows can draw it
			// What the last resolution read. It is resolved again only when this changes. 0 means never resolved.
			std::uint64_t witness = 0;
			std::uint32_t skip = 0;  // its skip reason when it cannot draw (the report counts it every build)
			std::array<std::uint64_t, 2> frameRegisters{};  // the frame textures its resolution asked for
			// The owners the region's bundle holds for this pair, refcounted in ownerCounts.
			const void* bindingSeen = nullptr;
			const void* maskSeen = nullptr;
		};
		ankerl::unordered_dense::map<std::uint64_t, Pair> pairs;
		// The frame inputs the resolutions read, plus every pair's frame textures and binding owners, kept by count.
		// The owner bundle is rebuilt only when an owner joins or leaves.
		std::uint64_t frameWitness = 0;
		std::uint64_t rowsClears = 0;  // MainRows::clears when its pairs last wrote their rows
		std::array<std::uint64_t, 2> frameRegisters{};
		std::array<std::uint32_t, 128> frameRegisterCounts{};
		ankerl::unordered_dense::map<const void*, std::pair<std::shared_ptr<const void>, std::uint32_t>> ownerCounts;
		std::shared_ptr<const void> owners;
		bool ownersChanged = true;
		// A build checks pairs of materials the logs name, new pairs, and pairs of pipelines whose witness moved.
		// A log it cannot continue, or a moved frame witness, means every pair.
		LogCursor materialCursor, lookupCursor;
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint64_t>> materialPairs;  // material -> its pairs
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint64_t>> pipelinePairs;  // pipeline -> its pairs
		std::vector<std::uint32_t> pairObjectAt;  // per object slot: its place in its pair's objects
		std::vector<std::uint64_t> freshPairs;
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> pipelineWitness;  // the last build's, per pipeline
		std::vector<std::uint64_t> pipelineWitnessNow;  // this build's, per pipeline slot (kept for its storage)
		// Pairs that cannot draw, by skip reason, so the report need not visit them.
		std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipCounts{};
		ankerl::unordered_dense::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pipelines;  // pipeline -> (set index, entries)
		std::size_t draws = 0;
		std::size_t decals = 0;               // drawable decal entries
		std::size_t undrawable = 0;           // entries with no draw this frame
		// Stream indices are relative to the geometry slot count. When it or the stream buffer changes, every stream object is redone.
		// ~0 means not yet read.
		std::size_t streamBase = ~std::size_t(0);
		SceneFit fit;  // what the scene's buffers held at its last build: their growth reads every slot again
		std::array<std::uint32_t, kDecalGroups> decalCount{};  // the decal ranges it was built for (MainBuild::decalCount)
		std::vector<std::uint32_t> touched;  // this build: the slots whose entry it wrote or removed, or whose log entry it read
		// Whole-scene mode puts every drawable object, cull-only candidate, decal and face shape in the list, not only residents.
		// The few that cannot be listed go to the per-frame loop.
		bool wholeScene = false;
		std::vector<std::uint32_t> loopList;   // the slots the loop visits
		std::vector<std::uint32_t> loopIndex;  // per slot: its place in loopList, or kNoRegion
		std::vector<std::uint8_t> candidate;   // per slot: a culling candidate only (no bindings, or shadow-only)
		std::size_t candidates = 0;

		// Which builds' parts each entry holds. The shadow build writes its words after the main build, and an entry stays while either holds it.
		std::vector<std::uint8_t> sideOf;
		std::uint64_t resets = 0;  // counts Reset: the shadow build writes its parts again after one (ShadowKept::sceneResets)
		bool begun = false;        // this publication's build of the journal began (Begin; the builds ahead clear it)
		bool MainHolds(std::size_t a_object) const
		{
			const std::uint32_t i = EntryOf(a_object);
			return i != kNoRegion && (sideOf[i] & kSideMain);
		}
		/** @brief Begins this publication's journal build. Called by whichever build writes first. */
		void Begin(std::uint64_t a_oldestContentEpoch)
		{
			if (!begun)
				inputs.BeginBuild(a_oldestContentEpoch);
			begun = true;
		}
		/** @brief Adds an entry holding a_side's part. */
		std::uint32_t AddEntry(std::uint32_t a_object, const DrawInput& a_input, std::uint8_t a_side)
		{
			const std::uint32_t i = Add(a_object, a_input);
			pairOf.push_back(kNoPair);
			drawsOf.push_back(0);
			sideOf.push_back(a_side);
			return i;
		}
		/** @brief Removes an entry. The last entry moves into its place. */
		void RemoveEntry(std::uint32_t a_object)
		{
			const auto removal = Remove(a_object);
			if (removal.at == kNoRegion)
				return;
			pairOf[removal.at] = pairOf[removal.from];
			drawsOf[removal.at] = drawsOf[removal.from];
			sideOf[removal.at] = sideOf[removal.from];
			pairOf.pop_back();
			drawsOf.pop_back();
			sideOf.pop_back();
		}
		/** @brief Empties the region so every slot is read again. The inputs' journal keeps counting. */
		void Reset()
		{
			auto kept = std::move(inputs);
			const std::uint64_t resetsBefore = resets;
			const bool begunBefore = begun;
			*this = ResidentRegion{};
			inputs = std::move(kept);
			inputs.Clear();
			resets = resetsBefore + 1;
			begun = begunBefore;
		}
	};

	/** @brief What the main build keeps across publications. Used by one build at a time. */
	struct BuildCache
	{
		ResidentRegion region;
		// Each body is held by its payloads. One no payload holds is reused by the next build.
		std::vector<std::shared_ptr<MainBody>> bodies;
	};

	/**
	 * @brief Builds the main-pass epoch's draws from the tables and lookups. Pure.
	 * An entry the render thread has not resolved yet defers the draw. The frame's own textures are left for the commit to patch.
	 */
	void BuildMainPayload(const MainInputs& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups, MainPayload& a_out, MainRows& a_rows,
		BuildCache* a_cache = nullptr, std::shared_ptr<const StreamViews> a_streams = nullptr);
	/** @brief Builds both main segments from one list, so a member draws in both or neither. A segment with no inputs is skipped. */
	void BuildMainPayloads(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
		const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams);
	/**
	 * @brief BuildMainPayloads split in two, so the stream views can be made alongside it. Finish is the part that reads them.
	 * Call Run then Finish once each, from one thread at a time.
	 */
	class MainBuildStages
	{
	public:
		MainBuildStages(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
			const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache);
		~MainBuildStages();
		MainBuildStages(const MainBuildStages&) = delete;
		MainBuildStages& operator=(const MainBuildStages&) = delete;
		void Run();
		void Finish(std::shared_ptr<const StreamViews> a_streams);

	private:
		struct State;
		std::unique_ptr<State> state;
	};

	// Shadow mode 0xD clips casters in front of the near plane. It is a spot light whose near clip the clamp would defeat.
	constexpr std::uint32_t kPlainShadowMode = 0xD - PassCapture::kFirstShadowMode;
	constexpr std::uint32_t kSunShadowMode = 0xE - PassCapture::kFirstShadowMode;
	constexpr std::uint32_t kParabolicShadowMode = 0xF - PassCapture::kFirstShadowMode;
}
