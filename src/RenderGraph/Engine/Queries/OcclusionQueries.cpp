// The engine's occlusion queries answered by the render graph. See OcclusionQueries.h.

#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"

#include "RenderGraph/Switches.h"
#include "Utils/Logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <utility>

#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RunRef.h>
#include <Resources/Buffers/Buffer.h>

#include <memory>
#include <string>
#include <vector>

namespace RenderGraph::Engine::OcclusionQueries
{
	namespace
	{
		constexpr std::uint32_t kNotReady = 0xFFFFFFFF;
		static_assert(kRingSlots <= 64, "Harvest keeps one bit per ring slot");
		// The first answers are logged one by one, then a summary per ring slot every kSummaryReads answers.
		constexpr std::uint32_t kLoggedAnswers = 12;
		constexpr std::uint32_t kSummaryReads = 100;

		using Ring = QueryRing::State;

		struct Slot
		{
			bool graph = false;  // its last BeginQuery was inside a Bracket
			bool begun = false;
			bool ready = false;
			std::uint32_t value = 0;
			std::uint32_t last = 0;
			// Set at BeginQuery from the bracket.
			bool hasSkipAnswer = false;
			std::uint32_t skipAnswer = 0;
			std::shared_ptr<Ring> ring;
			std::uint32_t range = 0;
			std::uint64_t generation = 0;
			std::uint32_t query = 0;
			std::uint32_t reads = 0, notReady = 0, minimum = kNotReady, maximum = 0;
			std::uint64_t sum = 0;
		};

		struct Counters
		{
			std::atomic<std::uint64_t> graphAnswers{ 0 }, notReady{ 0 }, skipped{ 0 }, waits{ 0 };
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

		/** @brief The ring slot of a pointer into the engine's ring, or kNoSlot. */
		std::int32_t SlotOf(ID3D11Query** a_entry)
		{
			static REL::Relocation<ID3D11Query**> ring{ REL::RelocationID(524740, 411344) };
			if (!a_entry)
				return kNoSlot;
			const auto index = a_entry - ring.get();
			return index >= 0 && index < static_cast<std::ptrdiff_t>(kRingSlots) ? static_cast<std::int32_t>(index) : kNoSlot;
		}

		/** @brief Low 32 bits, never the not-ready value. */
		std::uint32_t Answer(std::uint64_t a_count)
		{
			return static_cast<std::uint32_t>((std::min)(a_count, std::uint64_t{ kNotReady - 1 }));
		}

		/** @brief Mutex held. */
		void Deliver(Slot& a_slot, std::uint32_t a_value)
		{
			a_slot.value = a_value;
			a_slot.last = a_value;
			a_slot.ready = true;
			a_slot.ring.reset();
		}

		/** @brief Mutex held. */
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

		/** @brief Mutex held. */
		void SkipSlot(Globals& a_globals, Slot& a_slot, std::string_view a_why)
		{
			Deliver(a_slot, a_slot.hasSkipAnswer ? a_slot.skipAnswer : a_slot.last);
			a_globals.counters.skipped.fetch_add(1, std::memory_order_relaxed);
			Skips().Report(a_why);
		}

		/** @brief Mutex held. Reads a range's counts once its work is done, either at the lease's next commit or when its GPU point is reached. */
		bool Harvest(Globals& a_globals, Ring& a_ring, std::uint32_t a_range, bool a_retired);
	}

	struct QueryRing::State
	{
		struct Run
		{
			org::RunRef ref;                  // a stage's staging run until the stage's commit moves it
			std::vector<std::int32_t> slots;  // kNoSlot for none
			std::uint64_t generation = 0;
			bool committed = false, submitted = false, harvested = false;
			bool unresolved = false;         // its pass had not declared the readback, so nothing was resolved into it
			org::GraphHost::GpuPoint point;
			std::uint64_t seals = 0;
		};

		struct Chunk
		{
			rhi::QueryPoolPtr pool;
			std::shared_ptr<org::Buffer> readback;
		};

		std::string name;
		mutable rhi::Device device;  // ChunkOf makes pools from a const ring
		std::uint32_t perRun = 0;
		std::uint64_t stride = 8;
		// Grown at commit on the render thread and read at record time on any thread. A chunk never moves.
		mutable std::mutex chunkMutex;
		mutable std::vector<std::unique_ptr<Chunk>> chunks;
		std::vector<Run> runs;  // by lease index, guarded by the globals' mutex
		std::uint32_t lastCommitted = ~0u;
		std::uint64_t generations = 0;

		/** @brief Moves a stage's staging-run commits onto the run that submits it. */
		struct Mover final : ::RenderGraph::RunSlotsRegistry::Entry
		{
			State* state = nullptr;
			void MoveRun(const org::RunRef& a_from, const org::RunRef& a_to) const override;
		};
		Mover mover;

