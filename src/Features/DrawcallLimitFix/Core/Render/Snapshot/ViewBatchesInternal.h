#pragma once

// Internals shared by the full view batch build and the incremental scene lists. Used only on the builder thread.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "RenderGraph/BufferRefs.h"

#include <ORGModuleServices/Async/InboxQueue.h>
#include <ORGModuleServices/VersionedBufferBuild.h>
#include <Resources/Buffers/Buffer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <ankerl/unordered_dense.h>
#include <bit>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <memory>
#include <vector>

namespace DCLF::Draws::detail
{
	using Direct::ViewType;
	constexpr std::size_t View(ViewType a_view) { return static_cast<std::size_t>(a_view); }
	constexpr std::uint32_t kNoKey = ~0u;
	constexpr std::uint32_t kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT

	/** @brief One lane before its batch is laid out. It sorts by k0, then k1, then object. */
	struct Lane
	{
		std::uint32_t k0 = 0, k1 = 0;
		std::uint32_t object = 0, sequence = 0;
		std::uint16_t sequenceCount = 0;
		std::uint32_t bucketKey = 0, pipelineSlot = ~0u;
		std::uint32_t flags = 0, fadeRoot = 0;
		float fadeDistance = 0.0f;
		std::uint32_t partitions = 0, lodPartitions = 0;
		std::uint32_t decalGroup = 0;  // 1 to 3, decal list only
	};
	/** @brief A run of resident sequences, made the first time an input appears in a view class. */
	struct Run
	{
		std::uint32_t first = kNoKey;
		std::uint16_t count = 0;
	};
	/** @brief An input's runs, one per view class. */
	struct InputRuns
	{
		Run main, caster;
		std::array<Run, kOcclusionViews> occlusion{};
	};

	/** @brief Builder scratch, kept warm across sets. */
	struct Scratch
	{
		std::array<std::vector<Lane>, Direct::kViewTypes> lanes;
		std::vector<std::uint32_t> moverCount;
		std::size_t lastSequences = 0;  // used to reserve the next set
	};
	inline Scratch& BuilderScratch()
	{
		static Scratch scratch;
		return scratch;
	}

	/**
	 * @brief Recycled batch storage. Reusing blocks avoids a page fault on every page of a fresh block at its first fill.
	 * Blocks can come back on any thread. Never destroyed.
	 */
	class StoragePool
	{
	public:
		static StoragePool& Get()
		{
			static auto* pool = new StoragePool;
			return *pool;
		}
		/** @brief Builder thread. A block of at least a_bytes, not zeroed. */
		std::shared_ptr<std::byte[]> Take(std::size_t a_bytes)
		{
			Block block;
			while (free.TryPop(block)) {
				kept.fetch_sub(1, std::memory_order_relaxed);
				if (block.size >= a_bytes)
					return Wrap(block);
				delete[] block.bytes;
			}
			const std::size_t size = a_bytes + a_bytes / 4;  // room for growth
			return Wrap({ new std::byte[size], size });
		}

	private:
		struct Block
		{
			std::byte* bytes = nullptr;
			std::size_t size = 0;
		};
		// Frames in flight, plus the installed set and the one being made.
		static constexpr int kKept = 6;
		std::shared_ptr<std::byte[]> Wrap(Block a_block)
		{
			return std::shared_ptr<std::byte[]>(a_block.bytes, [this, size = a_block.size](std::byte* a_bytes) { Give({ a_bytes, size }); });
		}
		void Give(Block a_block)
		{
			if (kept.fetch_add(1, std::memory_order_relaxed) < kKept) {
				free.Push(a_block);
				return;
			}
			kept.fetch_sub(1, std::memory_order_relaxed);
			delete[] a_block.bytes;
		}
		org::async::InboxQueue<Block> free;
		std::atomic<int> kept{ 0 };
	};

