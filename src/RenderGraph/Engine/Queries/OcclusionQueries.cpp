// The engine's occlusion queries answered by the render graph: the ring slots' state, the BeginQuery, EndQuery and GetQueryData
// detours, and the ORG query rings (OcclusionQueries.h).

#include "RenderGraph/Engine/Queries/OcclusionQueries.h"

#include "RenderGraph/Switches.h"
#include "Utils/Logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <utility>

#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Resources/Buffers/Buffer.h>

#	include <vector>
#endif

namespace RenderGraph::Engine::OcclusionQueries
{
	namespace
	{
		// GetQueryData's not-ready answer, as the engine returns it.
		constexpr std::uint32_t kNotReady = 0xFFFFFFFF;
		// The first answers logged one by one, then one summary per ring slot every kSummaryReads answers.
		constexpr std::uint32_t kLoggedAnswers = 12;
		constexpr std::uint32_t kSummaryReads = 100;

#if defined(CS_HAS_RENDER_GRAPH)
		using Ring = QueryRing::State;
#else
		struct Ring
		{};
#endif

		/** @brief One entry of the engine's ring. */
		struct Slot
		{
			bool graph = false;  // its last BeginQuery was inside a Bracket: the graph answers it
			bool begun = false;  // between that BeginQuery and its EndQuery
			bool ready = false;  // value answers GetQueryData
			std::uint32_t value = 0;
			std::uint32_t last = 0;  // the last count it answered or was given
			// The run that counts it, once committed: the ring, its range (the run's lease index) and that commit's generation, and
			// the query index.
			std::shared_ptr<Ring> ring;
			std::uint32_t range = 0;
			std::uint64_t generation = 0;
			std::uint32_t query = 0;
			// The log's summary since its last line.
			std::uint32_t reads = 0, notReady = 0, minimum = kNotReady, maximum = 0;
			std::uint64_t sum = 0;
		};

		struct Counters
		{
			std::atomic<std::uint64_t> graphAnswers{ 0 }, d3d11Answers{ 0 }, notReady{ 0 }, skipped{ 0 }, waits{ 0 };
		};

		struct Globals
		{
			std::mutex mutex;
			std::array<Slot, kRingSlots> slots;
			Counters counters;
			std::uint32_t logged = 0;
			bool active = false;
		};

		Globals& Get()
		{
			static Globals globals;
			return globals;
		}

		thread_local Bracket* current = nullptr;

		Util::ReasonLog& Skips()
		{
			static Util::ReasonLog log("[ORG] occlusion queries: a query answers its last count", spdlog::level::warn);
			return log;
		}

		/** @brief The ring slot of an engine query entry (a pointer into the ring), or kNoSlot. */
		std::int32_t SlotOf(ID3D11Query** a_entry)
		{
			static REL::Relocation<ID3D11Query**> ring{ REL::RelocationID(524740, 411344) };
			if (!a_entry)
				return kNoSlot;
			const auto index = a_entry - ring.get();
			return index >= 0 && index < static_cast<std::ptrdiff_t>(kRingSlots) ? static_cast<std::int32_t>(index) : kNoSlot;
		}

		/** @brief A count for GetQueryData: its low 32 bits, never the not-ready value. */
		std::uint32_t Answer(std::uint64_t a_count)
		{
			return static_cast<std::uint32_t>((std::min)(a_count, std::uint64_t{ kNotReady - 1 }));
		}

		/** @brief Gives a slot its count (mutex held). */
		void Deliver(Slot& a_slot, std::uint32_t a_value)
		{
			a_slot.value = a_value;
			a_slot.last = a_value;
			a_slot.ready = true;
			a_slot.ring.reset();
		}

		/** @brief Logs what the engine read (mutex held): the first answers one by one, then a summary per slot. */
		void NoteRead(Globals& a_globals, std::int32_t a_index, Slot& a_slot, std::uint32_t a_value, bool a_graph)
		{
			if (a_value == kNotReady) {
				++a_slot.notReady;
				return;
			}
			++a_slot.reads;
			a_slot.sum += a_value;
			a_slot.minimum = (std::min)(a_slot.minimum, a_value);
			a_slot.maximum = (std::max)(a_slot.maximum, a_value);
			const char* path = a_graph ? "the graph's" : "D3D11's";
			if (a_globals.logged < kLoggedAnswers) {
				++a_globals.logged;
				logger::info("[ORG] occlusion queries: ring slot {} read {} samples ({} count)", a_index, a_value, path);
			}
			if (a_slot.reads >= kSummaryReads) {
				logger::info("[ORG] occlusion queries: ring slot {} ({} count): {} reads, samples last {}, min {}, max {}, mean {:.1f}; {} not-ready polls", a_index, path,
					a_slot.reads, a_value, a_slot.minimum, a_slot.maximum, static_cast<double>(a_slot.sum) / a_slot.reads, a_slot.notReady);
				a_slot.reads = a_slot.notReady = 0;
				a_slot.sum = 0;
				a_slot.minimum = kNotReady;
				a_slot.maximum = 0;
			}
		}