		State()
		{
			mover.state = this;
			::RenderGraph::RunSlotsRegistry::Add(&mover);
		}
		~State() { ::RenderGraph::RunSlotsRegistry::Remove(&mover); }
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		static std::uint32_t ChunkIndex(std::uint32_t a_range) { return a_range / QueryRing::kRunsPerChunk; }

		/** @brief With a_make, on the render thread at commit, makes a new chunk. Null if not made or on failure, logged once. */
		Chunk* ChunkOf(std::uint32_t a_range, bool a_make = false, bool* a_made = nullptr) const
		{
			const std::uint32_t index = ChunkIndex(a_range);
			const std::scoped_lock lock(chunkMutex);
			if (chunks.size() <= index) {
				if (!a_make)
					return nullptr;
				chunks.resize(static_cast<std::size_t>(index) + 1);
			}
			auto& chunk = chunks[index];
			if (chunk || !a_make)
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
			if (!made->pool || !made->readback)
				RenderGraph::Fatal("occlusion queries", std::format("{}: no query pool or readback for lease indices from {}", name, index * QueryRing::kRunsPerChunk));
			chunk = std::move(made);
			if (a_made)
				*a_made = true;
			return chunk.get();
		}
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
			// A stage run not yet submitted is not ready until the stage submits it or its lease comes round.
			if (!run.submitted && !run.unresolved && !a_retired)
				return false;
			if (!run.submitted || run.unresolved) {
				// A run that failed after its commit, or a pass built before its readback existed, counted nothing.
				for (const auto index : run.slots)
					if (auto* slot = bound(index))
						SkipSlot(a_globals, *slot, run.unresolved ? "its readback was not declared yet" : "its run was committed but not submitted");
				run.harvested = true;
				return true;
			}
			// At the lease's next commit the run has retired. Its point may name later work on other queues, so do not ask it then.
			if (!a_retired && !run.point.Reached())
				return false;
			auto* chunk = a_ring.ChunkOf(a_range);
			void* mapped = nullptr;
			if (chunk)
				chunk->readback->GetAPIResource().Map(&mapped);
			const auto* counts = static_cast<const std::uint64_t*>(mapped);
			// A slot several queries count answers their sum.
			std::array<std::uint64_t, kRingSlots> sums{};
			std::uint64_t named = 0;
			for (std::uint32_t i = 0; i < run.slots.size(); ++i) {
				const auto index = run.slots[i];
				if (!bound(index))
					continue;
				named |= std::uint64_t{ 1 } << index;
				if (counts)
					sums[index] += counts[(std::uint64_t(a_ring.First(a_range)) + i) * a_ring.stride / sizeof(std::uint64_t)];
			}
			for (std::uint32_t index = 0; index < kRingSlots; ++index) {
				if (!(named & (std::uint64_t{ 1 } << index)))
					continue;
				auto& slot = a_globals.slots[index];
				if (counts)
					Deliver(slot, Answer(sums[index]));
				else
					SkipSlot(a_globals, slot, "its readback could not be mapped");
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
		// A pool that cannot be made at all fails the client's setup.
		{
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::Occlusion;
			desc.count = kRunsPerChunk * a_perRun;
			rhi::QueryPoolPtr probe;
			if (state->device.CreateQueryPool(desc, probe) != rhi::Result::Ok || !probe)
				RenderGraph::Fatal("occlusion queries", std::format("{}: no occlusion query pool of {}", state->name, desc.count));
			state->stride = (std::max)(probe->GetQueryResultInfo().elementSize, 8u);
		}
		// Make every chunk play reaches now, so the first build declares their readbacks and no commit forces a rebuild mid-play.
		for (std::uint32_t chunk = 0; chunk < kSetupChunks; ++chunk)
			if (!state->ChunkOf(chunk * kRunsPerChunk, true))
				return nullptr;
		logger::info("[ORG] occlusion queries: {}: {} queries a run, in pools of {} runs; {} lease indices made at setup", state->name, a_perRun, kRunsPerChunk,
			kSetupChunks * kRunsPerChunk);
		return std::make_shared<QueryRing>(std::move(state));
	}

	std::uint32_t QueryRing::PerRun() const { return state->perRun; }

	std::vector<std::shared_ptr<org::Resource>> QueryRing::Readbacks() const
	{
		const std::scoped_lock lock(state->chunkMutex);
		std::vector<std::shared_ptr<org::Resource>> readbacks;
		// Chunks are made in lease order, so the made ones are a prefix.
		for (const auto& chunk : state->chunks) {
			if (!chunk)
				break;
			readbacks.push_back(chunk->readback);
		}
		return readbacks;
	}