	/** @brief Collects one set's resident sequences and the recorder's draw arguments. */
	struct SequenceSink
	{
		std::vector<DrawSequence>& sequences;
		std::vector<Direct::DrawArgs>& args;
		const GeometryDrawsOut& geometries;
		// Index of the first sequence, after those already in SceneLists' persistent store.
		std::uint32_t base = 0;
		// When set, every geometry slot a walk reads is appended, for SceneLists' reverse map.
		std::vector<std::uint32_t>* walked = nullptr;

		const GeometryDraw* At(std::uint32_t a_index) const
		{
			const std::size_t slots = geometries.SlotCount();
			if (a_index < slots)
				return geometries.slots.At(a_index);
			return a_index - slots < geometries.streamRows.size() ? &geometries.streamRows[a_index - slots] : nullptr;
		}
		/** @brief Emit one plain pulled draw's sequence. The pool's first index is left out because the recorder adds it. */
		void Emit(const DrawSequence& a_rows, std::uint32_t a_slot, const GeometryDraw& a_geometry, const GeometryDraw* a_stream)
		{
			DrawSequence s = a_rows;
			const GeometryDraw& second = a_stream ? *a_stream : a_geometry;
			s.vertexBufferRef = a_geometry.vertexBufferRef;
			s.vertexBufferSize = a_geometry.vertexBufferSize;
			s.vertexStride = a_geometry.vertexStride;
			s.streamBufferRef = second.vertexBufferRef;
			s.streamBufferSize = second.vertexBufferSize;
			s.streamStride = second.vertexStride;
			s.blockPad = 0;
			SetSequenceBlocks(s);
			s.indexBufferAddress = a_geometry.indexBufferAddress;
			s.indexBufferSize = a_geometry.indexBufferSize;
			s.indexFormat = kIndexFormatR16;
			s.indexCount = a_geometry.indexCount;
			s.instanceCount = 1;
			s.firstIndex = a_geometry.firstIndex;
			s.vertexOffset = 0;
			s.firstInstance = 0;
			sequences.push_back(s);
			args.push_back({ a_geometry.indexCount, a_geometry.firstIndex, a_slot });
		}
		/**
		 * @brief Emit a sequence for every partition any LOD level may draw. Must walk the same way as BuildDrawsCS.
		 * A LOD skin's level masks are ORed together.
		 */
		Run EmitPartitions(const DrawSequence& a_rows, const DrawInput& a_input)
		{
			const std::uint32_t lod = a_input.lodPartitions;
			const std::uint32_t mask = lod ? ((lod | (lod >> 8) | (lod >> 16) | (lod >> 24)) & 0xFFu) : a_input.partitions;
			Run run{ base + static_cast<std::uint32_t>(sequences.size()), 0 };
			if (mask == kNoPartitions)
				return run;
			const GeometryDraw* stream = a_input.streamIndex != ~0u ? At(a_input.streamIndex) : nullptr;
			const bool chain = (mask & kPartitionChain) != 0;
			const std::uint32_t links = chain ? (mask & kPartitionChainCount) : kMaxSkinPartitions;
			std::uint32_t slot = a_input.geometryIndex;
			for (std::uint32_t partition = 0; partition < links && slot != kNoPartition; ++partition) {
				const GeometryDraw* geometry = At(slot);
				if (walked)
					walked->push_back(slot);
				if (!geometry)
					break;
				if (chain || mask == 0 || ((mask >> partition) & 1u)) {
					Emit(a_rows, slot, *geometry, stream);
					++run.count;
				}
				slot = geometry->nextPartition;
			}
			return run;
		}
		/** @brief Emit a decal's drawn partitions in slot order. Must match BuildDrawsCS's decal walk. */
		Run EmitDecal(const DrawSequence& a_rows, const DrawInput& a_input)
		{
			const std::uint32_t p = a_input.partitions;
			const bool chain = (p & kPartitionChain) != 0;
			const std::uint32_t span = chain ? (p & kPartitionChainCount) :
			                           (p == 0 || p == kNoPartitions) ? 1u :
			                                                            static_cast<std::uint32_t>(std::bit_width(p & (kNoPartitions - 1)));
			const GeometryDraw* stream = a_input.streamIndex != ~0u ? At(a_input.streamIndex) : nullptr;
			Run run{ base + static_cast<std::uint32_t>(sequences.size()), 0 };
			std::uint32_t slot = a_input.geometryIndex;
			for (std::uint32_t k = 0; k < span && slot != kNoPartition; ++k) {
				const GeometryDraw* geometry = At(slot);
				if (!geometry)
					break;
				if (chain || p == 0 || ((p >> k) & 1u)) {
					Emit(a_rows, slot, *geometry, stream);
					++run.count;
				}
				slot = geometry->nextPartition;
			}
			return run;
		}
	};

