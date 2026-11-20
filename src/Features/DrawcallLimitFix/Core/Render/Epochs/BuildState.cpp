#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace DCLF::Draws
{
	namespace
	{
		thread_local const BuildScope* t_scope = nullptr;
		thread_local const BuildState* t_state = nullptr;

		std::mutex sourceMutex;
		std::function<std::shared_ptr<const BuildState>()> source;  // sourceMutex
		std::atomic<std::thread::id> sourceThread{};  // the render thread

		std::atomic<std::shared_ptr<const BuildState>> attached;
		std::atomic<std::uint64_t> attachedSerial{ 0 };
		std::atomic<std::uint64_t> builtSerial{ 0 };
	}

	ShadowSlotBuffers MakeShadowSlotBuffers(const ShadowResources& a_shadow)
	{
		ShadowSlotBuffers slots;
		slots.drawArgs = a_shadow.drawArgs;
		slots.bucketCounts = a_shadow.bucketCounts;
		slots.drawArgsOwner = a_shadow.drawArgsOwner;
		slots.count = a_shadow.count;
		slots.depth = a_shadow.depth;
		slots.depthLayers = a_shadow.depthLayers;
		return slots;
	}

	bool SameShadowSlotBuffers(const ShadowSlotBuffers& a_slots, const ShadowResources& a_shadow)
	{
		return a_slots.drawArgs == a_shadow.drawArgs && a_slots.bucketCounts == a_shadow.bucketCounts && a_slots.drawArgsOwner == a_shadow.drawArgsOwner &&
		       a_slots.count == a_shadow.count && a_slots.depth == a_shadow.depth && a_slots.depthLayers == a_shadow.depthLayers;
	}

	const ViewBuild* BuildState::ViewOf(const ViewResources* a_view) const
	{
		if (!a_view)
			return nullptr;
		for (const auto& view : views)
			if (view.resources.get() == a_view)
				return &view;
		return nullptr;
	}

	BuildScope::BuildScope(std::shared_ptr<const BuildState> a_state) :
		state(std::move(a_state)), previous(t_scope)
	{
		t_scope = this;
		t_state = state.get();
	}

	BuildScope::~BuildScope()
	{
		t_scope = previous;
		t_state = previous ? previous->state.get() : nullptr;
	}

	const BuildState* ScopedBuildState()
	{
		return t_state;
	}

	void SetBuildStateSource(std::function<std::shared_ptr<const BuildState>()> a_source)
	{
		std::scoped_lock lock(sourceMutex);
		source = std::move(a_source);
		sourceThread.store(std::this_thread::get_id(), std::memory_order_release);
	}

	std::shared_ptr<const BuildState> BuildingState(const char* a_who)
	{
		if (t_scope)
			return t_scope->State();
		// No scope. On the render thread its newest value is consistent. Anywhere else it is a defect, since the render thread
		// may be replacing the value.
		const bool renderThread = std::this_thread::get_id() == sourceThread.load(std::memory_order_acquire);
		static std::atomic<std::uint32_t> logged{ 0 };
		if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
			if (renderThread)
				logger::info(
					"[DCLF] {}: a graph build without a build scope on the render thread (the host's first build of an epoch); it uses the "
					"render thread's newest build state",
					a_who);
			else
				logger::error(
					"[DCLF] {}: a graph build without a build scope off the render thread; it uses the render thread's newest build "
					"state, which may be changing",
					a_who);
		}
		std::function<std::shared_ptr<const BuildState>()> current;
		{
			std::scoped_lock lock(sourceMutex);
			current = source;
		}
		if (renderThread && current)
			return current();
		// The source reads the render thread's state, so off that thread the attached value is the closest.
		return AttachedBuildState();
	}

	void PublishAttachedBuildState(std::shared_ptr<const BuildState> a_state)
	{
		if (!a_state)
			return;
		const auto serial = a_state->serial;
		const auto conditions = a_state->conditions;
		attached.store(std::move(a_state), std::memory_order_release);
		attachedSerial.store(serial, std::memory_order_release);
		if (conditions)
			AttachGraphConditions(conditions);
	}

	std::shared_ptr<const BuildState> AttachedBuildState()
	{
		return attached.load(std::memory_order_acquire);
	}

	std::uint64_t AttachedBuildSerial()
	{
		return attachedSerial.load(std::memory_order_acquire);
	}

	void PublishBuiltBuildState(std::uint64_t a_serial)
	{
		builtSerial.store(a_serial, std::memory_order_release);
	}

	std::uint64_t BuiltBuildSerial()
	{
		return builtSerial.load(std::memory_order_acquire);
	}

	bool AsyncGraphBuilds()
	{
		static const bool on = SwitchOnUnlessZero(Switch::AsyncGraphBuilds);
		return on;
	}
}