	rhi::QueryPoolHandle QueryRing::Pool(const org::RunRef& a_run) const
	{
		const auto* chunk = state->ChunkOf(a_run.lease);
		return chunk ? chunk->pool->GetHandle() : rhi::QueryPoolHandle{};
	}

	std::uint32_t QueryRing::Index(const org::RunRef& a_run, std::uint32_t a_query) const
	{
		return state->First(a_run.lease) + (std::min)(a_query, state->perRun - 1);
	}

	void QueryRing::Reset(rhi::CommandList& a_commands, const org::RunRef& a_run, std::uint32_t a_first, std::uint32_t a_count) const
	{
		if (a_first >= state->perRun)
			return;
		const auto count = (std::min)(a_count, state->perRun - a_first);
		if (const auto pool = Pool(a_run); pool.valid())
			a_commands.ResetQueries(pool, Index(a_run, a_first), count);
	}

	void QueryRing::Resolve(rhi::CommandList& a_commands, const org::RunRef& a_run, std::uint32_t a_begun, std::size_t a_declared, std::uint32_t a_first,
		std::uint32_t a_count) const
	{
		if (a_first >= state->perRun)
			return;
		const auto count = (std::min)(a_count, state->perRun - a_first);
		const auto* chunk = state->ChunkOf(a_run.lease);
		if (!chunk)
			return;
		if (State::ChunkIndex(a_run.lease) >= a_declared) {
			// The pass was built before the chunk existed, so its readback is not declared.
			auto& globals = Get();
			const std::scoped_lock lock(globals.mutex);
			if (a_run.lease < state->runs.size())
				state->runs[a_run.lease].unresolved = true;
			return;
		}
		const auto pool = chunk->pool->GetHandle();
		const auto first = Index(a_run, a_first);
		for (std::uint32_t i = 0; i < count; ++i) {
			if (a_begun & (1u << i))
				continue;
			a_commands.BeginQuery(pool, first + i);
			a_commands.EndQuery(pool, first + i);
		}
		a_commands.ResolveQueryData(pool, first, count, chunk->readback->GetAPIResource().GetHandle(), std::uint64_t(first) * state->stride);
	}

	bool QueryRing::Commit(const org::RunRef& a_run, std::span<const std::int32_t> a_slots, std::uint32_t a_first)
	{
		const std::uint32_t range = a_run.lease;
		// Make the chunk here, never at record time, so every readback a pass writes is one it declared.
		bool made = false;
		for (std::uint32_t chunk = 0; chunk <= State::ChunkIndex(range); ++chunk)
			state->ChunkOf(chunk * kRunsPerChunk, true, &made);
		auto& globals = Get();
		const std::scoped_lock lock(globals.mutex);
		if (state->runs.size() <= range)
			state->runs.resize(static_cast<std::size_t>(range) + 1);
		auto& run = state->runs[range];
		// Another tap of the same run adds its queries.
		const bool same = run.committed && !run.submitted && run.ref == a_run;
		if (!same) {
			// The lease's last run retired before reopening, so its counts are there.
			Harvest(globals, *state, range, true);
			run = {};
			run.generation = ++state->generations;
			run.committed = true;
			run.ref = a_run;
		}
		for (std::uint32_t i = 0; i < a_slots.size() && a_first + i < state->perRun; ++i) {
			const auto query = a_first + i;
			const auto index = a_slots[i];
			if (run.slots.size() <= query)
				run.slots.resize(static_cast<std::size_t>(query) + 1, kNoSlot);
			run.slots[query] = index;
			if (index < 0 || index >= static_cast<std::int32_t>(kRingSlots))
				continue;
			auto& slot = globals.slots[index];
			if (!slot.graph)
				continue;
			slot.ring = state;
			slot.range = range;
			slot.generation = run.generation;
			slot.query = query;
		}
		state->lastCommitted = range;
		return made;
	}

