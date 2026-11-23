// Face snapshot hooks in the engine's morph job and stage join, and the head records the scene walk keeps.
// Writers and the walk share an open-addressed table keyed by head.

#include "Features/DrawcallLimitFix/Features/Faces/FaceSnapshots.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include <ORGModuleServices/Async/LeasedArraySlots.h>

#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace DCLF::Features::Faces
{
	namespace
	{
		// Power of two, far above the number of heads loaded at once.
		constexpr std::uint32_t kCapacity = 2048;
		// latest holds the slot index in bits 0-1, plus a flag for a snapshot the reader has not taken.
		constexpr std::uint32_t kSlotMask = 3;
		constexpr std::uint32_t kFresh = 4;
		// Probing continues past a tombstone, and an insert may reuse it.
		const RE::BSFaceGenNiNode* const kTombstone = reinterpret_cast<const RE::BSFaceGenNiNode*>(std::uintptr_t{ 1 });
		constexpr std::uint32_t kStatsInterval = 600;

		bool hooksInstalled = false;

		std::uint32_t HashOf(const void* a_pointer)
		{
			auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_pointer));
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdull;
			value ^= value >> 33;
			return static_cast<std::uint32_t>(value) & (kCapacity - 1);
		}
	}

	struct FaceSnapshots::Impl
	{
		struct Record
		{
			struct Shape
			{
				RE::NiPointer<RE::BSDynamicTriShape> shape;
				std::uint32_t offset = 0;  // in floats
				std::uint32_t vertexCount = 0;
			};

			RE::NiPointer<RE::BSFaceGenNiNode> head;
			std::uint64_t recordId = 0;
			std::vector<Shape> shapes;  // immutable once the record is in the table
			std::uint32_t floats = 0;   // per slot, four per vertex of every shape
			org::async::LeasedArraySlots<float, 3> storage;
			// The writer sets this before publishing the slot. The reader reads it after taking the slot.
			std::array<std::uint64_t, 3> slotGeneration{};
			std::atomic<std::uint32_t> latest{ 1 };
			std::uint32_t writeSlot = 0;  // one writer at a time, a job then the stage join
			std::uint32_t readSlot = 2;
			// Asks the stage join for a first snapshot. Cleared once any capture publishes.
			std::atomic<bool> wantsCapture{ true };
			// One writer of the triple buffer at a time: a morph job, the stage join or the walk's first capture. A writer that finds it
			// taken skips; the next capture catches up.
			std::atomic<bool> writing{ false };
			// Set when a capture sees a changed shape. The walk then rebuilds the record.
			std::atomic<bool> stale{ false };
			std::uint32_t seenWalk = 0;
			bool staleReported = false;
			std::uint64_t retiredAtStage = 0;

		};

		struct Entry
		{
			std::atomic<const RE::BSFaceGenNiNode*> key{ nullptr };
			std::atomic<Record*> record{ nullptr };
		};

		std::array<Entry, kCapacity> table;
		std::atomic<std::uint64_t> generation{ 0 };
		std::uint64_t nextRecordId = 1; // scene-walk thread only
		std::atomic<std::uint64_t> stagesCompleted{ 0 };
		std::atomic<std::uint32_t> jobCaptures{ 0 }, stageCaptures{ 0 }, mismatches{ 0 }, stages{ 0 };

		std::vector<Record*> live;
		std::vector<Record*> retired;
		std::uint32_t walk = 0;
		Stats stats;

		Entry* Find(const RE::BSFaceGenNiNode* a_head)
		{
			for (std::uint32_t i = 0, h = HashOf(a_head); i < kCapacity; ++i, h = (h + 1) & (kCapacity - 1)) {
				const auto* key = table[h].key.load(std::memory_order_acquire);
				if (!key)
					return nullptr;
				if (key == a_head)
					return &table[h];
			}
			return nullptr;
		}

		Record* Lookup(const RE::BSFaceGenNiNode* a_head)
		{
			auto* entry = Find(a_head);
			return entry ? entry->record.load(std::memory_order_acquire) : nullptr;
		}

		// The record goes in before the key, so a writer that finds the key also finds the record.
		bool Insert(const RE::BSFaceGenNiNode* a_head, Record* a_record)
		{
			Entry* free = nullptr;
			for (std::uint32_t i = 0, h = HashOf(a_head); i < kCapacity; ++i, h = (h + 1) & (kCapacity - 1)) {
				const auto* key = table[h].key.load(std::memory_order_relaxed);
				if (key == a_head) {
					table[h].record.store(a_record, std::memory_order_release);
					return true;
				}
				if (!key) {
					if (!free)
						free = &table[h];
					break;
				}
				if (key == kTombstone && !free)
					free = &table[h];
			}
			if (!free)
				return false;
			free->record.store(a_record, std::memory_order_release);
			free->key.store(a_head, std::memory_order_release);
			return true;
		}

		void Remove(const RE::BSFaceGenNiNode* a_head)
		{
			if (auto* entry = Find(a_head)) {
				entry->record.store(nullptr, std::memory_order_release);
				entry->key.store(kTombstone, std::memory_order_release);
			}
		}

		/**
		 * @brief Copies the head's shapes into the next slot. Called by the thread that just morphed the head, after the stage join, or
		 * by the walk for a head's first snapshot. The walk's copy (a_locked) runs outside the morph stage's order, so it reads each
		 * shape's dynamic data under the shape's own lock, as the engine's renderer does.
		 */
		bool Capture(Record& a_record, bool a_locked = false)
		{
			if (a_record.writing.exchange(true, std::memory_order_acquire))
				return false;
			const bool captured = CaptureOwned(a_record, a_locked);
			a_record.writing.store(false, std::memory_order_release);
			return captured;
		}

		bool CaptureOwned(Record& a_record, bool a_locked)
		{
			float* out = a_record.storage.PrepareWrite(a_record.writeSlot, a_record.floats).data();
			for (const auto& shape : a_record.shapes) {
				auto* geometry = shape.shape.get();
				auto& dynamic = geometry->GetDynamicTrishapeRuntimeData();
				const std::uint32_t count = geometry->GetTrishapeRuntimeData().vertexCount;
				std::optional<RE::BSSpinLockGuard> guard;
				if (a_locked)
					guard.emplace(dynamic.lock);
				if (!dynamic.dynamicData || count != shape.vertexCount || dynamic.dataSize < count * 16u) {
					mismatches.fetch_add(1, std::memory_order_relaxed);
					a_record.stale.store(true, std::memory_order_relaxed);
					return false;
				}
				std::memcpy(out + shape.offset, dynamic.dynamicData, std::size_t(count) * 16);
			}
			a_record.slotGeneration[a_record.writeSlot] = generation.fetch_add(1, std::memory_order_relaxed) + 1;
			const auto previous = a_record.latest.exchange(a_record.writeSlot | kFresh, std::memory_order_acq_rel);
			a_record.writeSlot = previous & kSlotMask;
			a_record.wantsCapture.store(false, std::memory_order_relaxed);
			return true;
		}

		// The head's direct dynamic-shape children, which are the ones its morph job writes.
		static void ShapesOf(RE::BSFaceGenNiNode& a_head, std::vector<Record::Shape>& a_out)
		{
			a_out.clear();
			std::uint32_t offset = 0;
			for (const auto& child : Engine::Children(a_head)) {
				auto* geometry = child ? child->AsGeometry() : nullptr;
				if (!geometry || geometry->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape)
					continue;
				auto* shape = static_cast<RE::BSDynamicTriShape*>(geometry);
				const std::uint32_t count = shape->GetTrishapeRuntimeData().vertexCount;
				a_out.push_back({ RE::NiPointer<RE::BSDynamicTriShape>(shape), offset, count });
				offset += count * 4;
			}
		}

		bool Matches(const Record& a_record, const std::vector<Record::Shape>& a_shapes) const
		{
			if (a_record.stale.load(std::memory_order_relaxed) || a_record.shapes.size() != a_shapes.size())
				return false;
			for (std::size_t i = 0; i < a_shapes.size(); ++i)
				if (a_record.shapes[i].shape != a_shapes[i].shape || a_record.shapes[i].vertexCount != a_shapes[i].vertexCount)
					return false;
			return true;
		}

		void Retire(Record* a_record)
		{
			Remove(a_record->head.get());
			a_record->retiredAtStage = stagesCompleted.load(std::memory_order_acquire);
			retired.push_back(a_record);
			++stats.retired;
		}

		// Freed only after a later morph stage completes. Any job that could have found the record has joined by then.
		void Free(Record* a_record)
		{
			released.emplace_back(std::move(a_record->head));
			for (auto& shape : a_record->shapes)
				released.emplace_back(std::move(shape.shape));
			delete a_record;
		}

		void FreeRetired()
		{
			const auto completed = stagesCompleted.load(std::memory_order_acquire);
			std::erase_if(retired, [&](Record* a_record) {
				if (a_record->retiredAtStage >= completed)
					return false;
				Free(a_record);
				++stats.freed;
				return true;
			});
		}

		std::vector<Record::Shape> scratch;
		std::vector<ShapeInput> inputs;
		std::vector<RE::NiPointer<RE::NiRefObject>> released;
	};

	FaceSnapshots::FaceSnapshots() :
		impl(std::make_unique<Impl>())
	{}

	FaceSnapshots::~FaceSnapshots() = default;

	FaceSnapshots& FaceSnapshots::Get()
	{
		static FaceSnapshots instance;
		return instance;
	}

	bool FaceSnapshots::Enabled()
	{
		return hooksInstalled;
	}

	/** @brief Runs on the job's thread right after it morphs a head (FUN_140432550). */
	struct FaceSnapshots::MorphHook
	{
		static void thunk(RE::BSFaceGenNiNode* a_head, std::uint8_t a_flag)
		{
			func(a_head, a_flag);
			if (!a_head)
				return;
			auto& impl = *Get().impl;
			if (auto* record = impl.Lookup(a_head); record && impl.Capture(*record))
				impl.jobCaptures.fetch_add(1, std::memory_order_relaxed);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Runs after the morph stage's JobList::Finish. No morph job runs until the next stage. */
	struct FaceSnapshots::StageJoinHook
	{
		static void thunk(void* a_jobList)
		{
			func(a_jobList);
			auto& impl = *Get().impl;
			for (auto& entry : impl.table) {
				auto* record = entry.record.load(std::memory_order_acquire);
				if (record && record->wantsCapture.load(std::memory_order_relaxed) && impl.Capture(*record))
					impl.stageCaptures.fetch_add(1, std::memory_order_relaxed);
			}
			impl.stages.fetch_add(1, std::memory_order_relaxed);
			impl.stagesCompleted.fetch_add(1, std::memory_order_release);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	namespace
	{
		// FUN_1404334e0 calls FUN_140432550 at +0x13. Job_Face_morphing calls JobList::Finish at +0x4D. Same offsets on SE.
		constexpr Engine::Offset kMorphCallSite{ { 26999, 0x13 }, { 26418, 0x13 } };
		constexpr Engine::Offset kJoinCallSite{ { 39096, 0x4d }, { 38139, 0x4d } };
	}

	void FaceSnapshots::Preflight()
	{
		HookSites::RequireCall("the face morphing's per-head morph call", REL::Offset(kMorphCallSite).address());
		HookSites::RequireCall("the face morphing's stage join", REL::Offset(kJoinCallSite).address());
	}

	void FaceSnapshots::Install()
	{
		if (hooksInstalled)
			return;
		// Chains onto whatever the calls hold. Preflight requires CALLs.
		const auto morphCall = REL::Offset(kMorphCallSite).address();
		const auto joinCall = REL::Offset(kJoinCallSite).address();
		stl::write_thunk_call<MorphHook>(morphCall);
		stl::write_thunk_call<StageJoinHook>(joinCall);
		hooksInstalled = true;
		logger::info("[DCLF] face snapshots installed on the face morphing stage");
	}

	void FaceSnapshots::BeginWalk(std::vector<const RE::BSFaceGenNiNode*>& a_published, std::vector<const RE::BSFaceGenNiNode*>& a_stale)
	{
		auto& d = *impl;
		++d.walk;
		d.FreeRetired();
		for (auto* record : d.live) {
			if (record->latest.load(std::memory_order_relaxed) & kFresh) {
				const auto previous = record->latest.exchange(record->readSlot, std::memory_order_acq_rel);
				record->readSlot = previous & kSlotMask;
				++d.stats.acquired;
				a_published.push_back(record->head.get());
			}
			if (!record->staleReported && record->stale.load(std::memory_order_relaxed)) {
				record->staleReported = true;
				a_stale.push_back(record->head.get());
			}
		}
		d.stats.heads = static_cast<std::uint32_t>(d.live.size());
	}

	FaceSnapshots::ShapeView FaceSnapshots::View(const RE::BSGeometry* a_shape, const RE::BSFaceGenNiNode* a_head)
	{
		auto& d = *impl;
		auto* record = d.Lookup(a_head);
		if (!record)
			return {};
		const auto generation = record->slotGeneration[record->readSlot];
		if (generation == 0)
			return {};
		for (const auto& shape : record->shapes) {
			if (shape.shape.get() != a_shape)
				continue;
			auto owner = record->storage.Acquire(record->readSlot);
			const float* positions = owner->data() + shape.offset;
			return { positions, shape.vertexCount, generation, std::move(owner) };
		}
		return {};
	}

	void FaceSnapshots::Release(const RE::BSFaceGenNiNode* a_head)
	{
		auto& d = *impl;
		if (auto* record = d.Lookup(a_head)) {
			std::erase(d.live, record);
			d.Retire(record);
		}
	}

	FaceSnapshots::ShapeView FaceSnapshots::Shape(const RE::BSGeometry* a_shape, RE::BSFaceGenNiNode* a_head, const std::function<void(std::vector<ShapeInput>&)>& a_shapes)
	{
		auto& d = *impl;
		if (!a_head)
			return {};
		auto* record = d.Lookup(a_head);
		if (!record || record->seenWalk != d.walk) {
			// On the head's first shape this walk, check the record against the head's current shapes.
			d.inputs.clear();
			a_shapes(d.inputs);
			d.scratch.clear();
			std::uint32_t offset = 0;
			for (const auto& input : d.inputs) {
				d.scratch.push_back({ RE::NiPointer<RE::BSDynamicTriShape>(input.shape), offset, input.vertexCount });
				offset += input.vertexCount * 4;
			}
			d.inputs.clear();
			if (!record || !d.Matches(*record, d.scratch)) {
				if (d.scratch.empty()) {
					if (record) {
						std::erase(d.live, record);
						d.Retire(record);
					}
					return {};
				}
				auto* rebuilt = new Impl::Record();
				rebuilt->recordId = d.nextRecordId++;
				rebuilt->head.reset(a_head);
				rebuilt->shapes = d.scratch;
				for (const auto& shape : rebuilt->shapes)
					rebuilt->floats += shape.vertexCount * 4;
				// Allocate the triple buffer here, not in the engine's morph job. A writer reallocates only if an old payload pins its slot.
				for (std::size_t slot = 0; slot < 3; ++slot)
					(void)rebuilt->storage.PrepareWrite(slot, rebuilt->floats);
				if (record) {
					std::erase(d.live, record);
					d.Retire(record);
					++d.stats.rebuilt;
				}
				if (!d.Insert(a_head, rebuilt)) {
					d.Free(rebuilt);
					// Hand the scratch copies to the render thread too, so they are never the last release here.
					for (auto& shape : d.scratch)
						d.released.emplace_back(std::move(shape.shape));
					d.scratch.clear();
					return {};
				}
				d.live.push_back(rebuilt);
				record = rebuilt;
			}
			d.scratch.clear();  // the record now holds these references
			record->seenWalk = d.walk;
			// A head with no snapshot yet takes its first one here, rather than waiting for the morph stage to reach it: the stage
			// does not run while a load's hold pauses the game, and afterwards only for some heads a frame.
			if (record->slotGeneration[record->readSlot] == 0 && record->wantsCapture.load(std::memory_order_relaxed) && d.Capture(*record, true)) {
				++d.stats.walkCaptured;
				if (record->latest.load(std::memory_order_relaxed) & kFresh) {
					const auto previous = record->latest.exchange(record->readSlot, std::memory_order_acq_rel);
					record->readSlot = previous & kSlotMask;
					++d.stats.acquired;
				}
			}
			if (record->slotGeneration[record->readSlot] == 0)
				++d.stats.withoutSnapshot;
		}
		const auto generation = record->slotGeneration[record->readSlot];
		if (generation == 0)
			return {};
		for (const auto& shape : record->shapes) {
			if (shape.shape.get() != a_shape)
				continue;
			auto owner = record->storage.Acquire(record->readSlot);
			const float* positions = owner->data() + shape.offset;
			return { positions, shape.vertexCount, generation, std::move(owner) };
		}
		return {};
	}

	void FaceSnapshots::TakeReleased(std::vector<RE::NiRefObject*>& a_out)
	{
		auto& d = *impl;
		for (auto& reference : d.released)
			if (auto* object = reference.get()) {
				object->IncRefCount();  // the caller now owns one count. The reference drop is never the last.
				reference.reset();
				a_out.push_back(object);
			}
		d.released.clear();
	}

	FaceSnapshots::HeadView FaceSnapshots::HeadSnapshot(const RE::BSFaceGenNiNode& a_head)
	{
		auto& d = *impl;
		const auto* record = d.Lookup(&a_head);
		if (!record || !record->recordId)
			return {};
		const auto generation = record->slotGeneration[record->readSlot];
		if (!generation)
			return {};
		auto owner = record->storage.Acquire(record->readSlot);
		if (!owner || owner->size() != record->floats)
			return {};
		HeadView captured;
		captured.recordId = record->recordId;
		captured.generation = generation;
		captured.owner = std::move(owner);
		captured.shapes.reserve(record->shapes.size());
		for (std::uint32_t i = 0; i < record->shapes.size(); ++i) {
			const auto& shape = record->shapes[i];
			captured.shapes.push_back({ i, shape.offset, shape.vertexCount });
		}
		return captured;
	}

	void FaceSnapshots::EndWalk()
	{
		auto& d = *impl;
		if (d.walk % kStatsInterval == 0 && (!d.live.empty() || d.stats.retired)) {
			const auto stats = TakeStats();
			const auto writer = TakeWriterStats();
			logger::info("[DCLF] face snapshots: {} heads, {} taken fresh, {} seen without one, {} rebuilt, {} retired, {} freed; "
			             "captured by jobs {}, at the join {}, by the walk {}, {} mismatches, over {} stages",
				stats.heads, stats.acquired, stats.withoutSnapshot, stats.rebuilt, stats.retired, stats.freed, writer.jobCaptures, writer.stageCaptures,
				stats.walkCaptured, writer.mismatches, writer.stages);
		}
	}

	FaceSnapshots::Stats FaceSnapshots::TakeStats()
	{
		auto& d = *impl;
		auto stats = d.stats;
		stats.heads = static_cast<std::uint32_t>(d.live.size());
		d.stats = {};
		return stats;
	}

	FaceSnapshots::WriterStats FaceSnapshots::TakeWriterStats()
	{
		auto& d = *impl;
		return { d.jobCaptures.exchange(0), d.stageCaptures.exchange(0), d.mismatches.exchange(0), d.stages.exchange(0) };
	}
}