	/**
	 * @brief Row addresses for a set's sequences. They are offsets into the payload's row parts, and the bases are pushed per list.
	 * The derived blocks are offsets too, which is fine because only the GPU path reads them.
	 */
	struct RowBases
	{
		std::uint64_t pipelineRows = 0, materialRows = 0, shadowRows = 0;
		RenderGraph::BufferRef pipelineRowsRef, materialRowsRef, shadowRowsRef;  // the direct draws' bases
		const std::vector<ShadowPipelineKey>* slotKeys = nullptr;

		static std::uint64_t AddressOf(const ScenePayload& a_payload, PayloadPart a_part)
		{
			const auto& version = a_payload.versions[static_cast<std::size_t>(a_part)];
			return version ? version->lease->address : 0;
		}
		/** @brief Not a raw ref, because the payload's readers declare its parts. */
		static RenderGraph::BufferRef RefOf(const ScenePayload& a_payload, PayloadPart a_part)
		{
			const auto& version = a_payload.versions[static_cast<std::size_t>(a_part)];
			auto* host = RenderGraphRuntime::Get().Host();
			return host && version && version->lease->buffer ?
			           RenderGraph::MakeBufferRef(host->Device(), version->lease->buffer->GetAPIResource().GetHandle(), 0, std::source_location::current()) :
			           RenderGraph::BufferRef{};
		}
		/** @brief A main input's rows. The draw later resolves an animated row to the frame's copy. */
		DrawSequence Main(const DrawInput& a_input) const
		{
			DrawSequence s{};
			s.pipelineRowAddress = std::uint64_t(a_input.recordIndex >> kRowPipelineShift) * kPipelineRowBytes;
			s.materialRowAddress = std::uint64_t(a_input.recordIndex & kRowMaterialMask) * kMaterialRowBytes;
			s.objectIndex = a_input.objectIndex & kObjectIndexMask;
			return s;
		}
		/** @brief A shadow input's rows. The slot's vertex layout goes in the pipeline-row words and its technique in the pipeline word. */
		DrawSequence Shadow(const DrawInput& a_input, std::uint32_t a_key) const
		{
			DrawSequence s{};
			if (slotKeys && a_key < slotKeys->size()) {
				const auto& key = (*slotKeys)[a_key];
				s.pipelineIndex = key.technique | kShadowSlotTechniqueKnown;
				s.pipelineRowAddress = key.vertexLayout;
			}
			s.materialRowAddress = std::uint64_t(a_input.shadowRow & kRowMaterialMask) * kConstantAlignment;
			s.objectIndex = a_input.objectIndex & kObjectIndexMask;
			return s;
		}
	};

	/** @brief Fill a set's animated material rows from the main payload's row headers. */
	inline void FillAnimatedRows(Direct::ViewBatchSet& a_set, const ViewBatchInputs& a_in)
	{
		a_set.animatedRowOf.clear();
		if (!a_in.main || !a_in.main->body)
			return;
		const auto& rows = a_in.main->body->materialRows;
		a_set.animatedRowOf.assign(rows.Count(), ~0u);
		for (std::size_t m = 0; m < rows.Count(); ++m)
			if (const auto* row = rows.At(m); row && row->bytes.size() >= kMaterialRowHeader + sizeof(MaterialRowHeader))
				a_set.animatedRowOf[m] = reinterpret_cast<const MaterialRowHeader*>(row->bytes.data() + kMaterialRowHeader)->animatedRow;
	}

