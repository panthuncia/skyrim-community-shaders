// Builds both main segments' payloads from a publication's tables and lookups. Pure.
// The snapshot builder runs it on DCLF's pool, one build at a time.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "RenderGraph/Fatal.h"
#include "Utils/Hash.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Core/Gpu/TextureFeedback.h"

namespace DCLF::Draws
{
	namespace
	{
		using Skip = IndirectDraws::Skip;

		struct PipelineBlocks
		{
			std::uint32_t setIndex = Lookups::kNone;
			std::span<const std::uint8_t> vsTable, psTable;
			std::array<const Lookups::RegisterUsageBits*, 2> usage{};  // per segment (kAsyncColour, kAsyncZPrepass): its variant's
			std::uint32_t shadowMaskIndex = Lookups::kNone;
			std::uint64_t tables = 0;  // TablesHash of its constant tables
			bool rowOk = false;        // its pipeline row's blocks fit (MainRows)
		};

		/** @brief FNV-1a hash of a pipeline's constant tables, to tell which tables a material row was packed with. */
		std::uint64_t TablesHash(std::span<const std::uint8_t> a_vs, std::span<const std::uint8_t> a_ps)
		{
			Util::Fnv1a hash;
			hash.Mix(a_vs.size()).Bytes(a_vs.data(), a_vs.size());
			hash.Mix(a_ps.size()).Bytes(a_ps.data(), a_ps.size());
			return hash.Value();
		}

		std::int32_t ProjectedSlot(bool a_projectedPipeline, std::uint32_t a_slot)
		{
			if (!a_projectedPipeline)
				return -1;
			for (std::size_t i = 0; i < Scene::FrameView::ProjectedTextures::kSlots.size(); ++i) {
				if (Scene::FrameView::ProjectedTextures::kSlots[i] == a_slot)
					return static_cast<std::int32_t>(i);
			}
			return -1;
		}

		/** @brief Colour when built, since the report counts its skips. */
		std::size_t PrimarySegment(const std::array<const MainInputs*, 2>& a_in) { return a_in[kAsyncColour] ? kAsyncColour : kAsyncZPrepass; }

		// Pair resolution goes to the pool past this many pairs. Chunks end on a material boundary.
		constexpr std::size_t kPairFanoutMin = 512;
		constexpr std::size_t kPairGrain = 128;
		constexpr std::size_t kResyncFanoutMin = 4096;
		constexpr std::size_t kResyncGrain = 512;

		std::uint64_t Mix(std::uint64_t a_value, std::uint64_t a_word) { return (a_value ^ a_word) * Util::Fnv1a::kPrime; }

		// Prepares a kept array for writes from pool chunks.
		// The marks are applied afterwards on the owner thread, in chunk order.
		template <class Rows>
		void PrepareMainKeptWrites(Rows& a_rows, std::size_t a_first, std::size_t a_count)
		{
			if constexpr (requires { a_rows.PrepareWrites(a_first, a_count); })
				a_rows.PrepareWrites(a_first, a_count);
			else
				(void)a_rows.Mutable();
		}
		template <class Rows, class Row>
		std::uint64_t WriteKeptParts(Rows& a_rows, std::size_t a_index, const Row& a_row, std::size_t a_parts)
		{
			if constexpr (requires { a_rows.WriteParts(a_index, a_row, a_parts); }) {
				return a_rows.WriteParts(a_index, a_row, a_parts);
			} else {
				const std::size_t partBytes = sizeof(Row) / a_parts;
				const auto* held = reinterpret_cast<const std::byte*>(&a_rows.Get()[a_index]);
				const auto* value = reinterpret_cast<const std::byte*>(&a_row);
				std::uint64_t mask = 0;
				for (std::size_t p = 0; p < a_parts; ++p)
					if (std::memcmp(held + p * partBytes, value + p * partBytes, partBytes) != 0)
						mask |= 1ull << p;
				if (mask)
					a_rows.Mutable()[a_index] = a_row;
				return mask;
			}
		}
		template <class Rows>
		void MarkKeptParts(Rows& a_rows, std::size_t a_index, std::uint64_t a_mask, std::size_t a_parts)
		{
			if constexpr (requires { a_rows.MarkParts(a_index, a_mask, a_parts); }) {
				a_rows.MarkParts(a_index, a_mask, a_parts);
			} else {
				for (std::size_t p = 0; p < a_parts; ++p)
					if ((a_mask >> p) & 1)
						a_rows.Mark(a_index * a_parts + p);
			}
		}

		/** @brief Which source gives each texture register: the pipeline row for t14, the material row, the payload, or the frame record. */
		struct TextureClasses
		{
			std::array<std::uint64_t, 2> shadowMask{}, material{}, buffers{}, frame{}, characterLight{};
			std::array<std::uint32_t, Extensions::kFeatureTextureSlots> featureRegister{};
		};
		static_assert(kTextureRegisters == 128 && kPixelTextureSlots <= 32 && kSamplerRegisters <= 32 && kConstantBufferRegisters <= 32);
		const TextureClasses& TextureClassesOf()
		{
			static const TextureClasses classes = [] {
				TextureClasses c;
				for (std::uint32_t t = 0; t < kTextureRegisters; ++t) {
					const std::uint64_t bit = 1ull << (t % 64);
					const std::size_t w = t / 64;
					if (t == kShadowMaskSlot)
						c.shadowMask[w] |= bit;
					else if (t < kPixelTextureSlots)
						c.material[w] |= bit;
					else if (const int f = Extensions::FeatureTextureSlotOf(t); f >= 0)
						c.featureRegister[f] = t;
					else if (t == kObjectBufferRegister || t == kExtrasBufferRegister || Extensions::FrameBindingRegister(t) || t == kPlacementBufferRegister ||
							 t == kPaletteBufferRegister || t == kShadingBufferRegister)
						c.buffers[w] |= bit;
					else {
						c.frame[w] |= bit;
						if (t == kCharacterLightRegister)
							c.characterLight[w] |= bit;
					}
				}
				return c;
			}();
			return classes;
		}

		/** @brief A hash of the material row state a pair's verdict reads. */
		std::uint64_t MaterialStateWitness(const MainRows::MaterialState& a_state)
		{
			std::uint64_t value = Util::Fnv1a::kOffset;
			value = Mix(value, (a_state.written ? 1u : 0u) | (a_state.texturesOk ? 2u : 0u) | (a_state.deferred ? 4u : 0u) | (a_state.blocksOk ? 8u : 0u) |
								   (std::uint64_t(a_state.missingTexture) << 32));
			value = Mix(value, (std::uint64_t(a_state.textures) << 32) | a_state.samplers);
			value = Mix(value, a_state.features);
			return Mix(value, a_state.tables);
		}

		/**
		 * @brief What resolving pairs writes besides the pairs, merged in chunk order because the rows' journal is single-threaded.
		 * A chunk writes only its own materials' rows, since chunks split on material boundaries.
		 */
		struct PairWork
		{
			bool deferred = false;  // a pool chunk's: rows written unmarked into prepared storage, marked by MergeWork
			std::vector<std::pair<std::uint32_t, std::uint64_t>> rowMarks;  // (material, its parts written): every row it wrote, its bytes moved or not
			std::array<std::uint64_t, 8> keyMoved{};
			std::uint64_t materialsWritten = 0, materialRowsWritten = 0;
			std::uint32_t missingVertexConstants = 0, missingPixelConstants = 0;
			std::uint32_t rowTableConflicts = 0, conflictMaterial = 0, conflictPipeline = 0;
			// Region pairs that cannot draw, counted per resolution the way AssembleRecord counts a draw.
			std::array<std::uint32_t, static_cast<std::size_t>(Skip::Count)> skipped{};
			std::vector<std::uint32_t> missingTextures;
			std::uint32_t deferredTextures = 0;
			// Each re-resolved pair's verdict, applied in check order.
			struct Verdict
			{
				ResidentRegion::Pair* pair = nullptr;
				std::uint64_t key = 0, witness = 0;
				std::uint32_t slot = 0, skip = kNoSkip;
				bool ok = false;
				std::array<std::uint64_t, 2> registers{};
				const void* binding = nullptr;
				const void* mask = nullptr;
			};
			std::vector<Verdict> verdicts;

			void Clear()
			{
				const bool keep = deferred;
				*this = PairWork{};
				deferred = keep;
			}
		};

		/** @brief A body no payload holds, reset, or a new one. */
		std::shared_ptr<MainBody> AcquireBody(BuildCache* a_cache)
		{
			if (!a_cache)
				return std::make_shared<MainBody>();
			// Reset every idle body, not only the one taken. An idle body would otherwise hold its stream views and owners
			// and keep that publication from retiring.
			std::shared_ptr<MainBody> free;
			for (auto& held : a_cache->bodies)
				if (held.use_count() == 1) {
					held->Reset();
					if (!free)
						free = held;
				}
			return free ? free : a_cache->bodies.emplace_back(std::make_shared<MainBody>());
		}

		/**
		 * @brief One build of the main segments' payloads. Both segments read one list, and an object draws only when every built segment
		 * can draw its pair, so a member is drawn by both or neither.
		 */
		class MainBuild final
		{
		public:
			MainBuild(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
				const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache) :
				segmentIn(a_in), segmentOut(a_out), primary(PrimarySegment(a_in)), in(*a_in[primary]), tables(a_tables), frame(a_frame), lookups(a_lookups),
				out(*a_out[primary]), rows(a_rows), cache(a_cache), heldBody(AcquireBody(a_cache)), body(*heldBody)
			{}

			/** @brief Everything except the part that reads the stream views. */
			void Run();
			/** @brief The stream views' part, then the other segment's payload. */
			void Finish(std::shared_ptr<const StreamViews> a_streams);

		private:
			const std::array<const MainInputs*, 2> segmentIn;  // per segment; none when not built
			const std::array<MainPayload*, 2> segmentOut;
			const std::size_t primary;
			const MainInputs& in;  // the primary segment's
			const SceneTables& tables;
			const std::span<const std::uint32_t> objectRoots = ObjectRootsOf(tables);
			const FrameTables& frame;  // the frame's PerGeometry blocks
			const Lookups& lookups;
			MainPayload& out;  // the primary segment's
			MainRows& rows;
			BuildCache* cache;
			// Shared by both segments' payloads.
			const std::shared_ptr<MainBody> heldBody;
			MainBody& body;
			std::shared_ptr<const StreamViews> streams;  // the frame's (Finish), or none: the build packs its own geometry slots
			// Only read the clock when the report wants part timings.
			const bool timeParts = Diagnostics::Observe().stats || Diagnostics::Observe().profile;
			const std::uint8_t rootPhases = Extensions::Views().rootPhases;
			PairWork loopWork;  // the object loop's resolutions (merged after each)

			decltype(MainBody::sequences)& sequences = body.sequences;
			decltype(MainBody::inputList)& drawInputs = body.inputList;
			decltype(MainBody::decalCount)& decalCount = body.decalCount;
			decltype(MainBody::decalTemplates)& decalTemplates = body.decalTemplates;
			const bool colourBuilt = segmentIn[kAsyncColour] != nullptr;
			const std::uint32_t frameNumber = in.frameNumber;
			static constexpr auto renderFlags = GeometryPort::kMainPassRenderFlags;

			std::array<std::array<std::uint64_t, kConstantBufferRegisters>, 2> frameVS{}, framePS{};
			std::array<std::uint32_t, 2> frameVSBits{}, framePSBits{};  // the registers above that are bound (bit per register)
			std::vector<PipelineBlocks> pipelineBlocks;
			ankerl::unordered_dense::map<std::uint64_t, ResolvedBindings> resolvedBindings;
			std::uint32_t currentObject = ~0u;
			std::chrono::steady_clock::time_point partStart;
			ResidentRegion* region = nullptr;
			bool wholeScene = false;
			std::size_t regionInputs = 0, regionDraws = 0;

