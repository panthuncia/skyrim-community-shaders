#pragma once

// Host sides of the event lists the depth epoch's GPU work appends to, read back without a stall.
// The render thread arms and harvests, a task on DCLF's executor decodes, and the main thread writes to the engine at Main::Update's entry.
// Features include this header directly, so it must stay self-contained.

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/RunRanges.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DCLF
{
	struct EntrySpace;
	struct SceneTables;
}

namespace DCLF::Draws
{
	struct SceneBuffers;

	/** @brief An event list the depth epoch appends to, read back without a stall. The header holds the count, then the armed frame.
	 * The depth commit harvests its range's last run, whose lease has retired, then arms it again. DCLF never waits. */
	struct HostEventRing
	{
		/** @brief The host buffers for one list capacity, one per range. Append-only and kept until the ring goes, since a kept recording may name any of them. */
		struct StagingSet
		{
			std::uint32_t events = 0;
			std::vector<std::shared_ptr<org::Buffer>> buffers;  // by range
			StagingSet* next = nullptr;
		};
		explicit HostEventRing(std::uint64_t (*a_bytesFor)(std::uint32_t)) :
			bytesFor(a_bytesFor) {}
		~HostEventRing()
		{
			for (StagingSet* set = sets.exchange(nullptr); set;) {
				StagingSet* following = set->next;
				delete set;
				set = following;
			}
		}
		std::uint64_t (*bytesFor)(std::uint32_t);  // header included
		RunRanges ranges{ kReadbackRanges };
		std::atomic<StagingSet*> sets{ nullptr };
		// The largest count a harvest saw past its list. The list grows to it, and what was left out is appended again.
		std::atomic<std::uint32_t> wanted{ 0 };
		// Report counters since the last report.
		std::atomic<std::uint64_t> overflowed{ 0 }, missed{ 0 }, setsAdded{ 0 };