		/** @brief Skips a graph slot (mutex held): it answers its last count. */
		void SkipSlot(Globals& a_globals, Slot& a_slot, std::string_view a_why)
		{
			Deliver(a_slot, a_slot.last);
			a_globals.counters.skipped.fetch_add(1, std::memory_order_relaxed);
			Skips().Report(a_why);
		}

#if defined(CS_HAS_RENDER_GRAPH)
		/**
		 * @brief Reads the counts of a_ring's range (a lease index) into the slots its run still counts, once its work is done (mutex
		 * held): a_retired at the next commit of the lease index (its last run retired), else when the run's GPU point is reached. False
		 * when not yet.
		 */
		bool Harvest(Globals& a_globals, Ring& a_ring, std::uint32_t a_range, bool a_retired);
#endif
	}

#if defined(CS_HAS_RENDER_GRAPH)
	/** @brief A QueryRing's pool, readback and runs. */
	struct QueryRing::State
	{
		/** @brief A range's last run. */
		struct Run
		{
			std::vector<std::int32_t> slots;  // the engine slot each query counts, kNoSlot for none
			std::uint64_t generation = 0;
			bool committed = false, submitted = false, harvested = false;
			org::GraphHost::GpuPoint point;  // every queue past the run's submission
			std::uint64_t seals = 0;          // GraphHost::Seals at its submission
		};

		/** @brief The ranges of kRunsPerChunk lease indices: a query pool and its readback. */
		struct Chunk
		{
			rhi::QueryPoolPtr pool;
			std::shared_ptr<org::Buffer> readback;
		};

		std::string name;
		mutable rhi::Device device;  // ChunkOf makes pools from a const ring
		std::uint32_t perRun = 0;
		std::uint64_t stride = 8;
		// Grown by record time (any thread) and read by the render thread: guarded by chunkMutex. A chunk never moves.
		mutable std::mutex chunkMutex;
		mutable std::vector<std::unique_ptr<Chunk>> chunks;
		mutable bool chunkFailed = false;
		std::vector<Run> runs;  // by range (lease index), grown at commit; guarded by the globals' mutex
		std::uint32_t lastCommitted = ~0u;
		std::uint64_t generations = 0;

		/** @brief The chunk of a range, made on first use. Null, logged once, when its pool or readback cannot be made. */
		Chunk* ChunkOf(std::uint32_t a_range) const
		{
			const std::uint32_t index = a_range / QueryRing::kRunsPerChunk;
			const std::scoped_lock lock(chunkMutex);
			if (chunks.size() <= index)
				chunks.resize(static_cast<std::size_t>(index) + 1);
			auto& chunk = chunks[index];
			if (chunk)
				return chunk.get();
			auto made = std::make_unique<Chunk>();
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::Occlusion;
			desc.count = QueryRing::kRunsPerChunk * perRun;
			if (device.CreateQueryPool(desc, made->pool) == rhi::Result::Ok && made->pool) {
				made->pool->SetName(std::format("{}.pool{}", name, index).c_str());
				made->readback = org::Buffer::CreateShared(rhi::HeapType::Readback, stride * desc.count);
				if (made->readback)
					made->readback->SetName(std::format("{}.readback{}", name, index));
			}
			if (!made->pool || !made->readback) {
				if (!std::exchange(chunkFailed, true))
					logger::warn("[ORG] occlusion queries: {}: no query pool or readback for lease indices from {}", name, index * QueryRing::kRunsPerChunk);
				return nullptr;
			}
			chunk = std::move(made);
			return chunk.get();
		}
		/** @brief A range's first query in its chunk's pool. */
		std::uint32_t First(std::uint32_t a_range) const { return (a_range % QueryRing::kRunsPerChunk) * perRun; }
	};

