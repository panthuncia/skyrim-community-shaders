#include "Snapshot/Retire.h"

#include "Snapshot/FrameSnapshot.h"
#include "Snapshot/UiCommandStream.h"

#include "RenderGraph/Async/EventQueue.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameClock.h"

#include <array>
#include <atomic>
#include <vector>

namespace Snapshot
{
	namespace
	{
		struct Entry
		{
			std::shared_ptr<const void> object;
			std::uint64_t afterFrame = 0;
		};

		/** @brief One lane: any thread pushes; its one drainer keeps what the GPU still uses. */
		struct LaneState
		{
			RenderGraph::Frame::EventQueue<Entry> queue;
			std::vector<Entry> waiting;  // the drainer's
			std::atomic<std::uint64_t> pushed{ 0 }, retired{ 0 }, mostWaiting{ 0 };
		};

		constexpr std::size_t kLanes = 2;

		// Never destroyed: nothing may release engine objects while the process is torn down.
		std::array<LaneState, kLanes>& Lanes()
		{
			static auto* lanes = new std::array<LaneState, kLanes>;
			return *lanes;
		}

		LaneState& LaneOf(Retire::Lane a_lane) { return Lanes()[static_cast<std::size_t>(a_lane)]; }

		std::atomic<std::int64_t> g_snapshots{ 0 };
		std::atomic<std::int64_t> g_engineRefs{ 0 };

		/** @brief Whether the GPU finished render frame a_frame. Outside graph mode D3D11 keeps what its own work reads, so nothing waits. */
		bool GpuDone(std::uint64_t a_frame)
		{
			static const bool graph = RenderGraph::Engine::GraphMode();
			return a_frame == 0 || !graph || RenderGraph::FrameClock::GpuCompleted() >= a_frame;
		}

		void Drain(LaneState& a_lane)
		{
			a_lane.queue.Drain([&](Entry&& a_entry) { a_lane.waiting.push_back(std::move(a_entry)); });
			const auto before = a_lane.waiting.size();
			std::erase_if(a_lane.waiting, [](const Entry& a_entry) { return GpuDone(a_entry.afterFrame); });
			a_lane.retired.fetch_add(before - a_lane.waiting.size(), std::memory_order_relaxed);
			const std::uint64_t waiting = a_lane.waiting.size();
			for (auto most = a_lane.mostWaiting.load(std::memory_order_relaxed); waiting > most && !a_lane.mostWaiting.compare_exchange_weak(most, waiting);) {}
		}

		void RequireMainThread(const char* a_what)
		{
			if (!FrameAnchors::IsMainThread())
				RenderGraph::Fatal("snapshot lifetimes", fmt::format("{} off the main thread (thread {})", a_what, ::GetCurrentThreadId()));
		}
	}

	namespace Retire
	{
		void Push(Lane a_lane, std::shared_ptr<const void> a_object, std::uint64_t a_afterFrame)
		{
			if (!a_object)
				return;
			auto& lane = LaneOf(a_lane);
			lane.pushed.fetch_add(1, std::memory_order_relaxed);
			lane.queue.Push(Entry{ std::move(a_object), a_afterFrame });
		}

		void AfterThisFrame(Lane a_lane, std::shared_ptr<const void> a_object)
		{
			Push(a_lane, std::move(a_object), RenderGraph::FrameClock::BeginFrame());
		}

		void RenderedSnapshot(std::shared_ptr<const FrameSnapshot>&& a_snapshot, std::uint64_t a_lastFrame)
		{
			if (!a_snapshot)
				return;
			// The render side never renders a snapshot again once it adopted a newer one, so its stream can go now.
			if (a_snapshot->ui)
				Push(Lane::Render, a_snapshot->ui->Take(), a_lastFrame);
			Push(Lane::Engine, std::move(a_snapshot), a_lastFrame);
		}

		void LetGo(std::shared_ptr<const FrameSnapshot>&& a_snapshot)
		{
			// Even on the main thread: the capture runs beside the update's frame jobs, so its engine references wait for the drain.
			Push(Lane::Engine, std::move(a_snapshot), 0);
		}

		void DrainEngine()
		{
			RequireMainThread("the engine lane drained");
			Drain(LaneOf(Lane::Engine));
		}

		void DrainRender()
		{
			Drain(LaneOf(Lane::Render));
		}

		std::string TakeStatsLine()
		{
			const auto lane = [](Lane a_lane) {
				auto& state = LaneOf(a_lane);
				return fmt::format("{} retired, {} queued, at most {} waiting for the GPU", state.retired.exchange(0), state.pushed.exchange(0),
					state.mostWaiting.exchange(0));
			};
			return fmt::format("[Snapshot] lifetimes: {} snapshots alive, {} engine references held; engine lane {}; render lane {}", g_snapshots.load(),
				g_engineRefs.load(), lane(Lane::Engine), lane(Lane::Render));
		}

		LiveCount::LiveCount()
		{
			g_snapshots.fetch_add(1, std::memory_order_relaxed);
		}

		LiveCount::LiveCount(const LiveCount&)
		{
			g_snapshots.fetch_add(1, std::memory_order_relaxed);
		}

		LiveCount::~LiveCount()
		{
			g_snapshots.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	EngineRef::EngineRef(RE::NiRefObject* a_object) :
		object(a_object)
	{
		if (!object)
			return;
		RequireMainThread("an engine reference taken");
		object->IncRefCount();
		g_engineRefs.fetch_add(1, std::memory_order_relaxed);
	}

	EngineRef::EngineRef(const EngineRef& a_other) :
		EngineRef(a_other.object)
	{
	}

	EngineRef& EngineRef::operator=(const EngineRef& a_other)
	{
		if (a_other.object != object) {
			EngineRef copy(a_other);
			*this = std::move(copy);
		}
		return *this;
	}

	EngineRef& EngineRef::operator=(EngineRef&& a_other) noexcept
	{
		if (this != &a_other) {
			Let();
			object = a_other.object;
			a_other.object = nullptr;
		}
		return *this;
	}

	EngineRef::~EngineRef()
	{
		Let();
	}

	void EngineRef::Let()
	{
		if (!object)
			return;
		RequireMainThread("an engine reference let go");
		g_engineRefs.fetch_sub(1, std::memory_order_relaxed);
		std::exchange(object, nullptr)->DecRefCount();
	}
}
