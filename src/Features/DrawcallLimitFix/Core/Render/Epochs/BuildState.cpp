#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "RenderGraph/BuildManager.h"

#include <atomic>

namespace DCLF::Draws
{
	namespace
	{
		BuildStates inputs;
	}

	BuildStates& BuildStateInputs()
	{
		return inputs;
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

	const GraphConditions& BuildState::Conditions() const
	{
		static const GraphConditions kDefaults;
		return conditions ? *conditions : kDefaults;
	}

	std::shared_ptr<const BuildState> BuildingState(const char* a_who)
	{
		// The build manager took this value for the build on the render thread. Only DCLF gives the manager its input.
		auto state = std::static_pointer_cast<const BuildState>(RenderGraph::Builds::Input());
		if (!state) {
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged.fetch_add(1, std::memory_order_relaxed) < 20)
				logger::warn("[DCLF] {}: made outside a graph build, or before DCLF gave the build manager its build state; it has no passes", a_who);
		}
		return state;
	}

	void PublishAttachedBuildState(std::shared_ptr<const BuildState> a_state)
	{
		if (!a_state)
			return;
		const auto conditions = a_state->conditions;
		inputs.PublishAttached(std::move(a_state));
		if (conditions)
			AttachGraphConditions(conditions);
	}

	std::shared_ptr<const BuildState> AttachedBuildState()
	{
		return inputs.Attached();
	}

	std::uint64_t AttachedBuildSerial()
	{
		return inputs.AttachedSerial();
	}

	void PublishBuiltBuildState(std::uint64_t a_serial)
	{
		inputs.PublishBuilt(a_serial);
	}

	std::uint64_t BuiltBuildSerial()
	{
		return inputs.BuiltSerial();
	}
}