	namespace
	{
		bool Harvest(Globals& a_globals, Ring& a_ring, std::uint32_t a_range, bool a_retired)
		{
			if (a_range >= a_ring.runs.size())
				return true;
			auto& run = a_ring.runs[a_range];
			if (!run.committed || run.harvested)
				return true;
			const auto bound = [&](std::int32_t a_index) -> Slot* {
				if (a_index < 0 || a_index >= static_cast<std::int32_t>(kRingSlots))
					return nullptr;
				auto& slot = a_globals.slots[a_index];
				return slot.ring.get() == &a_ring && slot.range == a_range && slot.generation == run.generation ? &slot : nullptr;
			};
			if (!run.submitted) {
				// Committed but never submitted (the run failed after its commit): nothing counted it.
				for (const auto index : run.slots)
					if (auto* slot = bound(index))
						SkipSlot(a_globals, *slot, "its run was committed but not submitted");
				run.harvested = true;
				return true;
			}
			// At the next commit of the run's lease index the lease has retired, which holds the run: its point may also name later
			// work on other queues, so it is not asked then.
			if (!a_retired && !run.point.Reached())
				return false;
			auto* chunk = a_ring.ChunkOf(a_range);
			void* mapped = nullptr;
			if (chunk)
				chunk->readback->GetAPIResource().Map(&mapped);
			const auto* counts = static_cast<const std::uint64_t*>(mapped);
			for (std::uint32_t i = 0; i < run.slots.size(); ++i) {
				auto* slot = bound(run.slots[i]);
				if (!slot)
					continue;
				if (counts)
					Deliver(*slot, Answer(counts[(std::uint64_t(a_ring.First(a_range)) + i) * a_ring.stride / sizeof(std::uint64_t)]));
				else
					SkipSlot(a_globals, *slot, "its readback could not be mapped");
			}
			if (mapped)
				chunk->readback->GetAPIResource().Unmap(0, 0);
			run.harvested = true;
			return true;
		}
	}

	std::shared_ptr<QueryRing> QueryRing::Create(std::string_view a_name, std::uint32_t a_perRun)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !a_perRun || a_perRun > 32)
			return nullptr;
		auto state = std::make_shared<State>();
		state->name = std::string(a_name);
		state->device = host->Device();
		state->perRun = a_perRun;
		// The first chunk now: a pool that cannot be made at all fails the client's setup, as before.
		{
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::Occlusion;
			desc.count = kRunsPerChunk * a_perRun;
			rhi::QueryPoolPtr probe;
			if (state->device.CreateQueryPool(desc, probe) != rhi::Result::Ok || !probe) {
				logger::warn("[ORG] occlusion queries: {}: no occlusion query pool of {}", state->name, desc.count);
				return nullptr;
			}
			state->stride = (std::max)(probe->GetQueryResultInfo().elementSize, 8u);
		}
		if (!state->ChunkOf(0))
			return nullptr;
		logger::info("[ORG] occlusion queries: {}: {} queries a run, in pools of {} runs made as the host's runs in flight need them", state->name,
			a_perRun, kRunsPerChunk);
		return std::make_shared<QueryRing>(std::move(state));
	}

	std::uint32_t QueryRing::PerRun() const { return state->perRun; }

	rhi::QueryPoolHandle QueryRing::Pool(const org::RunRef& a_run) const
	{
		const auto* chunk = state->ChunkOf(a_run.lease);
		return chunk ? chunk->pool->GetHandle() : rhi::QueryPoolHandle{};
	}

	std::uint32_t QueryRing::Index(const org::RunRef& a_run, std::uint32_t a_query) const
	{
		return state->First(a_run.lease) + (std::min)(a_query, state->perRun - 1);
	}

	void QueryRing::Reset(rhi::CommandList& a_commands, const org::RunRef& a_run) const
	{
		if (const auto pool = Pool(a_run); pool.valid())
			a_commands.ResetQueries(pool, Index(a_run, 0), state->perRun);
	}

	void QueryRing::Resolve(rhi::CommandList& a_commands, const org::RunRef& a_run, std::uint32_t a_begun) const
	{
		const auto* chunk = state->ChunkOf(a_run.lease);
		if (!chunk)
			return;
		const auto pool = chunk->pool->GetHandle();
		const auto first = Index(a_run, 0);
		for (std::uint32_t i = 0; i < state->perRun; ++i) {
			if (a_begun & (1u << i))
				continue;
			a_commands.BeginQuery(pool, first + i);
			a_commands.EndQuery(pool, first + i);
		}
		a_commands.ResolveQueryData(pool, first, state->perRun, chunk->readback->GetAPIResource().GetHandle(), std::uint64_t(first) * state->stride);
	}

	void QueryRing::Commit(const org::RunRef& a_run, std::span<const std::int32_t> a_slots)
	{
		auto& globals = Get();
		const std::scoped_lock lock(globals.mutex);
		const std::uint32_t range = a_run.lease;
		if (state->runs.size() <= range)
			state->runs.resize(static_cast<std::size_t>(range) + 1);
		// The lease index's last run retired before the index was opened again: its counts are there.
		Harvest(globals, *state, range, true);
		auto& run = state->runs[range];
		run = {};
		run.generation = ++state->generations;
		run.committed = true;
		for (std::uint32_t i = 0; i < a_slots.size() && i < state->perRun; ++i) {
			const auto index = a_slots[i];
			run.slots.push_back(index);
			if (index < 0 || index >= static_cast<std::int32_t>(kRingSlots))
				continue;
			auto& slot = globals.slots[index];
			if (!slot.graph)
				continue;
			slot.ring = state;
			slot.range = range;
			slot.generation = run.generation;
			slot.query = i;
		}
		state->lastCommitted = range;
	}

	void QueryRing::Executed()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto& globals = Get();
		const std::scoped_lock lock(globals.mutex);
		if (state->lastCommitted >= state->runs.size() || !host)
			return;
		auto& run = state->runs[state->lastCommitted];
		run.submitted = true;
		run.point = host->SubmittedPoint();
		run.seals = host->Seals();
	}