			void Reset();
			void BeginRows();
			void FrameRegisters();
			void PackPipelines();
			void BeginStates();
			void PrepareObjects();
			void Skipped(Skip a_reason);
			void Mark(std::size_t a_part);
			std::uint8_t Segments() const
			{
				return static_cast<std::uint8_t>((segmentIn[kAsyncColour] ? 1u << kAsyncColour : 0u) | (segmentIn[kAsyncZPrepass] ? 1u << kAsyncZPrepass : 0u));
			}
			// Only set members are drawn. A CategoryRoot view's members are drawable through the view's phase alone.
			bool InSet(std::uint32_t o) const
			{
				return o < tables.objects.size() && ((tables.objects[o].flags & kObjectMember) || (o < tables.setPhases.size() && (tables.setPhases[o] & rootPhases)));
			}

			// Checked once per build. kNoRecord after Skipped() recorded why.
			std::uint32_t AssembleRecord(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks);
			/** @brief Checks what a pair's rows give against what its pipeline reads. Pure except for a_work, so any thread. */
			void ResolvePair(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const MainRows::MaterialState& a_material, ResolvedBindings& resolved,
				std::array<std::uint64_t, 2>& a_registers, PairWork& a_work) const;
			void WritePipelineRow(std::uint32_t p, const PipelineBlocks& blocks);
			std::array<std::uint32_t, 8> MaterialRowKey(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks) const;
			/** @brief Packs the material's row for pipeline p from scratch. Pure. */
			void PackMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const std::array<std::uint32_t, 8>& a_key, MainRows::MaterialState& a_state,
				MaterialRow& a_row) const;
			void PackMaterialBlocks(std::uint32_t p, const MaterialRecord& a_material, const PipelineBlocks& blocks, MainRows::MaterialState& a_state, MaterialRow& a_row) const;
			void PackMaterialTextures(std::uint32_t m, bool a_projected, const Lookups::Material* a_lookup, const MaterialRecord& a_material, MainRows::MaterialState& a_state,
				MaterialRowHeader& a_header) const;
			void PackMaterialSamplers(std::uint32_t p, bool a_projected, const MaterialRecord& a_material, MainRows::MaterialState& a_state, MaterialRowHeader& a_header) const;
			/** @brief Rewrites the material's row when its sources changed. A pool chunk's write is marked later by MergeWork. */
			void WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, PairWork& a_work);
			/** @brief Merges a_work into the build and rows on the owner thread, in order, then clears it. */
			void MergeWork(PairWork& a_work);
			/** @brief Takes or releases animated rows for the written rows. Runs in Finish before the rows' views. */
			void AssignAnimatedRows();

			void BuildResidentRegion();
			static std::uint64_t PairKeyOf(const ObjectRecord& a_object) { return (std::uint64_t(a_object.materialIndex) << 32) | a_object.pipelineIndex; }
			bool ResidentAt(std::uint32_t o) const { return o < tables.residentSlot.size() && tables.residentSlot[o] != 0; }
			// With the whole scene, any single-input record and the depth segment's cull-only candidates can join the region.
			bool RegionEligible(std::uint32_t o) const;
			std::uint8_t RegionEntry(std::uint32_t o, DrawInput& a_input) const;
			void RegionAcquire(std::uint64_t a_key, std::uint32_t o);
			void RegionRelease(std::uint64_t a_key, std::uint32_t o);
			// A pair's share of the bundle, kept by count.
			void SetPairRegisters(ResidentRegion::Pair& a_pair, const std::array<std::uint64_t, 2>& a_registers);
			void SetPairOwners(ResidentRegion::Pair& a_pair, std::uint32_t m, std::uint32_t p, const void* a_binding, const void* a_mask);
			void AcquireOwner(const void* a_owner, const std::shared_ptr<const void>& a_held);
			void ReleaseOwner(const void* a_owner);
			/** @brief Rebuilds every entry after a Reset on the pool, with one journal resync. */
			void ResyncRegionParallel();
			/** @brief Resolves whole materials of the check list into a_work. */
			void ResolvePairsChunk(const std::vector<std::uint64_t>& a_check, std::size_t a_begin, std::size_t a_end, bool a_everyPair, PairWork& a_work);
			void RegionSetDraws(std::uint32_t i, std::uint8_t a_draws);
			void RegionRemove(std::uint32_t o);
			/** @brief Writes entry i whole. The caller already merged its main part. */
			void RegionWrite(std::uint32_t i, const DrawInput& a_entry);
			/** @brief A decal's blank input. BuildDrawsCS writes a zero-count draw, and it names no row past the scene's buffers. */
			DrawInput DecalBlank(std::uint32_t o) const;
			void RegionUpsert(std::uint32_t o);
			void RegionTake(std::uint32_t o);
			/** @brief Updates the entries the change log named, or every slot on a resync. */
			void UpdateRegionEntries();
			/** @brief Updates entries whose pipeline set index changed or whose pair became buildable or unbuildable. */
			void UpdateRegionPairs();
			void ChangedPipelineSets(std::vector<std::uint32_t>& a_changed);
			bool EveryPairMoved();
			void MovedPipelines(std::vector<std::uint32_t>& a_moved);
			void PairsToCheck(bool a_everyPair, const std::vector<std::uint32_t>& a_moved, std::vector<std::uint64_t>& a_check);
			std::vector<PairWork> ResolveCheckedPairs(const std::vector<std::uint64_t>& a_check, bool a_everyPair);
			void ApplyVerdicts(std::vector<PairWork>& a_works, std::vector<std::uint64_t>& a_changedPairs);
			void BundleRegionPairs();
			void RewriteChangedEntries(const std::vector<std::uint32_t>& a_changedPipelines, const std::vector<std::uint64_t>& a_changedPairs);
			std::uint64_t PipelineWitness(std::uint32_t p) const;
			/** @brief Routes every touched slot to the region, the loop or nobody. */
			void RouteTouched();
			void PublishRegion();