		/** @brief The staging set for a_events, or null if none was added. */
		const StagingSet* SetFor(std::uint32_t a_events) const
		{
			for (const StagingSet* set = sets.load(std::memory_order_acquire); set; set = set->next)
				if (set->events == a_events)
					return set;
			return nullptr;
		}
		/** @brief Adds a staging set for a list of a_events. One thread at a time. */
		void AddSet(std::uint32_t a_events)
		{
			if (SetFor(a_events))
				return;
			auto* set = new StagingSet();
			set->events = a_events;
			for (std::uint32_t i = 0; i < ranges.Count(); ++i) {
				set->buffers.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, bytesFor(a_events)));
				set->buffers.back()->SetName("cs.dclf.event-ring-readback");
			}
			set->next = sets.load(std::memory_order_relaxed);
			while (!sets.compare_exchange_weak(set->next, set, std::memory_order_release, std::memory_order_relaxed)) {}
			setsAdded.fetch_add(1, std::memory_order_relaxed);
		}
		/** @brief Calls a_take on whichever set's buffer holds the range's run armed for a_frame, and asks for growth on overflow.
		 * Returns false, counted as missed, when no buffer holds it. */
		template <class Take>
		bool Harvest(std::uint32_t a_range, std::uint32_t a_frame, Take&& a_take)
		{
			for (const auto* set = sets.load(std::memory_order_acquire); set; set = set->next) {
				if (a_range >= set->buffers.size())
					continue;
				auto resource = set->buffers[a_range]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (!mapped)
					continue;
				const auto* words = static_cast<const std::uint32_t*>(mapped);
				if (words[1] != a_frame) {
					resource.Unmap(0, 0);
					continue;
				}
				const std::uint32_t count = words[0];
				if (count > set->events) {
					std::uint32_t asked = wanted.load(std::memory_order_relaxed);
					while (asked < count && !wanted.compare_exchange_weak(asked, count, std::memory_order_release, std::memory_order_relaxed)) {}
					overflowed.fetch_add(1, std::memory_order_relaxed);
				}
				a_take(words, count, set->events);
				resource.Unmap(0, 0);
				return true;
			}
			missed.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	};

	/** @brief Copies a ring's events into its staging buffer for the run's range. */
	std::shared_ptr<org::RenderPass> MakeEventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<HostEventRing> a_ring);

	/** @brief The visibility readback. Holds the VisibilityEventsCS list, the decoder's state, and the kAccumulated writes it prepares.
	 * See dclf-architecture.md, "The visibility events" and "kAccumulated written by DCLF". */
	struct VisibilityReadback : HostEventRing
	{
		VisibilityReadback() :
			HostEventRing(&VisibilityEventBytes) {}
		/** @brief A root slot's verdict, valid only for the listing of its generation. */
		struct RootVisibility
		{
			std::uint32_t generation = 0;
			bool known = false;    // FadeStateCS updated the listing this frame, so it has a verdict
			bool visible = false;
			std::uint32_t lastFrame = 0;  // the last scene frame it was in view
			std::uint32_t outFrame = 0;   // the scene frame it went out of view
		};
		/** @brief An object slot's verdict, for its occupant in the tables it was decoded with. */
		struct ObjectVisibility
		{
			bool visible = false;
			std::uint32_t lastFrame = 0;
			std::uint32_t outFrame = 0;
		};
		/** @brief A write's node kind. */
		enum WriteKind : std::uint8_t
		{
			kWriteRoot,
			kWriteEntry,
			kWriteGeometry,
		};
		/** @brief The kAccumulated writes from one decode's newest batch, for the main thread. Its nodes stay alive while it holds the retirement node. */
		struct Writes
		{
			struct Write
			{
				void* node = nullptr;
				const void* root = nullptr;  // where the walk that sets the nodes between ends
				bool set = false;
				std::uint8_t kind = kWriteGeometry;
				// A root the cut does not stand in for, a geometry under one, or a plain entry root.
				bool wide = false;
			};
			std::uint32_t frame = 0;  // the scene frame of the batch the writes follow
			bool full = false;        // every stood-in root and geometry, not only the changes
			std::uint64_t publication = 0;  // its nodes are alive while EngineReleases::Alive(publication)
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const EntrySpace> candidates;
			std::shared_ptr<const void> retirement;         // keeps every named node alive until the write
			std::vector<Write> writes;
			Writes* next = nullptr;
		};
		/** @brief What the decoder last asked the main thread to write for a slot. */
		enum Written : std::uint8_t
		{
			kWrittenNone,
			kWrittenClear,
			kWrittenSet,
		};
		// Categories of the observer's finds.
		enum Category : std::uint8_t
		{
			kRootDclfOnly,
			kRootEngineOnly,
			kGeometryDclfOnly,
			kGeometryEngineOnly,
			kCategories,
		};
		/** @brief A frame's events, and what the decoder found for the render thread. */
		struct Batch
		{
			std::uint32_t frame = 0;
			bool reset = false;     // the commit zeroed the reported words, so the verdicts start again
			bool complete = true;   // every change fit, so no verdict arrives a frame late
			std::uint64_t publication = 0;
			std::shared_ptr<const void> retirement;  // keeps the slots the events name theirs until decoded
			std::shared_ptr<const SceneTables> tables;  // released only by the render thread
			std::shared_ptr<const EntrySpace> candidates;
			std::vector<VisibilityEvent> events;
			std::vector<std::pair<std::uint8_t, const void*>> firsts;  // (category, node), named while `tables` holds the node
			Batch* next = nullptr;
		};
		/** @brief What a range's last arm was for. Render thread. */
		struct Armed
		{
			bool armed = false;
			std::uint32_t frame = 0;
			bool reset = false;
			std::uint64_t publication = 0;
			std::shared_ptr<const void> retirement;
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const EntrySpace> candidates;
		};

		// Render thread state. Each range's arm, and harvested batches not yet handed to a task, in frame order.
		std::vector<Armed> armed;
		std::vector<Batch*> pending;
		bool carryReset = false;  // a missed reset arm, so the next batch restarts the decoder
		// Lock-free stack of decoded batches. The task pushes and the render thread takes all.
		std::atomic<Batch*> decoded{ nullptr };
		std::atomic<bool> running{ false };
		// Decoder only. One task at a time, ordered by `running`.
		std::vector<RootVisibility> roots;
		std::vector<ObjectVisibility> objects;
		std::vector<std::uint32_t> rootApplied, objectApplied;  // scene frame of the event last taken
		// Decoder state. The last two decoded frames form the conservative rule's window.
		std::uint32_t decodedFrame = 0, previousDecodedFrame = 0, cutAnswered = 0;
		std::vector<std::uint32_t> rootWrittenGeneration;
		std::vector<std::uint8_t> rootWritten, objectWritten;
		std::vector<const void*> objectWrittenGeometry;
		// Decoder state. Each slot's last ask class (1 is wide), the plain entry roots' last asks, each object's cached plain entry.
		std::vector<std::uint8_t> rootWrittenWide, objectWrittenWide, entryWritten;
		std::vector<std::uint32_t> entryWrittenVersion;
		const EntrySpace* entryCandidates = nullptr;
		std::vector<std::uint32_t> objectEntry;
		std::vector<const void*> objectEntryGeometry;
		// Decoder only. PrepareWrites' incremental state.
		std::shared_ptr<void> decodeState;
		// Any thread. A full write is wanted when the engine culled the stood-in roots itself or writes were dropped.
		// cutFrame is the last camera cut. Stood-in roots stay visible until a batch of that frame or later is decoded.
		std::atomic<bool> refreshWanted{ true };
		std::atomic<std::uint32_t> cutFrame{ 0 };
		// Lock-free stack of prepared writes. The task pushes and Main::Update's entry takes all.
		std::atomic<Writes*> writes{ nullptr };
		// Main thread state. The installed publication's tables at the last write pass, and the cut it wrote.
		std::shared_ptr<const SceneTables> writtenTables;
		Api::RootListView writtenRoots;  // valid while writtenTables is held
		std::shared_ptr<const EntrySpace> writtenCandidates;
		std::uint32_t cutWritten = 0;
		std::uint32_t verdictPrunes = 0;           // passes since rootVerdicts was last pruned
		std::vector<RE::NiAVObject*> chain;        // scratch for a geometry's nodes up to its root
		std::string wideNoChainFirst;              // first geometries whose walk did not reach their root
		// Main thread. Each listed root's verdict as last written, forgotten once the newest tables no longer list it.
		ankerl::unordered_dense::map<const void*, bool> rootVerdicts;
		// Report counters since the last report.
		std::atomic<std::uint64_t> writesSet{ 0 }, writesCleared{ 0 }, writesUnchanged{ 0 }, writesHeldByCut{ 0 };
		std::atomic<std::uint64_t> writesExpired{ 0 };  // should stay 0, since each list holds its node
		std::atomic<std::uint32_t> writeAgeMax{ 0 };    // in frames since its arm
		std::atomic<std::uint64_t> cutWrites{ 0 }, fullWrites{ 0 }, cuts{ 0 }, writeNs{ 0 }, writeBatches{ 0 };
		// Main thread report counters for wide writes, chain walks and cell roots.
		std::atomic<std::uint64_t> wideSet{ 0 }, wideCleared{ 0 }, wideUnchanged{ 0 }, wideNodesSet{ 0 }, wideNoChain{ 0 };
		std::atomic<std::uint64_t> cellRootsSeen{ 0 }, cellRootsSet{ 0 };
		// Decode report counters since the last report.
		std::atomic<std::uint64_t> batches{ 0 }, events{ 0 }, stale{ 0 }, busy{ 0 }, decodeNs{ 0 };
		// Observer counters (<- ACCUMULATED).
		struct Counts
		{
			std::atomic<std::uint64_t> both{ 0 }, dclfOnly{ 0 }, engineOnly{ 0 }, neither{ 0 };
		};
		Counts rootCounts, geometryCounts, stoodInRootCounts, stoodInGeometryCounts;
		std::atomic<std::uint64_t> framesCompared{ 0 }, framesIncomplete{ 0 }, samplesUnmatched{ 0 }, moved{ 0 }, staleGeometry{ 0 };
		std::atomic<std::uint64_t> noVerdictEngine{ 0 }, noVerdictNeither{ 0 }, hiddenDclfOnly{ 0 };
		// Render thread. The first finds' names since the last report, per category.
		std::array<std::string, kCategories> firstNames;

		~VisibilityReadback()
		{
			for (Batch* batch = decoded.exchange(nullptr); batch;) {
				Batch* following = batch->next;
				delete batch;
				batch = following;
			}
			for (Batch* batch : pending)
				delete batch;
			for (Writes* list = writes.exchange(nullptr); list;) {
				Writes* following = list->next;
				delete list;
				list = following;
			}
		}
	};

	/** @brief Any thread. Posts a frame's CPU-made events, which go to the decoder with the harvested batches in frame order. */
	void PostCpuVisibility(VisibilityReadback::Batch* a_batch);

	/** @brief The culling counters, read back without a stall. Each execution copies each source's count words and its latch's stamp.
	 * The render thread arms one source and reads it once that range's copy is done. The stamp says which frame the words are from. */
	struct CounterReadback
	{
		static constexpr std::uint32_t kSourceWords = kCountWords + 1;  // the count words, then the latch's stamp
		explicit CounterReadback(std::uint32_t a_sources) :
			sources(a_sources)
		{
			for (std::uint32_t i = 0; i < ranges.Count(); ++i) {
				buffers.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(std::max(a_sources, 1u)) * kSourceWords * sizeof(std::uint32_t)));
				buffers.back()->SetName("cs.dclf.cull-counter-readback");
			}
		}
		static constexpr std::uint32_t StampOf(std::uint32_t a_frame) { return a_frame & 0x0FFFFFFFu; }  // as the commits write it
		std::uint32_t sources = 0;
		RunRanges ranges{ kReadbackRanges };
		std::vector<std::shared_ptr<org::Buffer>> buffers;  // by range, sources x kSourceWords words
		/** @brief The armed sample. Render thread. */
		struct Armed
		{
			bool armed = false;
			std::uint32_t range = 0, source = 0, stamp = 0;
			std::uint32_t view = 0, mode = 0;              // a shadow view's labels, for the report
			org::GraphHost::GpuPoint done;       // the execution's end, for when the range is not read in its next commit
		} armed;
		std::uint64_t harvested = 0, missed = 0;  // render thread

		/** @brief Calls a_take with the source's count words if the range's buffer holds them as of a_stamp.
		 * The caller must know the copy is done and no later execution writes it. */
		template <class Take>
		bool Read(std::uint32_t a_range, std::uint32_t a_source, std::uint32_t a_stamp, Take&& a_take) const
		{
			if (a_range >= buffers.size() || a_source >= sources)
				return false;
			auto resource = buffers[a_range]->GetAPIResource();
			void* mapped = nullptr;
			resource.Map(&mapped);
			if (!mapped)
				return false;
			const auto* words = static_cast<const std::uint32_t*>(mapped) + std::size_t(a_source) * kSourceWords;
			const bool held = words[kCountWords] == a_stamp;
			if (held)
				a_take(words);
			resource.Unmap(0, 0);
			return held;
		}
	};

	/** @brief A drawn source's count buffer and its culling latch's offset in the run's region. */
	struct CounterSource
	{
		std::shared_ptr<const org::LatchBlock> latch;
		struct Copy
		{
			std::uint32_t source = 0, latchOffset = 0;
		};
		std::vector<Copy> copies;
	};

	/** @brief The epochs whose culling counters are read back. */
	enum class CullCounterView : std::uint8_t
	{
		kColour,
		kShadow,
		kCount,
	};
	/** @brief The view's counter readback as its graph's pass last declared it; null before, or once that pass is gone. */
	std::shared_ptr<CounterReadback> CullCounterReadback(CullCounterView a_view);
	/** @brief Copies a_counts' words and their latches' stamps into a new CounterReadback, published for a_view. */
	std::shared_ptr<org::RenderPass> MakeCounterReadbackPass(CullCounterView a_view, std::vector<std::shared_ptr<org::Buffer>> a_counts,
		std::function<CounterSource(const org::PassPrepareContext&)> a_source);
	struct ShadowResources;
	/** @brief The shadow epoch's counter readback pass, for every drawn view by view slot. */
	std::shared_ptr<org::RenderPass> MakeShadowCullCounterPass(std::shared_ptr<ShadowResources> a_resources);
}