#endif

	bool Requested()
	{
		static const bool requested = !REL::Module::IsVR() && Switches::On("CS_ORG_OCCLUSION_QUERIES");
		return requested;
	}

	bool Active()
	{
		return Get().active;
	}

	/** @brief The three detours. */
	struct Hooks
	{
		struct BeginQuery
		{
			static void thunk(void* a_renderer, ID3D11Query** a_entry)
			{
				const auto index = SlotOf(a_entry);
				if (index != kNoSlot) {
					auto& globals = Get();
					const std::scoped_lock lock(globals.mutex);
					auto& slot = globals.slots[index];
					if (current && current->slot == kNoSlot) {
						// The bracket's query: the graph counts its draw.
						current->slot = index;
						slot.graph = true;
						slot.begun = true;
						slot.ready = false;
						slot.ring.reset();
						return;
					}
					if (current)
						Skips().Report("a second query begun inside one bracket is the engine's");
					slot.graph = false;
					slot.begun = false;
					slot.ring.reset();
				}
				func(a_renderer, a_entry);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct EndQuery
		{
			static void thunk(void* a_renderer, ID3D11Query** a_entry)
			{
				const auto index = SlotOf(a_entry);
				if (index != kNoSlot) {
					auto& globals = Get();
					const std::scoped_lock lock(globals.mutex);
					auto& slot = globals.slots[index];
					if (slot.graph) {
						slot.begun = false;
						return;
					}
				}
				func(a_renderer, a_entry);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GetQueryData
		{
			static std::uint32_t thunk(void* a_renderer, ID3D11Query** a_entry, bool a_wait, bool a_flush)
			{
				const auto index = SlotOf(a_entry);
				auto& globals = Get();
				if (index == kNoSlot) {
					return func(a_renderer, a_entry, a_wait, a_flush);
				}
				std::unique_lock lock(globals.mutex);
				auto& slot = globals.slots[index];
				if (!slot.graph) {
					lock.unlock();
					const auto value = func(a_renderer, a_entry, a_wait, a_flush);
					lock.lock();
					if (value != kNotReady)
						globals.counters.d3d11Answers.fetch_add(1, std::memory_order_relaxed);
					NoteRead(globals, index, slot, value, false);
					return value;
				}
				const auto value = Read(globals, lock, slot, a_wait);
				if (value != kNotReady)
					globals.counters.graphAnswers.fetch_add(1, std::memory_order_relaxed);
				else
					globals.counters.notReady.fetch_add(1, std::memory_order_relaxed);
				NoteRead(globals, index, slot, value, true);
				return value;
			}
			static inline REL::Relocation<decltype(thunk)> func;

			/** @brief A graph slot's count (mutex held through a_lock): ready, read from its run's readback, or not ready. */
			static std::uint32_t Read([[maybe_unused]] Globals& a_globals, [[maybe_unused]] std::unique_lock<std::mutex>& a_lock, Slot& a_slot,
				[[maybe_unused]] bool a_wait)
			{
				if (a_slot.ready)
					return a_slot.value;
				// Still between BeginQuery and EndQuery, or begun and not yet committed: D3D11 would not have the count either.
				if (a_slot.begun || !a_slot.ring)
					return kNotReady;
#if defined(CS_HAS_RENDER_GRAPH)
				auto ring = a_slot.ring;
				const auto range = a_slot.range;
				if (Harvest(a_globals, *ring, range, false))
					return a_slot.ready ? a_slot.value : kNotReady;
				if (!a_wait)
					return kNotReady;
				// The blocking form: wait for the run's GPU work, but only once the host sealed it (it is on the device then). Before that it
				// may still sit in DXVK's stream behind this very frame, and a wait here would never end.
				auto* host = RenderGraphRuntime::Get().Host();
				const auto& run = ring->runs[range];
				if (!host || host->Seals() <= run.seals) {
					Skips().Report("a blocking read of a query its frame has not sealed yet was answered not-ready");
					return kNotReady;
				}
				const auto point = run.point;
				const auto generation = run.generation;
				a_globals.counters.waits.fetch_add(1, std::memory_order_relaxed);
				a_lock.unlock();
				const bool waited = point.Wait();
				a_lock.lock();
				if (ring->runs[range].generation == generation && waited)
					Harvest(a_globals, *ring, range, false);
				return a_slot.ready ? a_slot.value : kNotReady;
#else
				return kNotReady;
#endif
			}
		};
	};

	void Install()
	{
		// CS_ORG_OCCLUSION_QUERY_LOG=1 alone: only GetQueryData, to log D3D11's answers for a comparison with the graph's.
		if (!Requested()) {
			if (REL::Module::IsVR() || !Switches::On("CS_ORG_OCCLUSION_QUERY_LOG"))
				return;
			stl::detour_thunk<Hooks::GetQueryData>(REL::RelocationID(75569, 77370));
			if (Hooks::GetQueryData::func.address())
				logger::info("[ORG] occlusion queries: logging D3D11's answers (CS_ORG_OCCLUSION_QUERY_LOG=1); the queries stay on D3D11");
			return;
		}
		stl::detour_thunk<Hooks::BeginQuery>(REL::RelocationID(75567, 77368));
		stl::detour_thunk<Hooks::EndQuery>(REL::RelocationID(75568, 77369));
		stl::detour_thunk<Hooks::GetQueryData>(REL::RelocationID(75569, 77370));
		if (!Hooks::BeginQuery::func.address() || !Hooks::EndQuery::func.address() || !Hooks::GetQueryData::func.address()) {
			logger::error("[ORG] occlusion queries: a detour failed; the engine's queries stay on D3D11");
			return;
		}
		Get().active = true;
		logger::info("[ORG] occlusion queries: on (CS_ORG_OCCLUSION_QUERIES=1): the queries a port brackets are answered by the graph");
	}

	Bracket::Bracket()
	{
		outer = current;
		current = this;
	}

	Bracket::~Bracket()
	{
		current = outer;
		if (slot == kNoSlot)
			return;
		auto& globals = Get();
		const std::scoped_lock lock(globals.mutex);
		auto& entry = globals.slots[slot];
		if (!entry.graph || entry.ready)
			return;
		// Begun and not counted by any run: neither committed, nor committed by a run that was then submitted.
		bool submitted = false;
#if defined(CS_HAS_RENDER_GRAPH)
		if (entry.ring) {
			const auto& run = entry.ring->runs[entry.range];
			submitted = run.generation == entry.generation && run.submitted;
		}
#endif
		if (!submitted)
			SkipSlot(globals, entry, "its bracket's draw was not submitted");
	}

	void Bracket::Skip(std::string_view a_why)
	{
		if (slot == kNoSlot)
			return;
		auto& globals = Get();
		const std::scoped_lock lock(globals.mutex);
		auto& entry = globals.slots[slot];
		if (entry.graph && !entry.ready)
			SkipSlot(globals, entry, a_why);
	}

	std::string Status()
	{
		if (!Active())
			return Requested() ? "requested, not installed" : "off (CS_ORG_OCCLUSION_QUERIES)";
		const auto& c = Get().counters;
		return std::format("{} answers from the graph, {} from D3D11, {} not-ready polls, {} skipped, {} blocking waits", c.graphAnswers.load(),
			c.d3d11Answers.load(), c.notReady.load(), c.skipped.load(), c.waits.load());
	}
}
