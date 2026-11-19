#pragma once

// Stores kept across frames and journalled, so a buffer is sent only what changed since its content epoch.
// Also the per-frame stream views made from them.

namespace DCLF::Draws
{

	// What a build addresses in an epoch's buffers. A change means the resources were recreated.
	struct ResourceAddresses
	{
		// The payload's tables are not here. The epochs take them from the installed payload.
		std::uint64_t constants = 0, frameConstants = 0;
		RenderGraph::BufferRef streamBuffer;  // the vertex stream's buffer view ref (SceneBuffers::vertexStream) while its entry class is taken
		std::uint32_t placementsIndex = 0;  // the frame's placement rows (FrameValues::PlacementsIndex)
		std::uint32_t palettesIndex = 0;    // the frame's palettes (FrameValues::PalettesIndex)
		std::uint32_t shadingIndex = 0;     // the frame's shading rows (FrameValues::ShadingIndex)
		Extensions::FrameBindingValues frameBindings{};
		std::uint32_t sequenceDecals = 0;    // the sequences' decal range per group (MainLayout): a decal past it waits for their growth
		SceneFit fit;                        // what the scene's buffers hold (an object past them waits)
		const void* identity = nullptr;

		bool operator==(const ResourceAddresses&) const = default;
	};

	/**
	 * @brief Per-object records kept across frames. A record is rewritten only when the change log names a column it uses.
	 * MakeStreamViews should never run twice at once. `collisions` counts it if it ever does.
	 */
	struct ObjectRecordStore
	{
		LogCursor cursor;
		MarkedList changedObjects;
		KeptArray<BindlessObject> records;
		std::atomic<std::uint32_t> busy{ 0 };
		// Counters since the last report.
		std::uint64_t updates = 0, rewritten = 0, resyncs = 0, collisions = 0;
		// Updates with only per-frame placement and palette changes, versus updates with other causes.
		std::uint64_t streamOnly = 0, structural = 0;
		std::array<std::uint64_t, kChangeCauseCount> byCause{};
	};

	/**
	 * @brief A view rasterizer state's buckets. Depth-only pipelines come first so alpha-tested casters fail the depth test sooner.
	 * A pipeline published after the epoch's state was taken has no bucket and draws nothing this frame.
	 */
	struct RowBuckets
	{
		std::vector<std::uint32_t> pipelines;     // by bucket
		std::vector<std::uint32_t> bucketOfSlot;  // by key slot: its bucket, or Lookups::kNone
		bool operator==(const RowBuckets&) const = default;
	};

	/**
	 * @brief The draws the scene's objects can produce, kept up to date from the change log.
	 * The revision producer owns it and updates it once per make. Others only read it.
	 */
	struct DrawBoundStore
	{
		static constexpr std::uint32_t kNoPipeline = ~0u;
		LogCursor cursor;
		bool partitioned = false;                 // the tables had a skin partition column (SceneDrawBound reads it only then)
		std::vector<std::uint32_t> produced;      // by object slot: its draws (a free slot's none)
		std::vector<std::uint32_t> pipeline;      // by object slot: the pipeline slot they count toward (kNoPipeline: a decal's)
		std::vector<std::uint64_t> perPipeline;   // by pipeline slot
		std::uint64_t draws = 0;
		/** @brief What an object casts with in each shadow mode, and its draws. */
		struct ShadowShare
		{
			std::uint32_t produced = 0;  // 0: none (a free slot)
			bool caster = false;         // not kObjectNoShadow
			std::uint32_t technique = 0;
			std::array<std::uint32_t, kOcclusionViews> occlusion{};
			std::uint32_t rasterFlags = 0;
			std::uint64_t vertexLayout = 0;
			bool operator==(const ShadowShare&) const = default;
			/** @brief Its key under a_mode. False when it does not draw in that mode. */
			bool KeyOf(std::uint32_t a_mode, ShadowPipelineKey& a_key) const;
		};
		// Draws per mode and caster key for every object that can cast. Shadow view capacities are sized from this,
		// so they change with the scene, not the frame's casters.
		std::vector<ShadowShare> shadow;  // by object slot
		std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::uint64_t, ShadowPipelineKeyHash>, kShadowModeCount> modeKeyDraws;
		// Caster rows over all modes. Each view family takes one draw, not each mode.
		std::uint64_t casterRows = 0;
		// Counters since the last report.
		std::uint64_t updates = 0, changes = 0, resyncs = 0;
		// Bumped whenever modeKeyDraws changes. The revisions' shadow shapes are sized from it.
		std::uint64_t shadowVersion = 0;