			void RunObjectLoop();
			void LoopObject(std::uint32_t o);
		};
	}

	void MainBuild::Run()
	{
		DCLF_ZONE("CS.DCLF.BuildMainPayload");
		Reset();
		FrameRegisters();
		PackPipelines();
		BeginStates();
		if (timeParts)
			partStart = std::chrono::steady_clock::now();
		PrepareObjects();
		BuildResidentRegion();
		RunObjectLoop();
	}

	void MainBuild::Reset()
	{
		TracyCZoneN(resetMainZone, "CS.DCLF.BuildMain.Reset", true);
		out.Reset();
		out.inputs = in;
		out.body = heldBody;
		if (lookups.sharedBindingBlock)
			body.bindingOwners.push_back(lookups.sharedBindingBlock);
		BeginRows();
		TracyCZoneEnd(resetMainZone);
	}

	void MainBuild::BeginRows()
	{
		// Only what is written from here on is new. The journals keep what the oldest backing lacks.
		rows.material.BeginBuild(in.materialRowsOldestContentEpoch);
		rows.pipeline.BeginBuild(in.pipelineRowsOldestContentEpoch);
		if (!rows.active || rows.identity != in.addresses.identity || rows.generation != in.tablesGeneration) {
			// New resources or tables mean the slots mean other things, so redo every row.
			rows.material.Clear();
			rows.pipeline.Clear();
			rows.materials.clear();
			rows.pipelines.clear();
			rows.animated.Clear();
			rows.active = true;
			rows.identity = in.addresses.identity;
			rows.generation = in.tablesGeneration;
			++rows.resyncs;
			rows.productDirty.MarkEvery();
		}
		++rows.builds;
		if (rows.material.Size() < tables.materials.size())
			rows.material.Mutable().resize(tables.materials.size());
		// Pipeline rows shrink with the pipeline table, so a slot reused past the cut starts unwritten
		// and the part's next version can shrink too.
		if (const auto slots = tables.pipelines.size(); rows.pipeline.Size() > slots) {
			rows.pipeline.Mutable().resize(slots);
			rows.pipelines.resize(slots);
			rows.tablesHashes.resize(std::min(rows.tablesHashes.size(), slots));
		}
		if (rows.pipeline.Size() < tables.pipelines.size())
			rows.pipeline.Mutable().resize(tables.pipelines.size());
		rows.materials.resize(std::max(rows.materials.size(), tables.materials.size()));
		rows.pipelines.resize(std::max(rows.pipelines.size(), tables.pipelines.size()));
		if (rows.tablesHashes.size() < tables.pipelines.size())
			rows.tablesHashes.resize(tables.pipelines.size());
	}

	void MainBuild::FrameRegisters()
	{
		// An unsupplied frame slot stays zero, which the constants check rejects for any pipeline reading it.
		frameVS = {};
		framePS = {};
		frameVSBits = framePSBits = {};
		for (std::size_t j = 0; j < segmentIn.size(); ++j) {
			if (!segmentIn[j])
				continue;
			const auto& segment = *segmentIn[j];
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((segment.vsFrameMask >> slot) & 1)
					frameVS[j][slot] = segment.addresses.frameConstants + FrameSlotOffset(false, slot);
				if ((segment.psFrameMask >> slot) & 1)
					framePS[j][slot] = segment.addresses.frameConstants + FrameSlotOffset(true, slot);
				frameVSBits[j] |= frameVS[j][slot] ? 1u << slot : 0u;
				framePSBits[j] |= framePS[j][slot] ? 1u << slot : 0u;
			}
		}
	}

	void MainBuild::PackPipelines()
	{
		pipelineBlocks.assign(tables.pipelines.size(), {});
		TracyCZoneN(pipelinesZone, "CS.DCLF.BuildMain.Pipelines", true);
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			auto& blocks = pipelineBlocks[p];
			if (!tables.PipelineUsed(p) || p >= lookups.pipelines.size())
				continue;
			const auto& entry = lookups.pipelines[p];
			if (entry.setIndex == Lookups::kNone || !(entry.key == tables.pipelines[p]))
				continue;
			// No published constants yet, so no row. Its members wait.
			if (!tables.PipelineConstantsCurrent(p) || !tables.TechniqueConstantsValid(p))
				continue;
			blocks.setIndex = entry.setIndex;
			blocks.vsTable = entry.vsTable;
			blocks.psTable = entry.psTable;
			blocks.usage[kAsyncColour] = &entry.usage[kColorVariant];
			blocks.usage[kAsyncZPrepass] = &entry.usage[kDepthVariant];
			blocks.shadowMaskIndex = entry.shadowMaskIndex;
			// Rehash only when the entry's version moved.
			auto& hashed = rows.tablesHashes[p];
			if (hashed.instance != lookups.instance || hashed.version != entry.version)
				hashed = { lookups.instance, entry.version, TablesHash(blocks.vsTable, blocks.psTable) };
			blocks.tables = hashed.hash;
			WritePipelineRow(static_cast<std::uint32_t>(p), blocks);
			blocks.rowOk = rows.pipelines[p].blocksOk;
		}
		TracyCZoneEnd(pipelinesZone);
	}

	void MainBuild::WritePipelineRow(std::uint32_t p, const PipelineBlocks& blocks)
	{
		auto& state = rows.pipelines[p];
		const auto& techniqueRow = tables.TechniqueOf(p);
		const std::uint64_t vertexLayout = tables.pipelines[p].vertexLayout;
		const std::array<std::uint32_t, 8> key{ tables.pipelineConstants[p].version, techniqueRow.constantsVersion,
			techniqueRow.bindingVersion, lookups.pipelines[p].version, lookups.sharedVersion, 1u, static_cast<std::uint32_t>(vertexLayout),
			static_cast<std::uint32_t>(vertexLayout >> 32) };
		if (state.written && state.key == key)
			return;
		const auto& technique = techniqueRow.value;
		PipelineRow row;
		bool ok = true;
		auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
						std::uint32_t a_offset, std::uint32_t a_room) {
			if (ConstantGroupSize(a_layout, a_table, a_variables, a_first) > a_room) {
				ok = false;
				return;
			}
			PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, std::span(row.bytes.data() + a_offset, a_room));
		};
		// A family without row blocks keeps its technique and PerGeometry blocks zero. The Effect port reads frame values there instead.
		if (Extensions::FamilyRules(tables.pipelines[p].rasterFlags).rowBlocks) {
			pack(technique.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique], kPipelineRowTechniqueVS, kPipelineRowTechniqueBytes);
			pack(technique.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique], kPipelineRowTechniquePS, kPipelineRowTechniqueBytes);
			GeometryTemplate geometry;
			PackGeometryTemplate(tables.pipelineConstants[p].constants, blocks.vsTable, blocks.psTable, true, geometry);
			if (geometry.vs.size() > kPipelineRowGeometryVSBytes || geometry.ps.size() > kPipelineRowGeometryPSBytes)
				ok = false;
			else {
				std::memcpy(row.bytes.data() + kPipelineRowGeometryVS, geometry.vs.data(), geometry.vs.size());
				std::memcpy(row.bytes.data() + kPipelineRowGeometryPS, geometry.ps.data(), geometry.ps.size());
			}
		}
		// The permutation is per pipeline. Its one per-object bit is read only by Effect.hlsl, never by Lighting.hlsl.
		const auto& permutation = tables.permutations[p];
		const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, permutation.extraShaderDescriptor,
			permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
		std::memcpy(row.bytes.data() + kPipelineRowPermutation, data, sizeof(data));
		// The header holds no addresses. The draws' sequences carry them.
		auto& header = HeaderOf(row);
		const std::uint32_t shadowMask = technique.shadowMask ? blocks.shadowMaskIndex : lookups.nullTexture;
		header.shadowMask = shadowMask == Lookups::kNone ? 0u : shadowMask;
		const std::uint32_t filter = technique.shadowMask && technique.filterModes[kShadowMaskSlot] != kUnwrittenFilterMode ? technique.filterModes[kShadowMaskSlot] : 0u;
		const std::uint32_t sampler = lookups.Sampler(0, filter);
		header.shadowMaskSampler = sampler == Lookups::kNone ? 0u : sampler;
		header.vertexLayout = vertexLayout;
		state.shadowMask = shadowMask != Lookups::kNone;
		state.shadowMaskSampler = sampler != Lookups::kNone;
		state.blocksOk = ok;
		state.key = key;
		state.written = true;
		if (rows.pipeline.Set(p, row))
			++out.pipelineRowsWritten;
		++rows.pipelinesWritten;
	}

	void MainBuild::BeginStates()
	{
		// Per-object states are only for diagnostics.
		if (Diagnostics::Observe().parity)
			body.objectState.assign(tables.objects.size(), kObjectStateAbsent);
	}

	void MainBuild::PrepareObjects()
	{
		// Object records come from the stream views and palettes from the frame values, not from this build.
		Mark(6);
		if (colourBuilt) {
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				// Decals past the range wait while its growth is outstanding.
				decalCount[group] = std::min(tables.rangeCount[group], in.addresses.sequenceDecals);
			}
			// A decal past the scene's buffers keeps its slot with a blank input.
			// Cutting the range would drop every later decal while the set claims them.
			for (std::uint32_t group = 0; group < kDecalGroups; ++group)
				decalTemplates[group].resize(decalCount[group]);
		}
	}

	void MainBuild::Skipped(Skip a_reason)
	{
		++body.skipped[static_cast<std::size_t>(a_reason)];
		if (currentObject < body.objectState.size())
			body.objectState[currentObject] = static_cast<std::uint8_t>(a_reason);
	}

	void MainBuild::Mark(std::size_t a_part)
	{
		if (!timeParts)
			return;
		const auto now = std::chrono::steady_clock::now();
		out.partMs[a_part] += std::chrono::duration<double, std::milli>(now - partStart).count();
		partStart = now;
	}

	std::uint32_t MainBuild::AssembleRecord(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks)
	{
		(void)o;
		auto [resolvedIt, newResolved] = resolvedBindings.try_emplace(PairKeyOf(object));
		auto& resolved = resolvedIt->second;
		if (newResolved) {
			const std::uint32_t m = object.materialIndex, p = object.pipelineIndex;
			if (m < lookups.materials.size() && lookups.materials[m].bindingBlock)
				body.bindingOwners.push_back(lookups.materials[m].bindingBlock);
			if (p < lookups.pipelines.size() && lookups.pipelines[p].shadowMaskOwner)
				body.bindingOwners.push_back(lookups.pipelines[p].shadowMaskOwner);
			// No row waits for a table, because the payload's row tables hold every row.
			Mark(0);
			WriteMaterialRow(m, p, blocks, loopWork);
			Mark(1);
			std::array<std::uint64_t, 2> registers{};
			ResolvePair(m, p, blocks, rows.materials[m], resolved, registers, loopWork);
			out.frameRegisters[0] |= registers[0];
			out.frameRegisters[1] |= registers[1];
			MergeWork(loopWork);
		}
		Mark(4);
		if (resolved.skipReason != kNoSkip) {
			if (resolved.skipReason == static_cast<std::uint32_t>(Skip::Texture) && body.missingNext < body.missingTextures.size())
				body.missingTextures[body.missingNext++] = resolved.missingTexture;
			if (resolved.deferred)
				++body.deferredTextures;
			Skipped(static_cast<Skip>(resolved.skipReason));  // per draw, not once per pair
			return kNoRecord;
		}
		return resolved.recordIndex;
	}

	void MainBuild::MergeWork(PairWork& a_work)
	{
		for (const auto& [m, parts] : a_work.rowMarks) {
			MarkKeptParts(rows.material, m, parts, kMaterialRowParts);
			rows.productDirty.Mark(m);
		}
		for (std::size_t k = 0; k < rows.keyMoved.size(); ++k)
			rows.keyMoved[k] += a_work.keyMoved[k];
		rows.materialsWritten += a_work.materialsWritten;
		out.materialRowsWritten += a_work.materialRowsWritten;
		body.missingVertexConstants |= a_work.missingVertexConstants;
		body.missingPixelConstants |= a_work.missingPixelConstants;
		if (a_work.rowTableConflicts) {
			if (out.rowTableConflicts == 0)
				logger::warn("[DCLF] material slot {} was packed with other constant tables than pipeline slot {} has; its draws stay native", a_work.conflictMaterial,
					a_work.conflictPipeline);
			out.rowTableConflicts += a_work.rowTableConflicts;
		}
		for (std::size_t reason = 0; reason < body.skipped.size(); ++reason)
			body.skipped[reason] += a_work.skipped[reason];
		for (const std::uint32_t texture : a_work.missingTextures)
			if (body.missingNext < body.missingTextures.size())
				body.missingTextures[body.missingNext++] = texture;
		body.deferredTextures += a_work.deferredTextures;
		a_work.Clear();
	}

	void MainBuild::ResolvePair(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const MainRows::MaterialState& material, ResolvedBindings& resolved,
		std::array<std::uint64_t, 2>& a_registers, PairWork& a_work) const
	{
		// An input packs its rows in one word, so overflowing its fields is a hard failure.
		if (m > kRowMaterialMask || p >= (1u << (32 - kRowPipelineShift)))
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("material slot {} or pipeline slot {} past an input's row fields", m, p));
		auto fail = [&](Skip a_reason) { resolved.skipReason = static_cast<std::uint32_t>(a_reason); };
		const auto& pipeline = rows.pipelines[p];
		// The row was packed with its technique's tables. A pipeline in the same slot with other tables would read it wrong.
		if (material.tables != blocks.tables) {
			if (a_work.rowTableConflicts++ == 0) {
				a_work.conflictMaterial = m;
				a_work.conflictPipeline = p;
			}
			return fail(Skip::Constants);
		}
		if (!material.texturesOk) {
			resolved.deferred = material.deferred;
			resolved.missingTexture = material.missingTexture;
			return fail(Skip::Texture);
		}
		// Whether segment j's variant gets everything it reads. The first register not given is the skip's sample,
		// and the frame textures asked of the commit are those below it.
		const auto& classes = TextureClassesOf();
		auto reads = [&](std::size_t j) {
			const auto& usage = *blocks.usage[j];
			const bool depthOnly = j == kAsyncZPrepass;
			std::array<std::uint64_t, 2> given{};
			for (std::size_t w = 0; w < 2; ++w)
				given[w] = classes.buffers[w] | (pipeline.shadowMask ? classes.shadowMask[w] : 0) | (depthOnly ? classes.characterLight[w] : classes.frame[w]);
			given[0] |= std::uint64_t(material.textures) & classes.material[0];
			for (std::uint32_t features = material.features; features;) {
				const auto f = static_cast<std::uint32_t>(std::countr_zero(features));
				features &= features - 1;
				if (f < Extensions::kFeatureTextureSlots) {
					const std::uint32_t t = classes.featureRegister[f];
					given[t / 64] |= 1ull << (t % 64);
				}
			}
			const std::array<std::uint64_t, 2> missing{ usage.textures[0] & ~given[0], usage.textures[1] & ~given[1] };
			std::array<std::uint64_t, 2> below{ ~0ull, ~0ull };
			if (missing[0] || missing[1]) {
				const std::uint32_t t = missing[0] ? static_cast<std::uint32_t>(std::countr_zero(missing[0])) : 64 + static_cast<std::uint32_t>(std::countr_zero(missing[1]));
				below = t < 64 ? std::array<std::uint64_t, 2>{ (1ull << t) - 1, 0 } : std::array<std::uint64_t, 2>{ ~0ull, t == 64 ? 0 : (1ull << (t - 64)) - 1 };
				resolved.missingTexture = t;
			}
			if (!depthOnly)
				for (std::size_t w = 0; w < 2; ++w)
					a_registers[w] |= usage.textures[w] & classes.frame[w] & below[w];  // the commit resolves them into the frame record
			if (missing[0] || missing[1]) {
				fail(Skip::Texture);
				return false;
			}
			constexpr std::uint32_t samplerMask = kSamplerRegisters >= 32 ? ~0u : (1u << kSamplerRegisters) - 1;
			const std::uint32_t samplersGiven = (material.samplers & ~(1u << kShadowMaskSlot)) | (pipeline.shadowMaskSampler ? 1u << kShadowMaskSlot : 0u);
			if (usage.samplers & samplerMask & ~samplersGiven) {
				fail(Skip::Sampler);
				return false;
			}
			constexpr std::uint32_t registerMask = kConstantBufferRegisters >= 32 ? ~0u : (1u << kConstantBufferRegisters) - 1;
			constexpr std::uint32_t rowRegisters = (1u << kPerTechnique) | (1u << kPerMaterial) | (1u << kPerGeometry) | (1u << 4);
			const std::uint32_t rowsMissing = (material.blocksOk ? 0u : 1u << kPerMaterial) | (pipeline.blocksOk ? 0u : rowRegisters & ~(1u << kPerMaterial));
			const std::uint32_t vsUsed = usage.vertexConstants & registerMask, psUsed = usage.pixelConstants & registerMask;
			const std::uint32_t pass = (1u << 3) | (1u << kFrameLightingRegister);
			const std::uint32_t vsMissing = (vsUsed & rowRegisters & rowsMissing) | (vsUsed & ~rowRegisters & ~frameVSBits[j]);
			const std::uint32_t psMissing = (psUsed & rowRegisters & rowsMissing) | (psUsed & ~rowRegisters & ~(pass | framePSBits[j]));
			a_work.missingVertexConstants |= vsMissing;
			a_work.missingPixelConstants |= psMissing;
			if (vsMissing || psMissing) {
				fail(Skip::Constants);
				return false;
			}
			return true;
		};
		// Both segments or neither. Colour is checked first since its skip is the report's.
		if (colourBuilt && !reads(kAsyncColour))
			return;
		// Forward keys are not depth segment inputs, so their depth variant is never drawn.
		// The Effect port's depth variant is drawn by the colour decal depth pass, which the check above covered.
		const auto rasterFlags = tables.pipelines[p].rasterFlags;
		if (segmentIn[kAsyncZPrepass] && !RasterForward(rasterFlags) && Extensions::FamilyRules(rasterFlags).depthSegment && !reads(kAsyncZPrepass)) {
			// Colour alone would test EQUAL against depth nobody wrote, so neither segment draws it.
			static std::atomic<std::uint32_t> logged{ 0 };
			if (colourBuilt && logged.fetch_add(1, std::memory_order_relaxed) < 8)
				logger::info("[DCLF] main pair (material {}, pipeline {}): its colour variant is given what it reads, its depth variant not ({}): neither segment "
							 "draws it <- SEGMENT SPLIT",
					m, p, kSkipNames[resolved.skipReason]);
			return;
		}
		resolved.recordIndex = RowsOf(p, m);
	}

	std::array<std::uint32_t, 8> MainBuild::MaterialRowKey(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks) const
	{
		// The technique's bindings matter for filter modes a material leaves unwritten.
		const Lookups::Material* lookup = m < lookups.materials.size() ? &lookups.materials[m] : nullptr;
		// An effect key has no ProjectedUV bit. Its bit 15 is Membrane's, which is not ported.
		const bool projected = (tables.pipelines[p].passDescriptor & Extensions::FamilyRules(tables.pipelines[p].rasterFlags).projectedPassBit) != 0;
		return { m < tables.materialVersion.size() ? tables.materialVersion[m] : 0u, m < tables.materialFrameVersion.size() ? tables.materialFrameVersion[m] : 0u,
			lookup ? lookup->version : ~0u, lookups.sharedVersion, tables.TechniqueOf(p).bindingVersion, projected ? 1u : 0u, static_cast<std::uint32_t>(blocks.tables),
			static_cast<std::uint32_t>(blocks.tables >> 32) };
	}

	void MainBuild::WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, PairWork& a_work)
	{
		auto& state = rows.materials[m];
		const auto key = MaterialRowKey(m, p, blocks);
		if (state.written && state.key == key)
			return;
		if (state.written)
			for (std::size_t k = 0; k < key.size(); ++k)
				a_work.keyMoved[k] += state.key[k] != key[k] ? 1u : 0u;
		MaterialRow row;
		PackMaterialRow(m, p, blocks, key, state, row);
		// Read only here. Finish assigns and releases animated rows.
		HeaderOf(row).animatedRow = MainRows::AnimatedRows::Animated(state) ? rows.animated.RowOf(m) : ~0u;
		// Mark only the parts that moved, so a backing catches up just those. An animated effect moves one part, not the row.
		if (!a_work.deferred) {
			if (rows.material.SetParts(m, row, kMaterialRowParts))
				++a_work.materialRowsWritten;
			rows.productDirty.Mark(m);
		} else {
			const std::uint64_t parts = WriteKeptParts(rows.material, m, row, kMaterialRowParts);
			a_work.rowMarks.emplace_back(m, parts);
			a_work.materialRowsWritten += parts ? 1u : 0u;
		}
		++a_work.materialsWritten;
	}

	void MainBuild::PackMaterialBlocks(std::uint32_t p, const MaterialRecord& material, const PipelineBlocks& blocks, MainRows::MaterialState& state, MaterialRow& row) const
	{
		// PerMaterial blocks are packed in the record's shader variable space through the technique's tables.
		// A record of another shader than its key's gets no row.
		const std::uint8_t keyKind = Extensions::FamilyRules(tables.pipelines[p].rasterFlags).kind;
		const auto& recordSpace = Extensions::MaterialSpaceOf(material.kind);
		const auto& keySpace = Extensions::MaterialSpaceOf(keyKind);
		const auto& vsLayout = recordSpace.vs.layout;
		const auto& psLayout = recordSpace.ps.layout;
		const std::uint64_t vsGroup = keySpace.vs.groups[kPerMaterial];
		const std::uint64_t psGroup = keySpace.ps.groups[kPerMaterial];
		const std::uint32_t vsFirst = keySpace.vs.first[kPerMaterial];
		const std::uint32_t psFirst = keySpace.ps.first[kPerMaterial];
		const auto vsSize = ConstantGroupSize(vsLayout, blocks.vsTable, vsGroup, vsFirst);
		const auto psSize = ConstantGroupSize(psLayout, blocks.psTable, psGroup, psFirst);
		state.blocksOk = vsSize <= kMaterialRowVSBytes && psSize <= kMaterialRowPSBytes && keyKind == material.kind;
		if (state.blocksOk) {
			PackConstantGroup(material.vs, vsLayout, blocks.vsTable, vsGroup, vsFirst, std::span(row.bytes.data() + kMaterialRowVS, kMaterialRowVSBytes));
			PackConstantGroup(material.ps, psLayout, blocks.psTable, psGroup, psFirst, std::span(row.bytes.data() + kMaterialRowPS, kMaterialRowPSBytes));
			// The row holds the record's values. A feature's frame patch overwrites the frame-owned ones.
			PlaceFrameVariables(material, keyKind, { vsLayout, blocks.vsTable, vsGroup, vsFirst }, { psLayout, blocks.psTable, psGroup, psFirst }, state);
		}
	}

	void MainBuild::PackMaterialTextures(std::uint32_t m, bool projected, const Lookups::Material* lookup, const MaterialRecord& material, MainRows::MaterialState& state,
		MaterialRowHeader& header) const
	{
		header.feedbackSlot = m;
		header.feedbackRef = TextureFeedback::Ref();
		// The material's textures, else a projected technique's, else the null texture. t14 is the pipeline row's.
		const bool resolved = lookup && lookup->resolved && lookup->key.first == tables.materialSlotKey[m].first && lookup->key.second == tables.materialSlotKey[m].second;
		// A character-light pass reads the frame's kCharacterLightRegister instead of t11, so t11 gets the null texture.
		const std::uint32_t textureWritten = material.textureWritten &
		                                     ~(MaterialSources::FrameCharacterLight(tables.materialSlotKey[m].second) ? 1u << kCharacterLightMaterialRegister : 0u);
		state.texturesOk = true;
		for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
			std::uint32_t index = lookups.nullTexture;
			if ((textureWritten >> t) & 1) {
				if (!resolved) {
					state.texturesOk = false;
					state.deferred = true;
					state.missingTexture = t;
					break;
				}
				index = lookup->textureIndex[t];
			} else if (const auto slot = ProjectedSlot(projected, t); slot >= 0) {
				index = lookups.projectedTextures[slot];
			}
			if (t == kShadowMaskSlot)
				continue;
			header.textures[t] = index == kInvalidIndex ? 0u : index;
			state.textures |= index == kInvalidIndex ? 0u : (1u << t);
		}
		for (std::uint32_t f = 0; f < Extensions::kFeatureTextureSlots && state.texturesOk; ++f) {
			std::uint32_t index = lookups.nullTexture;
			if (material.featureTextures[f]) {
				if (!resolved) {
					state.texturesOk = false;
					state.deferred = true;
					state.missingTexture = Extensions::FeatureTextureRegister(f);
					break;
				}
				index = lookup->featureIndex[f];
			}
			header.features[f] = index == kInvalidIndex ? 0u : index;
			state.features |= index == kInvalidIndex ? 0u : (1u << f);
		}
	}

	void MainBuild::PackMaterialSamplers(std::uint32_t p, bool projected, const MaterialRecord& material, MainRows::MaterialState& state, MaterialRowHeader& header) const
	{
		const auto& technique = tables.TechniqueOf(p).value;
		// The material's sampler modes, using the technique's filter where unwritten.
		for (std::uint32_t sampler = 0; sampler < kSamplerRegisters; ++sampler) {
			if (sampler == kShadowMaskSlot)
				continue;
			std::uint32_t address = 0, filter = 0;
			if ((material.textureWritten >> sampler) & 1) {
				address = material.addressModes[sampler];
				filter = material.filterModes[sampler] != kUnwrittenFilterMode ? material.filterModes[sampler] : technique.filterModes[sampler];
			} else if (ProjectedSlot(projected, sampler) >= 0) {
				address = 3;  // SetupGeometry: wrap, anisotropic (engine notes: samplers)
				filter = 1;
			}
			if (filter == kUnwrittenFilterMode)
				filter = 0;
			const auto index = lookups.Sampler(address, filter);
			header.samplers[sampler] = index == kInvalidIndex ? 0u : index;
			state.samplers |= index == kInvalidIndex ? 0u : (1u << sampler);
		}
	}

	void MainBuild::PackMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const std::array<std::uint32_t, 8>& a_key, MainRows::MaterialState& state,
		MaterialRow& row) const
	{
		const Lookups::Material* lookup = m < lookups.materials.size() ? &lookups.materials[m] : nullptr;
		const bool projected = a_key[5] != 0;
		const auto& material = tables.materials[m];
		state = {};
		PackMaterialBlocks(p, material, blocks, state, row);
		auto& header = HeaderOf(row);
		header.animatedRow = ~0u;
		PackMaterialTextures(m, projected, lookup, material, state, header);
		PackMaterialSamplers(p, projected, material, state, header);
		state.tables = blocks.tables;
		state.key = a_key;
		state.written = true;
	}

	// The resident region keeps draw inputs across frames. They change only with the change log, a pipeline's set index,
	// or a pair's record failing. The loop skips its objects.
	void MainBuild::BuildResidentRegion()
	{
		region = cache ? &cache->region : nullptr;
		if (!region && cache && cache->region.cursor.active)
			cache->region.Reset();
		wholeScene = region != nullptr;
		TracyCZoneN(residentRegionZone, "CS.DCLF.BuildMain.ResidentRegion", true);
		if (region) {
			{
				DCLF_ZONE("CS.DCLF.BuildMain.Region.Entries");
				UpdateRegionEntries();
			}
			{
				DCLF_ZONE("CS.DCLF.BuildMain.Region.Pairs");
				UpdateRegionPairs();
			}
			{
				DCLF_ZONE("CS.DCLF.BuildMain.Region.Route");
				RouteTouched();
			}
			{
				DCLF_ZONE("CS.DCLF.BuildMain.Region.Publish");
				PublishRegion();
			}
		}
		TracyCZoneEnd(residentRegionZone);
	}

	bool MainBuild::RegionEligible(std::uint32_t o) const
	{
		if (o >= tables.objects.size())
			return false;
		const bool resident = ResidentAt(o);
		if (!resident && !wholeScene)
			return false;
		const auto& object = tables.objects[o];
		if (object.flags & (kObjectFree | kObjectShadowOnly))
			return false;
		// A decal past the buffers stays as a blank entry so its group's range keeps every slot written.
		if (!ObjectFits(tables, o, in.addresses.fit) && !ObjectDecalGroup(object.flags))
			return false;
		if (object.flags & kObjectNoBindings)
			return wholeScene && object.geometryIndex < tables.geometries.size();
		if (object.pipelineIndex >= pipelineBlocks.size() || object.pipelineIndex >= tables.pipelines.size() || object.geometryIndex >= tables.geometries.size())
			return false;
		const auto& geometry = tables.geometries[object.geometryIndex];
		if (!geometry.vertexAddress || !geometry.indexAddress)
			return false;
		// A decal needs its ordinal within its group's range. OrderDecals logs every move.
		if (const std::uint32_t group = ObjectDecalGroup(object.flags))
			if (o >= tables.rangeOrdinal.size() || tables.rangeOrdinal[o] >= decalCount[group - 1])
				return false;
		// A face shape or second-stream position needs its vertex stream row.
		const bool needsStream = IsStreamObject(tables, o) || (tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream);
		return !needsStream || StreamGeometry(tables, o, in.addresses.streamBuffer) != ~0u;
	}

	std::uint8_t MainBuild::RegionEntry(std::uint32_t o, DrawInput& a_input) const
	{
		auto& r = *region;
		const auto& object = tables.objects[o];
		if (object.flags & kObjectNoBindings) {
			a_input = { 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, o, 0 };
			SetFadeRow(a_input, objectRoots, tables, o);
			return 0;
		}
		if (ObjectDecalGroup(object.flags) && !ObjectFits(tables, o, in.addresses.fit)) {
			a_input = DecalBlank(o);
			return 0;
		}
		const auto& blocks = pipelineBlocks[object.pipelineIndex];
		const auto pair = r.pairs.find(PairKeyOf(object));
		// One slot per partition. An undrawable decal is still an input, drawn with zero counts.
		const bool decal = ObjectDecalGroup(object.flags) != 0;
		const std::uint32_t ordinal = decal ? tables.rangeOrdinal[o] : 0u;
		const std::uint32_t partitions = PartitionsOf(tables, o);
		const bool drawable = InSet(o) && blocks.setIndex != Lookups::kNone && pair != r.pairs.end() && pair->second.ok;
		a_input = { drawable ? blocks.setIndex : 0u, drawable ? pair->second.slot : 0u, object.geometryIndex, object.flags | (drawable ? kInputDrawable : 0u),
			{ MainMaskOf(tables, o) }, o, ordinal, partitions, StreamGeometry(tables, o, in.addresses.streamBuffer) };
		SetFadeRow(a_input, objectRoots, tables, o);
		if (!drawable)
			return 0;
		return decal ? kRegionDecal : static_cast<std::uint8_t>(PartitionDraws(partitions));
	}

	void MainBuild::RegionAcquire(std::uint64_t a_key, std::uint32_t o)
	{
		auto& r = *region;
		auto [found, fresh] = r.pairs.try_emplace(a_key);
		auto& pair = found->second;
		if (fresh) {
			pair.slot = kNoRecord;
			pair.ok = false;
			pair.skip = kNoSkip;
			auto& materialList = r.materialPairs[static_cast<std::uint32_t>(a_key >> 32)];
			pair.materialAt = static_cast<std::uint32_t>(materialList.size());
			materialList.push_back(a_key);
			auto& pipelineList = r.pipelinePairs[static_cast<std::uint32_t>(a_key)];
			pair.pipelineAt = static_cast<std::uint32_t>(pipelineList.size());
			pipelineList.push_back(a_key);
			r.freshPairs.push_back(a_key);
		}
		if (r.pairObjectAt.size() <= o)
			r.pairObjectAt.resize(std::max<std::size_t>(std::size_t(o) + 1, tables.objects.size()), 0);
		r.pairObjectAt[o] = static_cast<std::uint32_t>(pair.objects.size());
		pair.objects.push_back(o);
		const auto pipeline = static_cast<std::uint32_t>(a_key);
		auto [state, freshPipeline] = r.pipelines.try_emplace(pipeline, std::pair{ pipelineBlocks[pipeline].setIndex, 0u });
		++state->second.second;
	}

	void MainBuild::RegionRelease(std::uint64_t a_key, std::uint32_t o)
	{
		auto& r = *region;
		if (a_key == kNoPair)
			return;
		if (const auto found = r.pairs.find(a_key); found != r.pairs.end()) {
			auto& pair = found->second;
			if (const std::uint32_t at = o < r.pairObjectAt.size() ? r.pairObjectAt[o] : ~0u; at < pair.objects.size() && pair.objects[at] == o) {
				const std::uint32_t moved = pair.objects.back();
				pair.objects[at] = moved;
				r.pairObjectAt[moved] = at;
				pair.objects.pop_back();
			}
			if (pair.objects.empty()) {
				if (!pair.ok && pair.skip != kNoSkip)
					--r.skipCounts[pair.skip];
				SetPairRegisters(pair, {});
				ReleaseOwner(pair.bindingSeen);
				ReleaseOwner(pair.maskSeen);
				// Remove by place. The last one takes its spot.
				auto unlist = [&](auto& a_lists, std::uint32_t a_list, std::uint32_t a_at, std::uint32_t ResidentRegion::Pair::*a_place) {
					const auto list = a_lists.find(a_list);
					if (list == a_lists.end())
						return;
					auto& keys = list->second;
					if (a_at < keys.size() && keys[a_at] == a_key) {
						const std::uint64_t moved = keys.back();
						keys[a_at] = moved;
						keys.pop_back();
						if (moved != a_key)
							if (const auto other = r.pairs.find(moved); other != r.pairs.end())
								other->second.*a_place = a_at;
					}
					if (keys.empty())
						a_lists.erase(list);
				};
				unlist(r.materialPairs, static_cast<std::uint32_t>(a_key >> 32), pair.materialAt, &ResidentRegion::Pair::materialAt);
				unlist(r.pipelinePairs, static_cast<std::uint32_t>(a_key), pair.pipelineAt, &ResidentRegion::Pair::pipelineAt);
				r.pairs.erase(found);
			}
		}
		if (const auto state = r.pipelines.find(static_cast<std::uint32_t>(a_key)); state != r.pipelines.end() && --state->second.second == 0)
			r.pipelines.erase(state);
	}

	void MainBuild::SetPairRegisters(ResidentRegion::Pair& a_pair, const std::array<std::uint64_t, 2>& a_registers)
	{
		auto& r = *region;
		for (std::size_t w = 0; w < 2; ++w) {
			const std::uint64_t held = a_pair.frameRegisters[w], now = a_registers[w];
			for (std::uint64_t gone = held & ~now; gone; gone &= gone - 1) {
				const auto bit = static_cast<std::uint32_t>(std::countr_zero(gone));
				if (--r.frameRegisterCounts[w * 64 + bit] == 0)
					r.frameRegisters[w] &= ~(1ull << bit);
			}
			for (std::uint64_t added = now & ~held; added; added &= added - 1) {
				const auto bit = static_cast<std::uint32_t>(std::countr_zero(added));
				if (r.frameRegisterCounts[w * 64 + bit]++ == 0)
					r.frameRegisters[w] |= 1ull << bit;
			}
		}
		a_pair.frameRegisters = a_registers;
	}

	void MainBuild::AcquireOwner(const void* a_owner, const std::shared_ptr<const void>& a_held)
	{
		if (!a_owner)
			return;
		auto& r = *region;
		auto [owner, fresh] = r.ownerCounts.try_emplace(a_owner, std::pair{ a_held, 0u });
		r.ownersChanged |= fresh;
		++owner->second.second;
	}

	void MainBuild::ReleaseOwner(const void* a_owner)
	{
		if (!a_owner)
			return;
		auto& r = *region;
		if (const auto owner = r.ownerCounts.find(a_owner); owner != r.ownerCounts.end() && --owner->second.second == 0) {
			r.ownerCounts.erase(owner);
			r.ownersChanged = true;
		}
	}

	void MainBuild::SetPairOwners(ResidentRegion::Pair& a_pair, std::uint32_t m, std::uint32_t p, const void* a_binding, const void* a_mask)
	{
		// The counted owner keeps its object alive, so a new owner never reuses a counted one's address.
		if (a_binding != a_pair.bindingSeen) {
			AcquireOwner(a_binding, a_binding ? lookups.materials[m].bindingBlock : nullptr);
			ReleaseOwner(a_pair.bindingSeen);
			a_pair.bindingSeen = a_binding;
		}
		if (a_mask != a_pair.maskSeen) {
			AcquireOwner(a_mask, a_mask ? lookups.pipelines[p].shadowMaskOwner : nullptr);
			ReleaseOwner(a_pair.maskSeen);
			a_pair.maskSeen = a_mask;
		}
	}

	void MainBuild::RegionSetDraws(std::uint32_t i, std::uint8_t a_draws)
	{
		auto& r = *region;
		r.undrawable += (a_draws ? 0 : 1) - (r.drawsOf[i] ? 0 : 1);
		r.draws = r.draws - RegionDraws(r.drawsOf[i]) + RegionDraws(a_draws);
		r.decals = r.decals - (r.drawsOf[i] == kRegionDecal ? 1 : 0) + (a_draws == kRegionDecal ? 1 : 0);
		r.drawsOf[i] = a_draws;
	}

	DrawInput MainBuild::DecalBlank(std::uint32_t o) const
	{
		const auto& object = tables.objects[o];
		// Not drawable. The partitions word names the decal's slots, each a zero-count draw.
		return { 0, 0, object.geometryIndex < tables.geometries.size() ? object.geometryIndex : 0u, object.flags, { MainMaskOf(tables, o) }, o, tables.rangeOrdinal[o],
			o < tables.skinPartitions.size() ? tables.skinPartitions[o] : 0u };
	}

	void MainBuild::RegionWrite(std::uint32_t i, const DrawInput& a_entry)
	{
		auto& r = *region;
		if (org::async::keptStorageOptions.reuseStorage)
			r.inputs.Set(i, a_entry);
		else {
			r.inputs.Mutable()[i] = a_entry;
			r.inputs.Mark(i);
		}
	}

	void MainBuild::RegionRemove(std::uint32_t o)
	{
		auto& r = *region;
		const std::uint32_t i = r.EntryOf(o);
		if (i == kNoRegion || !(r.sideOf[i] & kSideMain))
			return;
		RegionRelease(r.pairOf[i], o);
		r.touched.push_back(o);
		r.undrawable -= r.drawsOf[i] ? 0 : 1;
		r.draws -= RegionDraws(r.drawsOf[i]);
		r.decals -= r.drawsOf[i] == kRegionDecal ? 1 : 0;
		if (r.sideOf[i] & kSideShadow) {
			// The shadow build's words stay and the entry becomes its alone.
			r.pairOf[i] = kNoPair;
			r.drawsOf[i] = 0;
			r.sideOf[i] = kSideShadow;
			DrawInput entry = r.inputs.Get()[i];
			ClearMainPart(entry);
			RegionWrite(i, entry);
			return;
		}
		r.RemoveEntry(o);
	}

	void MainBuild::RegionUpsert(std::uint32_t o)
	{
		auto& r = *region;
		if (!RegionEligible(o)) {
			RegionRemove(o);
			return;
		}
		r.Cover(tables.objects.size());
		const auto& object = tables.objects[o];
		const std::uint64_t key = (object.flags & kObjectNoBindings) ? kNoPair : PairKeyOf(object);
		std::uint32_t i = r.indexOf[o];
		if (i == kNoRegion || !(r.sideOf[i] & kSideMain)) {
			// Region and loop together hold at most one input per object, which the buffers fit. A shadow-only entry takes the main part too.
			if (i == kNoRegion)
				i = r.AddEntry(o, DrawInput{}, kSideMain);
			else
				r.sideOf[i] |= kSideMain;
			r.pairOf[i] = key;
			++r.undrawable;
			if (key != kNoPair)
				RegionAcquire(key, o);
		} else if (r.pairOf[i] != key) {
			RegionRelease(r.pairOf[i], o);
			if (key != kNoPair)
				RegionAcquire(key, o);
			r.pairOf[i] = key;
		}
		DrawInput fresh{};
		RegionSetDraws(i, RegionEntry(o, fresh));
		DrawInput entry = r.inputs.Get()[i];
		SetMainPart(entry, fresh);
		RegionWrite(i, entry);
		r.touched.push_back(o);
	}

	void MainBuild::RegionTake(std::uint32_t o)
	{
		auto& r = *region;
		if (!ResidentAt(o) && !wholeScene)
			RegionRemove(o);
		else
			RegionUpsert(o);
		r.touched.push_back(o);
	}

	void MainBuild::UpdateRegionEntries()
	{
		auto& r = *region;
		r.touched.clear();
		// Once per publication, by whichever build writes first.
		r.Begin(in.inputsOldestContentEpoch);
		const std::array<bool, kResyncReasons> reasons{ !r.cursor.Continues(tables.changeLog, in.tablesGeneration), r.segments != Segments(),
			r.indexOf.size() > tables.objects.size(), r.wholeScene != wholeScene, r.fit != in.addresses.fit };
		const bool resync = std::find(reasons.begin(), reasons.end(), true) != reasons.end();
		if (resync) {
			for (std::size_t k = 0; k < reasons.size(); ++k)
				out.residentResyncReasons |= reasons[k] ? 1u << k : 0u;
			r.Reset();
			r.fit = in.addresses.fit;
			r.decalCount = decalCount;
			r.cursor.Restart(in.tablesGeneration);
			r.segments = Segments();
			r.wholeScene = wholeScene;
			r.indexOf.assign(tables.objects.size(), kNoRegion);
			++out.residentResyncs;
			++rows.pairStats.resyncs;
			if (tables.objects.size() >= kResyncFanoutMin) {
				ResyncRegionParallel();
			} else {
				for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
					RegionTake(o);
			}
		} else {
			// Placement is the object record's, not the input's.
			constexpr std::uint32_t kInputCauses = kChangeBindings | kChangeSkin | kChangeGeometry | kChangeMembership;
			for (const auto& change : r.cursor.Unread(tables.changeLog))
				if (change.causes & kInputCauses)
					RegionTake(change.slot);
			// Stream row indices count from the geometry slots. When that count or the stream buffer changes, retake every stream object.
			if (const std::size_t base = in.addresses.streamBuffer ? tables.geometries.size() : 0; base != r.streamBase) {
				for (const auto& row : tables.streamRows)
					if (row.object != Api::VertexStreamRow::kNoObject && row.object < tables.objects.size())
						RegionTake(row.object);
			}
			// A decal group's capped count moved. Retake the decals between the old and new counts.
			if (r.decalCount != decalCount) {
				++out.residentDecalRetakes;
				for (std::uint32_t o = 0; o < tables.rangeOrdinal.size() && o < tables.objects.size(); ++o) {
					const std::uint32_t ordinal = tables.rangeOrdinal[o];
					const std::uint32_t group = ordinal != ~0u ? ObjectDecalGroup(tables.objects[o].flags) : 0;
					if (group && ordinal >= std::min(r.decalCount[group - 1], decalCount[group - 1]) &&
						ordinal < std::max(r.decalCount[group - 1], decalCount[group - 1]))
						RegionTake(o);
				}
				r.decalCount = decalCount;
			}
		}
		r.streamBase = in.addresses.streamBuffer ? tables.geometries.size() : 0;
		r.cursor.Advance(tables.changeLog);
	}

	void MainBuild::ResyncRegionParallel()
	{
		auto& r = *region;
		const std::size_t count = tables.objects.size();
		// 1. Per slot on the pool. The Reset emptied the pairs, so every entry waits for UpdateRegionPairs, as on the serial path.
		std::vector<std::uint8_t> eligible(count, 0), draws(count, 0);
		std::vector<std::uint64_t> keys(count, kNoPair);
		std::vector<DrawInput> entries(count);
		auto& executor = ::RenderGraph::Frame::SharedExecutor();
		executor.ParallelFor("CS.DCLF.BuildMain.Region.Resync", count, kResyncGrain, [&](std::size_t a_begin, std::size_t a_end) {
			DCLF_ZONE("CS.DCLF.BuildMain.Region.ResyncShards");
			for (std::size_t n = a_begin; n < a_end; ++n) {
				const auto o = static_cast<std::uint32_t>(n);
				if (!RegionEligible(o))
					continue;
				eligible[n] = 1;
				const auto& object = tables.objects[o];
				keys[n] = (object.flags & kObjectNoBindings) ? kNoPair : PairKeyOf(object);
				DrawInput fresh{};
				draws[n] = RegionEntry(o, fresh);
				SetMainPart(entries[n], fresh);
			}
		});
		// 2. In slot order, as the serial path does. Prefix-sum the entry indices and acquire the pairs.
		std::size_t held = 0;
		for (std::size_t n = 0; n < count; ++n)
			held += eligible[n];
		r.pairOf.assign(held, kNoPair);
		r.drawsOf.assign(held, 0);
		r.sideOf.assign(held, kSideMain);
		r.Cover(count);
		r.inputs.Mutable().resize(held);
		std::uint32_t i = 0;
		for (std::size_t n = 0; n < count; ++n) {
			const auto o = static_cast<std::uint32_t>(n);
			r.touched.push_back(o);
			if (!eligible[n])
				continue;
			r.indexOf[o] = i;
			r.pairOf[i] = keys[n];
			++r.undrawable;
			if (keys[n] != kNoPair)
				RegionAcquire(keys[n], o);
			RegionSetDraws(i, draws[n]);
			++i;
		}
		// 3. Scatter the entries on the pool, with one journal resync for them all.
		PrepareMainKeptWrites(r.inputs, 0, held);
		auto& list = r.inputs.Mutable();
		executor.ParallelFor("CS.DCLF.BuildMain.Region.Scatter", count, kResyncGrain, [&](std::size_t a_begin, std::size_t a_end) {
			DCLF_ZONE("CS.DCLF.BuildMain.Region.ScatterShards");
			for (std::size_t n = a_begin; n < a_end; ++n)
				if (eligible[n])
					list[r.indexOf[n]] = entries[n];
		});
		r.inputs.Resync();
		++rows.pairStats.resyncsFannedOut;
	}

	std::uint64_t MainBuild::PipelineWitness(std::uint32_t p) const
	{
		// A hash of what a pair's verdict and row packing read from the pipeline.
		// Not its constants, which move the row's bytes but never a verdict.
		std::uint64_t value = Util::Fnv1a::kOffset;
		const auto& blocks = pipelineBlocks[p];
		value = Mix(value, lookups.sharedVersion);
		value = Mix(value, tables.TechniqueOf(p).bindingVersion);
		value = Mix(value, (tables.pipelines[p].passDescriptor & 0x8000u) != 0 ? 1u : 0u);
		value = Mix(value, blocks.tables);
		value = Mix(value, blocks.setIndex);
		value = Mix(value, tables.pipelines[p].rasterFlags);
		for (const auto* usage : blocks.usage) {
			if (!usage)
				continue;
			value = Mix(value, (std::uint64_t(usage->vertexConstants) << 32) | usage->pixelConstants);
			value = Mix(value, usage->textures[0]);
			value = Mix(value, usage->textures[1]);
			value = Mix(value, usage->samplers);
		}
		const auto& pipeline = rows.pipelines[p];
		value = Mix(value, (pipeline.written ? 1u : 0u) | (pipeline.blocksOk ? 2u : 0u) | (pipeline.shadowMask ? 4u : 0u) | (pipeline.shadowMaskSampler ? 8u : 0u));
		return Mix(value, reinterpret_cast<std::uintptr_t>(p < lookups.pipelines.size() ? lookups.pipelines[p].shadowMaskOwner.get() : nullptr));
	}

	void MainBuild::ResolvePairsChunk(const std::vector<std::uint64_t>& a_check, std::size_t a_begin, std::size_t a_end, bool a_everyPair, PairWork& a_work)
	{
		// No inserts or erases meanwhile, and each pair is a distinct slot, so chunks can run in parallel.
		auto& r = *region;
		for (std::size_t k = a_begin; k < a_end; ++k) {
			const std::uint64_t key = a_check[k];
			const auto found = r.pairs.find(key);
			if (found == r.pairs.end())
				continue;
			auto& pair = found->second;
			const auto p = static_cast<std::uint32_t>(key);
			const auto m = static_cast<std::uint32_t>(key >> 32);
			const bool pipelineOk = p < pipelineBlocks.size() && pipelineBlocks[p].setIndex != Lookups::kNone;
			const void* binding = m < lookups.materials.size() ? lookups.materials[m].bindingBlock.get() : nullptr;
			const void* mask = p < lookups.pipelines.size() ? lookups.pipelines[p].shadowMaskOwner.get() : nullptr;
			// The row always follows the material's frame values. The verdict reruns only when the pipeline witness or row state moved.
			std::uint64_t witness = 1ull;
			if (pipelineOk) {
				WriteMaterialRow(m, p, pipelineBlocks[p], a_work);
				witness = Mix(r.pipelineWitnessNow[p], MaterialStateWitness(rows.materials[m])) | 1;  // never 0, which is "never resolved"
			}
			if (!a_everyPair && witness == pair.witness && binding == pair.bindingSeen && mask == pair.maskSeen)
				continue;
			PairWork::Verdict verdict{ &pair, key, witness, pair.slot, kNoSkip, pipelineOk, {}, binding, mask };
			if (pipelineOk) {
				ResolvedBindings resolved;
				ResolvePair(m, p, pipelineBlocks[p], rows.materials[m], resolved, verdict.registers, a_work);
				if (resolved.skipReason != kNoSkip) {
					// Counted once per resolution, as AssembleRecord would refuse a draw.
					verdict.ok = false;
					verdict.skip = resolved.skipReason;
					if (resolved.skipReason == static_cast<std::uint32_t>(Skip::Texture))
						a_work.missingTextures.push_back(resolved.missingTexture);
					if (resolved.deferred)
						++a_work.deferredTextures;
					++a_work.skipped[resolved.skipReason];
				} else {
					verdict.slot = resolved.recordIndex;
				}
			}
			a_work.verdicts.push_back(verdict);
		}
	}

	void MainBuild::ChangedPipelineSets(std::vector<std::uint32_t>& a_changed)
	{
		for (auto& [pipeline, state] : region->pipelines)
			if (pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != state.first) {
				state.first = pipelineBlocks[pipeline].setIndex;
				a_changed.push_back(pipeline);
			}
	}

	bool MainBuild::EveryPairMoved()
	{
		auto& r = *region;
		std::uint64_t frameWitness = Util::Fnv1a::kOffset ^ Segments();
		for (const auto* segment : segmentIn)
			if (segment)
				frameWitness = Mix(frameWitness, (std::uint64_t(segment->vsFrameMask) << 32) | segment->psFrameMask);
		frameWitness |= 1;
		bool everyPair = frameWitness != r.frameWitness;
		r.frameWitness = frameWitness;
		if (!r.materialCursor.Continues(tables.materialLog, in.tablesGeneration) || !r.lookupCursor.Continues(lookups.materialLog, lookups.logGeneration)) {
			everyPair = true;
			r.materialCursor.Restart(in.tablesGeneration);
			r.lookupCursor.Restart(lookups.logGeneration);
		}
		return everyPair;
	}

	void MainBuild::MovedPipelines(std::vector<std::uint32_t>& a_moved)
	{
		auto& r = *region;
		TracyCZoneN(pairsPipelinesZone, "CS.DCLF.BuildMain.Pairs.Pipelines", true);
		r.pipelineWitnessNow.assign(pipelineBlocks.size(), 0);
		for (const auto& [pipeline, state] : r.pipelines) {
			if (pipeline >= pipelineBlocks.size())
				continue;
			if (pipelineBlocks[pipeline].setIndex != Lookups::kNone)
				r.pipelineWitnessNow[pipeline] = PipelineWitness(pipeline);
			auto& seen = r.pipelineWitness[pipeline];
			if (seen != r.pipelineWitnessNow[pipeline])
				a_moved.push_back(pipeline);
			seen = r.pipelineWitnessNow[pipeline];
		}
		TracyCZoneEnd(pairsPipelinesZone);
	}

	void MainBuild::PairsToCheck(bool a_everyPair, const std::vector<std::uint32_t>& a_moved, std::vector<std::uint64_t>& a_check)
	{
		auto& r = *region;
		auto& stats = rows.pairStats;
		if (a_everyPair) {
			++stats.everyPair;
			a_check.reserve(r.pairs.size());
			for (const auto& [key, pair] : r.pairs)
				a_check.push_back(key);
		} else {
			auto listed = [&](const auto& a_lists, std::uint32_t a_list) {
				if (const auto list = a_lists.find(a_list); list != a_lists.end())
					a_check.insert(a_check.end(), list->second.begin(), list->second.end());
			};
			for (const std::uint32_t m : r.materialCursor.Unread(tables.materialLog))
				listed(r.materialPairs, m);
			for (const std::uint32_t m : r.lookupCursor.Unread(lookups.materialLog))
				listed(r.materialPairs, m);
			for (const std::uint32_t p : a_moved)
				listed(r.pipelinePairs, p);
			a_check.insert(a_check.end(), r.freshPairs.begin(), r.freshPairs.end());
		}
		// A material's pairs are adjacent once sorted, so chunks split on material boundaries.
		std::sort(a_check.begin(), a_check.end());
		a_check.erase(std::unique(a_check.begin(), a_check.end()), a_check.end());
		stats.pipelinesMoved += a_moved.size();
		stats.checked += a_check.size();
		r.freshPairs.clear();
		r.materialCursor.Advance(tables.materialLog);
		r.lookupCursor.Advance(lookups.materialLog);
	}

	std::vector<PairWork> MainBuild::ResolveCheckedPairs(const std::vector<std::uint64_t>& a_check, bool a_everyPair)
	{
		auto& stats = rows.pairStats;
		const bool fanout = a_check.size() >= kPairFanoutMin;
		std::vector<std::size_t> bounds{ 0 };
		if (fanout)
			for (std::size_t next = kPairGrain; next < a_check.size(); next += kPairGrain) {
				while (next < a_check.size() && (a_check[next] >> 32) == (a_check[next - 1] >> 32))
					++next;
				if (next >= a_check.size())
					break;
				bounds.push_back(next);
			}
		bounds.push_back(a_check.size());
		const std::size_t chunks = bounds.size() - 1;
		std::vector<PairWork> works(chunks);
		if (fanout) {
			// Make the chunks' rows writable here on the builder's thread. MergeWork marks them later.
			for (std::size_t k = 0; k < a_check.size(); ++k)
				if (k == 0 || (a_check[k] >> 32) != (a_check[k - 1] >> 32))
					PrepareMainKeptWrites(rows.material, static_cast<std::size_t>(a_check[k] >> 32), 1);
			for (auto& work : works)
				work.deferred = true;
			::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.BuildMain.Pairs.Resolve", chunks, 1, [&](std::size_t a_begin, std::size_t a_end) {
				DCLF_ZONE("CS.DCLF.BuildMain.Pairs.ResolveShards");
				for (std::size_t c = a_begin; c < a_end; ++c)
					ResolvePairsChunk(a_check, bounds[c], bounds[c + 1], a_everyPair, works[c]);
			});
			++stats.fannedOut;
			stats.chunks += chunks;
		} else {
			ResolvePairsChunk(a_check, 0, a_check.size(), a_everyPair, works[0]);
		}
		return works;
	}

	void MainBuild::ApplyVerdicts(std::vector<PairWork>& a_works, std::vector<std::uint64_t>& a_changedPairs)
	{
		auto& r = *region;
		auto& stats = rows.pairStats;
		// Apply in chunk order.
		for (auto& work : a_works) {
			for (const auto& verdict : work.verdicts) {
				auto& pair = *verdict.pair;
				const auto p = static_cast<std::uint32_t>(verdict.key);
				const auto m = static_cast<std::uint32_t>(verdict.key >> 32);
				if (!pair.ok && pair.skip != kNoSkip)
					--r.skipCounts[pair.skip];
				pair.skip = verdict.skip;
				if (!verdict.ok && verdict.skip != kNoSkip)
					++r.skipCounts[verdict.skip];
				SetPairRegisters(pair, verdict.registers);
				SetPairOwners(pair, m, p, verdict.binding, verdict.mask);
				pair.witness = verdict.witness;
				if (verdict.ok != pair.ok || (verdict.ok && verdict.slot != pair.slot)) {
					pair.ok = verdict.ok;
					if (verdict.ok)
						pair.slot = verdict.slot;
					a_changedPairs.push_back(verdict.key);
				}
			}
			stats.resolved += work.verdicts.size();
			MergeWork(work);
		}
	}

	void MainBuild::BundleRegionPairs()
	{
		auto& r = *region;
		for (std::size_t reason = 0; reason < r.skipCounts.size(); ++reason)
			body.skipped[reason] += r.skipCounts[reason];
		// Rebuild the owner list only when an owner joined or left.
		if (r.ownersChanged) {
			r.ownersChanged = false;
			auto owners = std::make_shared<std::vector<std::shared_ptr<const void>>>();
			owners->reserve(r.ownerCounts.size());
			for (const auto& [pointer, held] : r.ownerCounts)
				owners->push_back(held.first);
			r.owners = std::move(owners);
			++rows.pairStats.bundles;
		}
		out.frameRegisters[0] |= r.frameRegisters[0];
		out.frameRegisters[1] |= r.frameRegisters[1];
		if (r.owners)
			body.bindingOwners.push_back(r.owners);
	}

	void MainBuild::RewriteChangedEntries(const std::vector<std::uint32_t>& a_changedPipelines, const std::vector<std::uint64_t>& a_changedPairs)
	{
		DCLF_ZONE("CS.DCLF.BuildMain.Pairs.Inputs");
		auto& r = *region;
		auto& stats = rows.pairStats;
		if (!a_changedPipelines.empty() || !a_changedPairs.empty()) {
			auto& inputs = r.inputs.Mutable();
			auto rewrite = [&](const ResidentRegion::Pair& a_pair) {
				for (const std::uint32_t o : a_pair.objects) {
					const std::uint32_t i = r.EntryOf(o);
					if (i == kNoRegion || !(r.sideOf[i] & kSideMain))
						continue;
					DrawInput fresh{};
					RegionSetDraws(i, RegionEntry(o, fresh));
					SetMainPart(inputs[i], fresh);
					r.inputs.Mark(i);
					r.touched.push_back(o);
					++stats.entriesRewritten;
				}
			};
			for (const std::uint64_t key : a_changedPairs)
				if (const auto pair = r.pairs.find(key); pair != r.pairs.end())
					rewrite(pair->second);
			for (const std::uint32_t pipeline : a_changedPipelines)
				if (const auto list = r.pipelinePairs.find(pipeline); list != r.pipelinePairs.end())
					for (const std::uint64_t key : list->second)
						if (const auto pair = r.pairs.find(key); pair != r.pairs.end())
							rewrite(pair->second);
		}
	}

	void MainBuild::UpdateRegionPairs()
	{
		auto& stats = rows.pairStats;
		++stats.builds;
		std::vector<std::uint32_t> changedPipelines;
		ChangedPipelineSets(changedPipelines);
		// A pair resolves again only when what it reads changed. Every pair when the frame's half moved or a log was lost.
		const bool everyPair = EveryPairMoved();
		std::vector<std::uint64_t> changedPairs;
		currentObject = ~0u;
		std::vector<std::uint32_t> movedPipelines;
		MovedPipelines(movedPipelines);
		const auto resolveStart = std::chrono::steady_clock::now();
		TracyCZoneN(pairsResolveZone, "CS.DCLF.BuildMain.Pairs.Resolve", true);
		std::vector<std::uint64_t> check;
		PairsToCheck(everyPair, movedPipelines, check);
		auto works = ResolveCheckedPairs(check, everyPair);
		ApplyVerdicts(works, changedPairs);
		TracyCZoneEnd(pairsResolveZone);
		const auto bundleStart = std::chrono::steady_clock::now();
		const double resolveMs = std::chrono::duration<double, std::milli>(bundleStart - resolveStart).count();
		stats.resolveMs += resolveMs;
		stats.resolveMaxMs = std::max(stats.resolveMaxMs, resolveMs);
		TracyCZoneN(pairsBundleZone, "CS.DCLF.BuildMain.Pairs.Bundle", true);
		BundleRegionPairs();
		TracyCZoneEnd(pairsBundleZone);
		const auto inputsStart = std::chrono::steady_clock::now();
		stats.bundleMs += std::chrono::duration<double, std::milli>(inputsStart - bundleStart).count();
		RewriteChangedEntries(changedPipelines, changedPairs);
		stats.inputsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
	}

	void MainBuild::RouteTouched()
	{
		auto& r = *region;
		if (r.loopIndex.size() < tables.objects.size()) {
			r.loopIndex.resize(tables.objects.size(), kNoRegion);
			r.candidate.resize(tables.objects.size(), 0);
		}
		for (const std::uint32_t o : r.touched) {
			if (o >= r.loopIndex.size())
				continue;
			const auto flags = o < tables.objects.size() ? tables.objects[o].flags : kObjectFree;
			const bool inRegion = r.MainHolds(o);
			const bool isCandidate = !(flags & kObjectFree) && (flags & (kObjectNoBindings | kObjectShadowOnly));
			const bool loop = !inRegion && !(flags & (kObjectFree | kObjectShadowOnly));
			if (loop && r.loopIndex[o] == kNoRegion) {
				r.loopIndex[o] = static_cast<std::uint32_t>(r.loopList.size());
				r.loopList.push_back(o);
			} else if (!loop && r.loopIndex[o] != kNoRegion) {
				const std::uint32_t at = r.loopIndex[o];
				const std::uint32_t tail = r.loopList.back();
				r.loopList[at] = tail;
				r.loopIndex[tail] = at;
				r.loopList.pop_back();
				r.loopIndex[o] = kNoRegion;
			}
			if (r.candidate[o] != (isCandidate ? 1 : 0)) {
				r.candidates += isCandidate ? 1 : std::size_t(-1);
				r.candidate[o] = isCandidate ? 1 : 0;
			}
		}
	}

	void MainBuild::PublishRegion()
	{
		auto& r = *region;
		// Per-object states are only for set parity.
		const auto& inputs = r.inputs.Get();
		if (!body.objectState.empty()) {
			for (std::uint32_t i = 0; i < inputs.size(); ++i)
				if ((r.sideOf[i] & kSideMain) && inputs[i].objectIndex < body.objectState.size())
					body.objectState[inputs[i].objectIndex] = r.drawsOf[i] == kRegionDecal ? kObjectStateDecal :
					                                         r.drawsOf[i]                  ? kObjectStateDrawable :
					                                         r.pairOf[i] == kNoPair        ? static_cast<std::uint8_t>(Skip::CandidateOnly) :
					                                         !InSet(inputs[i].objectIndex) ? static_cast<std::uint8_t>(Skip::NotInSet) :
					                                                                         static_cast<std::uint8_t>(Skip::Pipeline);
			for (std::uint32_t o = 0; o < r.candidate.size() && o < body.objectState.size(); ++o)
				if (r.candidate[o] && body.objectState[o] == kObjectStateAbsent)
					body.objectState[o] = static_cast<std::uint8_t>(Skip::CandidateOnly);
		}
		body.skipped[static_cast<std::size_t>(Skip::CandidateOnly)] += static_cast<std::uint32_t>(r.candidates);
		body.residentUndrawable = static_cast<std::uint32_t>(r.undrawable);
		// The shadow build writes the scene list after this, so RunAhead hands out the region's view once both have.
		body.residentDraws = static_cast<std::uint32_t>(r.draws);
		body.decalsDrawn += static_cast<std::uint32_t>(r.decals);
		body.residentPairs = static_cast<std::uint32_t>(r.pairs.size());
		regionInputs = inputs.size();
		regionDraws = r.draws;
	}

	void MainBuild::RunObjectLoop()
	{
		TracyCZoneN(objectLoopZone, "CS.DCLF.BuildMain.ObjectLoop", true);
		const std::vector<std::uint32_t>* loopObjects = region ? &region->loopList : nullptr;
		const std::size_t loopCount = loopObjects ? loopObjects->size() : tables.objects.size();
		if (loopObjects)
			rows.pairStats.loopListed += loopCount;
		for (std::size_t n = 0; n < loopCount; ++n) {
			const std::uint32_t o = loopObjects ? (*loopObjects)[n] : static_cast<std::uint32_t>(n);
			if (o < tables.objects.size())
				LoopObject(o);
		}
		TracyCZoneEnd(objectLoopZone);
	}

	void MainBuild::LoopObject(std::uint32_t o)
	{
		currentObject = o;
		const auto& object = tables.objects[o];
		if (region && region->MainHolds(o))
			return;
		if (object.flags & kObjectFree)
			return;
		if (object.flags & kObjectShadowOnly) {
			if (!region)
				Skipped(Skip::CandidateOnly);
			return;
		}
		// Past the scene's buffers there is no input, except a decal's blank, which keeps its slot as a zero-count draw.
		if (!ObjectFits(tables, o, in.addresses.fit)) {
			if (const std::uint32_t group = ObjectDecalGroup(object.flags);
				group && o < tables.rangeOrdinal.size() && tables.rangeOrdinal[o] < decalCount[group - 1])
				drawInputs.push_back(DecalBlank(o));
			Skipped(Skip::Capacity);
			return;
		}
		if (object.flags & kObjectNoBindings) {
			// No material or pipeline entry. The depth segment submits it cull-only.
			drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, static_cast<std::uint32_t>(o), 0 });
			SetFadeRow(drawInputs.back(), objectRoots, tables, o);
			if (!region)
				Skipped(Skip::CandidateOnly);
			return;
		}
		// Decals are not occluders. Only the colour segment's second pass draws them.
		const std::uint32_t decalGroup = ObjectDecalGroup(object.flags);
		if (decalGroup && (o >= tables.rangeOrdinal.size() || tables.rangeOrdinal[o] >= decalCount[decalGroup - 1]))
			return;
		// An undrawable decal must still reach BuildDraws so its slot is a zero-count draw, not a stale one.
		// This guard pushes the blank on every early return below.
		struct DecalSlot
		{
			std::vector<DrawInput>* inputs = nullptr;
			DrawInput blank{};
			~DecalSlot()
			{
				if (inputs)
					inputs->push_back(blank);
			}
		} decalSlot;
		if (decalGroup) {
			decalSlot.inputs = &drawInputs;
			decalSlot.blank = { 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, static_cast<std::uint32_t>(o), tables.rangeOrdinal[o],
				o < tables.skinPartitions.size() ? tables.skinPartitions[o] : 0u };
		}
		const auto& blocks = pipelineBlocks[object.pipelineIndex];
		if (blocks.setIndex == Lookups::kNone) {
			Skipped(Skip::Pipeline);
			return;
		}
		const auto& geometry = tables.geometries[object.geometryIndex];
		if (!geometry.vertexAddress || !geometry.indexAddress) {
			Skipped(Skip::Geometry);
			return;
		}
		// Outside the set the engine draws depth and colour. DCLF depth would break its EQUAL test against its own prepass.
		if (!InSet(o)) {
			// Still cull-only, because the colour segment reads every candidate's verdict from the depth segment.
			if (!decalGroup) {
				drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, static_cast<std::uint32_t>(o), 0 });
				SetFadeRow(drawInputs.back(), objectRoots, tables, o);
			}
			Skipped(Skip::NotInSet);
			return;
		}
		// A multi-partition skin writes a sequence per drawn partition. A skinned decal such as blood writes one per slot of its span.
		const std::uint32_t partitions = PartitionsOf(tables, o);
		// Without its stream row, the geometry's other attributes would draw as positions.
		const std::uint32_t streamIndex = StreamGeometry(tables, o, in.addresses.streamBuffer);
		if (streamIndex == ~0u && (IsStreamObject(tables, o) || (tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream))) {
			Skipped(Skip::Geometry);
			return;
		}
		// No draw cap. The sequences hold every draw the scene can produce.
		const std::uint32_t recordIndex = AssembleRecord(o, object, blocks);
		if (recordIndex == kNoRecord)
			return;
		// A CPU template of what BuildDraws writes, for the build parity. Row addresses are byte offsets that the parity rebases.
		Mark(2);
		auto sequence = tables.draws[o];
		sequence.pipelineIndex = blocks.setIndex;
		sequence.objectIndex = o;
		sequence.pipelineRowAddress = std::uint64_t(recordIndex >> kRowPipelineShift) * kPipelineRowBytes;
		sequence.materialRowAddress = std::uint64_t(recordIndex & kRowMaterialMask) * kMaterialRowBytes;
		if (streamIndex != ~0u)
			SetSequenceStream(sequence, tables, o, in.addresses.streamBuffer);
		if (decalGroup) {
			const std::uint32_t ordinal = tables.rangeOrdinal[o];
			decalSlot.inputs = nullptr;  // drawn: the blank is not needed
			drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex, object.flags | kInputDrawable, { MainMaskOf(tables, o) },
				static_cast<std::uint32_t>(o), ordinal, partitions, streamIndex });
			decalTemplates[decalGroup - 1][ordinal] = sequence;
			++body.decalsDrawn;
			if (o < body.objectState.size())
				body.objectState[o] = kObjectStateDecal;
		} else {
			if (o < body.objectState.size())
				body.objectState[o] = kObjectStateDrawable;
			drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex, object.flags | kInputDrawable, { MainMaskOf(tables, o) },
				static_cast<std::uint32_t>(o), 0, partitions, streamIndex });
			SetFadeRow(drawInputs.back(), objectRoots, tables, o);
			if (!partitions) {
				sequences.push_back(sequence);
			} else {
				ForEachDrawnGeometry(tables, object.geometryIndex, partitions, [&](std::uint32_t a_slot) {
					auto partitionSequence = sequence;
					SetSequenceGeometry(partitionSequence, tables.geometries[a_slot], streamIndex != ~0u);
					sequences.push_back(partitionSequence);
				});
			}
		}
		// A short vertex or index slice reads zeros and the object silently collapses.
		// Checked here because nothing on the D3D11 side sees the Vulkan slice.
		const std::uint64_t vertexNeeded = std::uint64_t(geometry.vertexCount) * geometry.vertexStride;
		const std::uint64_t indexNeeded = (std::uint64_t(geometry.firstIndex) + geometry.indexCount) * sizeof(std::uint16_t);
		if (geometry.vertexBytes < vertexNeeded || geometry.indexBytes < indexNeeded) {
			if (body.shortBuffers++ == 0)
				body.shortBuffer = { o, vertexNeeded, indexNeeded };
		}
		Mark(3);
	}

	void MainBuild::AssignAnimatedRows()
	{
		// Each animated row holds its row of the frame's copy. A row taken or released here has just its header part rewritten.
		DCLF_ZONE("CS.DCLF.BuildMain.AnimatedRows");
		auto& animated = rows.animated;
		const auto assign = [&](std::uint32_t m) {
			if (m >= rows.materials.size() || m >= rows.material.Size())
				return;
			std::uint32_t row = ~0u;
			if (MainRows::AnimatedRows::Animated(rows.materials[m]))
				row = animated.Assign(m);
			else
				animated.Release(m);
			if (MaterialRowHeader header = *reinterpret_cast<const MaterialRowHeader*>(rows.material.Get()[m].bytes.data() + kMaterialRowHeader); header.animatedRow != row) {
				MaterialRow written = rows.material.Get()[m];
				HeaderOf(written).animatedRow = row;
				rows.material.SetParts(m, written, kMaterialRowParts);
			}
		};
		const auto& dirty = rows.productDirty;
		if (dirty.every) {
			for (std::uint32_t m = 0; m < rows.materials.size(); ++m)
				assign(m);
		} else {
			// Slot order keeps the rows taken independent of chunk order.
			static std::vector<std::uint32_t> slots;  // the builds run one at a time (the snapshot builder's)
			slots.assign(dirty.slots.begin(), dirty.slots.end());
			std::sort(slots.begin(), slots.end());
			for (const std::uint32_t m : slots)
				assign(m);
		}
	}

	void MainBuild::Finish(std::shared_ptr<const StreamViews> a_streams)
	{
		DCLF_ZONE("CS.DCLF.BuildMain.Finish");
		streams = std::move(a_streams);
		TakeGeometryDraws(streams.get(), tables, in.tablesGeneration, frameNumber, body.geometryDraws);
		body.streams = streams;
		// The commit uploads the tables' own stream rows. The draws holding the payload keep the tables alive.
		TracyCZoneN(streamRowsZone, "CS.DCLF.BuildMain.StreamRows", true);
		AppendStreamRows(tables, in.addresses.streamBuffer, body.geometryDraws);
		if (in.addresses.streamBuffer)
			body.streamRows = tables.streamRows;
		TracyCZoneEnd(streamRowsZone);
		AssignAnimatedRows();
		body.materialRows = rows.material.View();
		body.pipelineRows = rows.pipeline.View();
		// Payload products are rebuilt from their last build for the material slots that moved since.
		TracyCZoneN(textureListZone, "CS.DCLF.BuildMain.TextureList", true);
		auto& dirty = rows.productDirty;
		if (!dirty.cursor.Continues(tables.materialLog, in.tablesGeneration)) {
			dirty.MarkEvery();
			dirty.cursor.Restart(in.tablesGeneration);
		} else {
			for (const std::uint32_t m : dirty.cursor.Unread(tables.materialLog))
				dirty.Mark(m);
		}
		dirty.cursor.Advance(tables.materialLog);
		Extensions::BuildPayloadProducts({ rows, tables, dirty.slots, dirty.every, nullptr }, rows.products, body.products);
		rows.products = body.products;
		dirty.Clear();
		TracyCZoneEnd(textureListZone);
		Mark(5);
		// The other segment shares the body with its own inputs. One-off counts stay the primary's.
		TracyCZoneN(zCopyZone, "CS.DCLF.BuildMain.ZCopy", true);
		for (std::size_t j = 0; j < segmentOut.size(); ++j) {
			if (j == primary || !segmentOut[j] || !segmentIn[j])
				continue;
			auto& other = *segmentOut[j];
			other.Reset();
			other.inputs = *segmentIn[j];
			other.body = heldBody;
			if (j != kAsyncZPrepass)
				other.frameRegisters = out.frameRegisters;
			other.cullViewProj = out.cullViewProj;
			other.culled = out.culled;
			other.resident = out.resident;
		}
		TracyCZoneEnd(zCopyZone);
	}

	void BuildMainPayloads(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
		const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams)
	{
		if (!a_in[kAsyncColour] && !a_in[kAsyncZPrepass])
			return;
		MainBuild build(a_in, a_tables, a_frame, a_lookups, a_out, a_rows, a_cache);
		build.Run();
		build.Finish(std::move(a_streams));
	}

	struct MainBuildStages::State
	{
		MainBuild build;
	};

	MainBuildStages::MainBuildStages(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
		const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache)
	{
		if (a_in[kAsyncColour] || a_in[kAsyncZPrepass])
			state.reset(new State{ MainBuild(a_in, a_tables, a_frame, a_lookups, a_out, a_rows, a_cache) });
	}

	MainBuildStages::~MainBuildStages() = default;

	void MainBuildStages::Run()
	{
		if (state)
			state->build.Run();
	}

	void MainBuildStages::Finish(std::shared_ptr<const StreamViews> a_streams)
	{
		if (state)
			state->build.Finish(std::move(a_streams));
	}

	std::string MainPairsReport(MainRows& a_rows)
	{
		auto& s = a_rows.pairStats;
		std::string text;
		if (s.builds || s.aheads) {
			const double builds = static_cast<double>(std::max<std::uint64_t>(s.builds, 1)), aheads = static_cast<double>(std::max<std::uint64_t>(s.aheads, 1));
			text = fmt::format("[DCLF] main pairs: {} builds; a build: {:.1f} pairs checked, {:.1f} resolved, {:.1f} entries rewritten, {:.1f} loop list slots; {} checked every pair, {} pipelines "
							   "moved; Pairs.Resolve {:.3f} ms a build (max {:.3f}), {} on the pool in {} chunks; bundle {:.3f} ms, {} made; inputs {:.3f} ms; resyncs {} ({} on "
							   "the pool); stages: {} aheads, {} overlapped, stream views {:.3f} ms, main {:.3f} ms, both {:.3f} ms, shadow {:.3f} ms, exclusions {:.3f} ms\n",
				s.builds, s.checked / builds, s.resolved / builds, s.entriesRewritten / builds, s.loopListed / builds, s.everyPair, s.pipelinesMoved, s.resolveMs / builds, s.resolveMaxMs,
				s.fannedOut, s.chunks, s.bundleMs / builds, s.bundles, s.inputsMs / builds, s.resyncs, s.resyncsFannedOut, s.aheads, s.overlapped, s.streamsMs / aheads,
				s.mainMs / aheads, s.stagesMs / aheads, s.shadowMs / aheads, s.exclusionsMs / aheads);
		}
		s = {};
		return text;
	}

	void BuildMainPayload(const MainInputs& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups, MainPayload& a_out,
		MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams)
	{
		const std::size_t j = a_in.depthOnly ? kAsyncZPrepass : kAsyncColour;
		std::array<const MainInputs*, 2> in{};
		std::array<MainPayload*, 2> out{};
		in[j] = &a_in;
		out[j] = &a_out;
		BuildMainPayloads(in, a_tables, a_frame, a_lookups, out, a_rows, a_cache, std::move(a_streams));
	}
}