	/** @brief The lane flags an input's object flags carry. */
	inline std::uint32_t LaneFlagsOf(const DrawInput& a_input)
	{
		const std::uint32_t f = a_input.flags;
		std::uint32_t out = 0;
		out |= (f & kObjectFadeTest) ? Direct::kLaneFadeTest : 0u;
		out |= (f & kObjectHeightTest) ? Direct::kLaneHeightTest : 0u;
		out |= (f & kObjectSunTest) ? Direct::kLaneSunTest : 0u;
		out |= (f & kObjectForward) ? Direct::kLaneForward : 0u;
		out |= ObjectDecalGroup(f) ? Direct::kLaneDecal : 0u;
		out |= (f & kObjectVolumetricOnly) ? Direct::kLaneVolumetricOnly : 0u;
		out |= (f & kObjectLandscapeLights) ? Direct::kLaneLandscapeLights : 0u;
		out |= ObjectFocusOnly(f) ? Direct::kLaneFocusOnly : 0u;
		const std::uint32_t terrain = ObjectTerrainGroup(f);
		out |= terrain == 1 ? Direct::kLaneTerrainGroup1 : terrain == 2 ? Direct::kLaneTerrainGroup2 : 0u;
		return out;
	}

	/** @brief Visit every input of the scene list in order, the resident region first and then the frame inputs. */
	template <class F>
	void ForEachInput(const ViewBatchInputs& a_in, F&& a_visit)
	{
		const auto visit = [&](const KeptView<DrawInput>& a_view) {
			for (std::size_t i = 0, n = a_view.Count(); i < n; ++i)
				a_visit(*a_view.At(i));
		};
		if (a_in.main) {
			visit(a_in.main->resident);
			for (const auto& input : a_in.main->body->inputList)
				a_visit(input);
		} else if (a_in.shadow) {
			visit(a_in.shadow->regionInputs);
		}
		if (a_in.shadow)
			for (const auto& input : a_in.shadow->inputList)
				a_visit(input);
	}

	/** @brief Builds the lists' lanes from the scene list. Each input's sequences are emitted once per view class. */
	class ListBuilder
	{
	public:
		ListBuilder(const ViewBatchInputs& a_in, const RowBases& a_rows, SequenceSink& a_sink, Scratch& a_scratch) :
			rows(a_rows), sinkTarget(&a_sink), lanes(&a_scratch.lanes), forwardOnly(a_in.forwardOnly)
		{
			if (a_in.catalog)
				zGroups = GetIndirectState(*a_in.catalog).zGroups;
			reflectionBit = Extensions::ViewEntryOf(Api::EngineView::Reflection).viewBit;
			firstPersonBit = Extensions::ViewEntryOf(Api::EngineView::FirstPerson).viewBit;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				const std::uint32_t bit = Extensions::OcclusionMapOf(v).viewBit;
				// The bit values come from Features/OcclusionMaps and must match GpuLayouts.h.
				occlusionBits[v] = bit;
				occlusionView[v] = bit == (1u << 5) ? ViewType::SkyOccluder : bit == (1u << 6) ? ViewType::PrecipOccluder : ViewType::kCount;
			}
		}

		/** @brief Redirect where Add pushes lanes and emits sequences. */
		void Target(std::array<std::vector<Lane>, Direct::kViewTypes>& a_lanes, SequenceSink& a_sink)
		{
			lanes = &a_lanes;
			sinkTarget = &a_sink;
		}
		/** @brief The Z groups the main list is ordered by. */
		const std::vector<std::uint32_t>& ZGroups() const { return zGroups; }