		std::uint32_t Draws() const { return static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX)); }
		std::uint32_t PipelineDraws(std::size_t a_pipeline) const
		{
			return a_pipeline < perPipeline.size() ? static_cast<std::uint32_t>(std::min<std::uint64_t>(perPipeline[a_pipeline], UINT32_MAX)) : 0u;
		}
	};

	using ModeKeyDraws = std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::uint64_t, ShadowPipelineKeyHash>, kShadowModeCount>;
	/** @brief The draw bound from a full scan of a_tables. Reference for checking DrawBoundStore. */
	struct ScannedDrawBound
	{
		std::uint32_t draws = 0;
		std::vector<std::uint32_t> perPipeline;  // by pipeline slot
		ModeKeyDraws modeKeys;
	};
	ScannedDrawBound ScanDrawBound(const SceneTables& a_tables);

	/** @brief The store's object records, or a full set of its own without a store. */
	using ObjectRecordsOut = KeptView<BindlessObject>;

	/** @brief The extras rows, read straight from the tables and journalled so only changed rows are sent. */
	struct ExtrasStore
	{
		LogCursor cursor;
		ChangeJournal rows;
		std::uint64_t updates = 0, rowsSent = 0, resyncs = 0;
	};

	/** @brief A build's view of the extras rows. Content epoch 0 means sent whole. */
	struct ExtrasOut
	{
		const float* extras = nullptr;
		std::uint32_t extraRows = 0;
		ChangeJournal::Snapshot changes;
		std::uint64_t ContentEpoch() const { return changes.contentEpoch; }
		std::uint32_t Rows() const { return extraRows; }
		const float* Row(std::uint64_t a_row) const { return extras + std::size_t(a_row) * 4; }
		void Reset() { *this = {}; }
	};

	void UpdateExtras(ExtrasStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation, ExtrasOut& a_out);

	/**
	 * @brief Emits the rows a buffer at a_contentEpoch lacks, clipped to a_bufferRows. Rows past it wait for growth.
	 * a_mirror, when set, receives a copy of what was uploaded.
	 */
	template <class Emit>
	std::size_t EmitExtras(const ExtrasOut& a_out, std::uint64_t a_contentEpoch, std::vector<float>* a_mirror, Emit&& a_emit, std::uint32_t a_bufferRows = ~0u)
	{
		const std::uint32_t rows = std::min(a_out.Rows(), a_bufferRows);
		if (!rows || !a_out.extras)
			return 0;
		std::size_t sent = 0;
		a_out.changes.ForEachRun(a_contentEpoch, rows, [&](std::uint64_t a_first, std::uint64_t a_count) {
			a_emit(a_out.Row(a_first), std::size_t(a_count) * 16, std::size_t(a_first) * 16);
			if (a_mirror) {
				if (a_mirror->size() < std::size_t(rows) * 4)
					a_mirror->resize(std::size_t(rows) * 4, 0.0f);
				std::memcpy(&(*a_mirror)[std::size_t(a_first) * 4], a_out.Row(a_first), std::size_t(a_count) * 16);
			}
			sent += static_cast<std::size_t>(a_count);
		});
		return sent;
	}

	/** @brief A build's geometry table. The vertex stream rows follow the slots and are sent whole every build. */
	struct GeometryDrawsOut
	{
		KeptView<GeometryDraw> slots;
		std::vector<GeometryDraw> streamRows;
		std::size_t SlotCount() const { return slots.Count(); }
		std::size_t Count() const { return SlotCount() + streamRows.size(); }
		std::uint64_t ContentEpoch() const { return slots.ContentEpoch(); }
		std::vector<GeometryDraw> Flat() const
		{
			std::vector<GeometryDraw> flat;
			if (slots.elements)
				flat = *slots.elements;
			flat.insert(flat.end(), streamRows.begin(), streamRows.end());
			return flat;
		}
		void Reset() { *this = {}; }
	};

	/** @brief The scene streams of one set of tables, updated once and read by every build and commit as immutable views. */
	struct StreamViews
	{
		std::shared_ptr<const SceneTables> tables;
		std::uint32_t generation = 0;
		ObjectRecordsOut objects;
		ExtrasOut extras;
		KeptView<GeometryDraw> geometries;
	};
	/** @brief A build's geometry slots, from the stream views or packed from the tables when there are none. */
	void TakeGeometryDraws(const StreamViews* a_streams, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame,
		GeometryDrawsOut& a_out);

	/** @brief The geometry slots' draws kept across builds, repacked from the tables' geometry log. */
	struct GeometryStore
	{
		LogCursor cursor;
		KeptArray<GeometryDraw> packed;
		// Counters since the last report.
		std::uint64_t updates = 0, rewritten = 0, resyncs = 0;
	};

	/** @brief Brings the stores up to a_tables and returns them as views. Only the snapshot builder calls this. */
	std::shared_ptr<const StreamViews> MakeStreamViews(ObjectRecordStore& a_objects, ExtrasStore& a_extras, GeometryStore& a_geometries, const TablesContentEpochs& a_oldest,
		std::shared_ptr<const SceneTables> a_hold, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame);

	// The change causes a record is built from.
	constexpr std::uint32_t kObjectRecordCauses = kChangeBindings | kChangeAncestorValue | kChangeSkin | kChangeExtras;

	/** @brief Updates the object records from the store's change log, or builds them all without a store. */
	void UpdateObjectRecords(ObjectRecordStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, ObjectRecordsOut& a_out);
}