	void QueryRing::State::Mover::MoveRun(const org::RunRef& a_from, const org::RunRef& a_to) const
	{
		auto& ring = *state;
		if (a_from.lease == a_to.lease)
			return;
		auto& globals = Get();
		{
			const std::scoped_lock lock(globals.mutex);
			if (a_from.lease >= ring.runs.size() || !ring.runs[a_from.lease].committed || !(ring.runs[a_from.lease].ref == a_from))
				return;
		}
		// Record time never makes a chunk, so make it now.
		for (std::uint32_t chunk = 0; chunk <= State::ChunkIndex(a_to.lease); ++chunk)
			ring.ChunkOf(chunk * QueryRing::kRunsPerChunk, true);
		const std::scoped_lock lock(globals.mutex);
		if (ring.runs.size() <= a_to.lease)
			ring.runs.resize(static_cast<std::size_t>(a_to.lease) + 1);
		Harvest(globals, ring, a_to.lease, true);
		auto moved = std::exchange(ring.runs[a_from.lease], Run{});
		moved.ref = a_to;
		for (auto& slot : globals.slots)
			if (slot.ring.get() == &ring && slot.range == a_from.lease && slot.generation == moved.generation)
				slot.range = a_to.lease;
		ring.runs[a_to.lease] = std::move(moved);
		if (ring.lastCommitted == a_from.lease)
			ring.lastCommitted = a_to.lease;
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

	bool Active()
	{
		return Get().active;
	}

	struct Hooks
	{
		struct BeginQuery
		{
			static void thunk(void* a_renderer, ID3D11Query** a_entry)
			{
				const auto index = SlotOf(a_entry);
				if (index == kNoSlot) {
					func(a_renderer, a_entry);
					return;
				}
				auto& globals = Get();
				bool second = false;
				{
					const std::scoped_lock lock(globals.mutex);
					auto& slot = globals.slots[index];
					if (current && current->slot == kNoSlot) {
						current->slot = index;
						slot.graph = true;
						slot.begun = true;
						slot.ready = false;
						slot.hasSkipAnswer = current->hasSkipAnswer;
						slot.skipAnswer = current->skipAnswer;
						slot.ring.reset();
						return;
					}
					// Graph mode has no place for a query outside a bracket. It answers its last count, and D3D11 never sees it.
					second = current != nullptr;
					slot.graph = true;
					slot.begun = false;
					slot.hasSkipAnswer = false;
					slot.ring.reset();
					SkipSlot(globals, slot, second ? "a second query begun inside one bracket" : "a query begun outside a bracket");
				}
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::EngineWork, RenderGraph::Frame::SegmentId::Count,
					std::format("occlusion query ring slot {}: {}", index, second ? "a second query begun inside one bracket" : "a query begun outside a bracket"));
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

			/** @brief Mutex held through a_lock. Ready, read from the run's readback, or not ready. */
			static std::uint32_t Read([[maybe_unused]] Globals& a_globals, [[maybe_unused]] std::unique_lock<std::mutex>& a_lock, Slot& a_slot,
				[[maybe_unused]] bool a_wait)
			{
				if (a_slot.ready)
					return a_slot.value;
				// D3D11 would not have the count yet either.
				if (a_slot.begun || !a_slot.ring)
					return kNotReady;
				auto ring = a_slot.ring;
				const auto range = a_slot.range;
				if (Harvest(a_globals, *ring, range, false))
					return a_slot.ready ? a_slot.value : kNotReady;
				if (!a_wait)
					return kNotReady;
				// Only wait once the host sealed the run. Before that it may sit in DXVK's stream behind this very frame, and the wait would never end.
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
			}
		};
	};

	void Install()
	{
		// With only CS_ORG_OCCLUSION_QUERY_LOG=1, log D3D11's answers for comparison.
		if (!Engine::GraphMode()) {
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
		if (!Hooks::BeginQuery::func.address() || !Hooks::EndQuery::func.address() || !Hooks::GetQueryData::func.address())
			RenderGraph::Fatal("occlusion queries", "a detour failed");
		Get().active = true;
		logger::info("[ORG] occlusion queries: on: the queries a port brackets are answered by the graph");
	}

	Bracket::Bracket()
	{
		outer = current;
		current = this;
	}

	Bracket::Bracket(std::uint32_t a_skipAnswer) :
		Bracket()
	{
		hasSkipAnswer = true;
		skipAnswer = a_skipAnswer;
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
		bool submitted = false;
		// A stage run is submitted at the stage's commit, after the bracket. Its count comes then, or the slot skips when the lease comes round.
		if (entry.ring) {
			const auto& run = entry.ring->runs[entry.range];
			submitted = run.generation == entry.generation && run.committed;
		}
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

	void Bracket::Answer(std::uint32_t a_count)
	{
		if (slot == kNoSlot)
			return;
		auto& globals = Get();
		const std::scoped_lock lock(globals.mutex);
		auto& entry = globals.slots[slot];
		// The member hides the file's Answer helper.
		if (entry.graph && !entry.ready)
			Deliver(entry, (std::min)(a_count, kNotReady - 1));
	}

	std::string Status()
	{
		if (!Active())
			return Engine::GraphMode() ? "graph mode, not installed" : "off (not graph mode)";
		const auto& c = Get().counters;
		return std::format("{} answers from the graph, {} not-ready polls, {} skipped, {} blocking waits", c.graphAnswers.load(), c.notReady.load(),
			c.skipped.load(), c.waits.load());
	}
}