		/** @brief Add a_input's lanes and return its runs. With a_reuse, its sequences already exist and none are emitted. */
		InputRuns Add(const DrawInput& a_input, const InputRuns* a_reuse = nullptr)
		{
			InputRuns out = a_reuse ? *a_reuse : InputRuns{};
			Run& mainRun = out.main;
			Run& casterRun = out.caster;
			auto& occlusionRuns = out.occlusion;
			auto& sink = *sinkTarget;
			const std::uint32_t mask = a_input.view.mask;
			const bool drawable = (a_input.flags & kInputDrawable) != 0;
			const std::uint32_t decalGroup = ObjectDecalGroup(a_input.flags);
			const bool forward = (a_input.flags & kObjectForward) != 0;
			const auto mainRuns = [&]() -> const Run& {
				if (mainRun.first == kNoKey)
					mainRun = decalGroup ? sink.EmitDecal(rows.Main(a_input), a_input) : sink.EmitPartitions(rows.Main(a_input), a_input);
				return mainRun;
			};
			if (forwardOnly) {
				if (drawable && forward && (mask & kViewForward) && !(mask & kViewRefraction))
					AddMain(ViewType::Forward, a_input, mainRuns(), 0, a_input.pipelineIndex);
				if (drawable && (mask & kViewRefraction))
					AddMain(ViewType::Refraction, a_input, mainRuns(), 0, 0).bucketKey = 0;
				if (drawable && forward && firstPersonBit && (mask & firstPersonBit) && !(mask & kViewRefraction))
					AddMain(ViewType::FirstPerson, a_input, mainRuns(), 0, a_input.pipelineIndex);
				return out;
			}
			if (!decalGroup && !forward && (mask & (kViewMain | kViewMainCull))) {
				if (drawable && (mask & kViewMain))
					AddMain(ViewType::Main, a_input, mainRuns(), ZGroupOf(a_input.pipelineIndex), a_input.pipelineIndex);
				else if (mask & kViewMainCull)
					AddCullOnly(a_input);
			}
			if (drawable && decalGroup && (mask & kViewDecal))
				AddMain(ViewType::Decal, a_input, mainRuns(), decalGroup, a_input.decalOrdinal).decalGroup = decalGroup;
			if (drawable && forward && (mask & kViewForward) && !(mask & kViewRefraction))
				AddMain(ViewType::Forward, a_input, mainRuns(), 0, a_input.pipelineIndex);
			if (drawable && (mask & kViewRefraction))
				AddMain(ViewType::Refraction, a_input, mainRuns(), 0, 0).bucketKey = 0;
			if (drawable && reflectionBit && (mask & reflectionBit))
				AddMain(ViewType::Reflection, a_input, mainRuns(), 0, a_input.pipelineIndex);
			if (drawable && firstPersonBit && (mask & firstPersonBit))
				AddMain(ViewType::FirstPerson, a_input, mainRuns(), 0, a_input.pipelineIndex);
			// A deferred range's member (water): its key's main set, whose range pipelines the water sites bind.
			if (drawable && (mask & kViewWater))
				AddMain(ViewType::Water, a_input, mainRuns(), 0, a_input.pipelineIndex);
			const std::uint32_t caster = a_input.view.shadowKey;
			const auto casters = [&]() -> const Run& {
				if (casterRun.first == kNoKey)
					casterRun = sink.EmitPartitions(rows.Shadow(a_input, caster), a_input);
				return casterRun;
			};
			if (caster != kNoKey) {
				if (mask & kViewSunCaster)
					AddShadow(ViewType::Sun, a_input, casters(), caster);
				if (mask & kViewSpotCaster)
					AddShadow(ViewType::Spot, a_input, casters(), caster);
				if (mask & kViewPointCaster)
					AddShadow(ViewType::Point, a_input, casters(), caster);
			}
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				const std::uint32_t key = a_input.view.occlusionKey[v];
				if (key == kNoKey || !occlusionBits[v] || !(mask & occlusionBits[v]) || occlusionView[v] == ViewType::kCount)
					continue;
				if (occlusionRuns[v].first == kNoKey)
					occlusionRuns[v] = sink.EmitPartitions(rows.Shadow(a_input, key), a_input);
				AddShadow(occlusionView[v], a_input, occlusionRuns[v], key);
			}
			return out;
		}

	private:
		std::uint32_t ZGroupOf(std::uint32_t a_set) const { return a_set < zGroups.size() ? zGroups[a_set] : 0xFFFFFu; }

		Lane& Push(ViewType a_view, const DrawInput& a_input, const Run& a_run)
		{
			auto& lane = (*lanes)[View(a_view)].emplace_back();
			lane.object = a_input.objectIndex & kObjectIndexMask;
			lane.sequence = a_run.first == kNoKey ? 0u : a_run.first;
			lane.sequenceCount = a_run.count;
			lane.flags = LaneFlagsOf(a_input);
			lane.fadeRoot = a_input.fadeRoot == ~0u ? 0u : a_input.fadeRoot + 1;
			lane.fadeDistance = a_input.fadeDistance;
			lane.partitions = a_input.partitions;
			lane.lodPartitions = a_input.lodPartitions;
			return lane;
		}
		Lane& AddMain(ViewType a_view, const DrawInput& a_input, const Run& a_run, std::uint32_t a_k0, std::uint32_t a_k1)
		{
			auto& lane = Push(a_view, a_input, a_run);
			lane.k0 = a_k0;
			lane.k1 = a_k1;
			lane.bucketKey = a_input.pipelineIndex;
			lane.pipelineSlot = a_input.recordIndex >> kRowPipelineShift;
			return lane;
		}
		// A cull-only candidate has no sequences and sorts after every drawn lane.
		void AddCullOnly(const DrawInput& a_input)
		{
			auto& lane = Push(ViewType::Main, a_input, Run{});
			lane.k0 = 1u << 31;
			lane.flags |= Direct::kLaneCullOnly;
			lane.bucketKey = kNoKey;
		}
		// Shadow lanes bucket by key slot. The view's map row picks the pipeline for its mode and state.
		void AddShadow(ViewType a_view, const DrawInput& a_input, const Run& a_run, std::uint32_t a_key)
		{
			auto& lane = Push(a_view, a_input, a_run);
			lane.k1 = a_key;
			lane.bucketKey = a_key;
		}

		const RowBases& rows;
		SequenceSink* sinkTarget;
		std::array<std::vector<Lane>, Direct::kViewTypes>* lanes;
		bool forwardOnly = false;
		std::vector<std::uint32_t> zGroups;
		std::uint32_t reflectionBit = 0, firstPersonBit = 0;
		std::array<std::uint32_t, kOcclusionViews> occlusionBits{};
		std::array<ViewType, kOcclusionViews> occlusionView{};
	};

	/** @brief The bytes a lane takes in its batch. Must match LayOut's columns. */
	constexpr std::size_t kLaneWords = 20;  // object, sequence, pipelineBucket, flags, fadeRoot, fadeDistance, partitions, lodPartitions, 4 spheres, 8 live
	constexpr std::size_t kLaneHalves = 1;  // sequenceCount
	constexpr std::size_t kLaneBytes = kLaneWords * 4 + kLaneHalves * 2;

	/** @brief Lay out a_batch's columns at a_base. */
	inline void LayOut(Direct::ViewBatch& a_batch, std::byte* a_base, std::uint32_t a_lanes)
	{
		auto words = reinterpret_cast<std::uint32_t*>(a_base);
		const auto column = [&]() {
			auto* at = words;
			words += a_lanes;
			return at;
		};
		const auto floats = [&]() { return reinterpret_cast<float*>(column()); };
		a_batch.object = column();
		a_batch.sequence = column();
		a_batch.pipelineBucket = column();
		a_batch.flags = column();
		a_batch.fadeRoot = column();
		a_batch.fadeDistance = floats();
		a_batch.partitions = column();
		a_batch.lodPartitions = column();
		a_batch.cx = floats();
		a_batch.cy = floats();
		a_batch.cz = floats();
		a_batch.r = floats();
		for (auto& live : a_batch.liveSpheres) {
			live.cx = floats();
			live.cy = floats();
			live.cz = floats();
			live.r = floats();
		}
		auto halves = reinterpret_cast<std::uint16_t*>(words);
		a_batch.sequenceCount = halves;
	}

	/** @brief The snapshot sphere of a_object, if known. It is known only for a non-moving object whose row matches its geometry. */
	inline bool SnapshotSphere(const SceneTables& a_tables, const Direct::SnapshotSpheres* a_spheres, std::uint32_t a_object, std::array<float, 4>& a_out)
	{
		if (!a_spheres || a_object >= a_spheres->sphere.size() || a_object >= a_tables.objectGeometry.size())
			return false;
		const void* owner = a_spheres->owner[a_object];
		if (!owner || owner != a_tables.objectGeometry[a_object] || a_spheres->moving[a_object])
			return false;
		a_out = a_spheres->sphere[a_object];
		return true;
	}

	/** @brief Sort a list's lanes and give them dense buckets. With a_extend, new keys are appended after a base's buckets. */
	inline void SortList(std::vector<Lane>& a_lanes, Direct::ViewBuckets& a_buckets, ankerl::unordered_dense::map<std::uint32_t, std::uint32_t>& a_bucketOf,
		bool a_extend = false)
	{
		std::sort(a_lanes.begin(), a_lanes.end(),
			[](const Lane& a, const Lane& b) { return a.k0 != b.k0 ? a.k0 < b.k0 : a.k1 != b.k1 ? a.k1 < b.k1 : a.object < b.object; });
		if (!a_extend)
			a_bucketOf.clear();
		// Lanes of one bucket key are mostly adjacent after the sort, so cache the last key's bucket.
		std::uint32_t lastKey = kNoKey, lastBucket = 0;
		for (auto& lane : a_lanes) {
			if (lane.bucketKey == kNoKey) {
				lane.bucketKey = 0;  // cull-only, so its bucket is never read
				continue;
			}
			if (lane.bucketKey != lastKey) {
				const auto [it, fresh] = a_bucketOf.try_emplace(lane.bucketKey, static_cast<std::uint32_t>(a_buckets.pipeline.size()));
				if (fresh) {
					a_buckets.pipeline.push_back(lane.bucketKey);
					a_buckets.pipelineSlot.push_back(lane.pipelineSlot);
				}
				lastKey = lane.bucketKey;
				lastBucket = it->second;
			}
			lane.bucketKey = lastBucket;
		}
	}

	/**
	 * @brief A set's sequence upload. It writes [first, total) on top of previous, or the whole buffer when previous is null.
	 * changedSince reports other ranges a backing needs, or null for none.
	 */
	struct SequencesUpload
	{
		std::shared_ptr<const org::services::PublishedBufferVersion> previous;
		std::uint64_t first = 0, total = 0;
		std::function<void(const std::function<void(const void*, std::size_t, std::uint64_t)>&)> emit;
		std::function<std::optional<std::vector<org::services::BufferByteRange>>(std::uint64_t)> changedSince;
		std::shared_ptr<const void> keep;
	};

	/**
	 * @brief Incremental scene lists. Each set is a shared sorted base, minus tombstoned lanes, plus a delta and a volatile part
	 * rebuilt every set. A compaction rebuilds the base when the delta or dead lanes grow. See dclf-architecture.md. Builder thread only.
	 */
	class SceneLists
	{
	public:
		SceneLists();
		~SceneLists();
		/** @brief Make a_in's set from the last set and the deltas, or by compaction. Null means the caller must make a full set. */
		std::shared_ptr<Direct::ViewBatchSet> Make(const ViewBatchInputs& a_in, const std::shared_ptr<const Direct::SnapshotSpheres>& a_spheres, std::uint64_t a_write,
			SequencesUpload& a_upload);
		/** @brief Set a_write's version is GPU ready. A null version means it failed and the next set compacts. */
		void Posted(std::uint64_t a_write, std::shared_ptr<const org::services::PublishedBufferVersion> a_version);
		/** @brief Forget everything, so the next set compacts. */
		void Reset();
		/** @brief The report line, or empty. Resets the counters. */
		std::string TakeReport();

		struct State;
		struct Compaction;  // runs on the pool

	private:
		std::unique_ptr<State> state;
		std::shared_ptr<Compaction> compaction;
	};
}
