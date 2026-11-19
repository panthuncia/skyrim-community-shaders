#pragma once

// The schedule's executor. It holds every registered pass, resolves the frame's schedule against them once at install,
// runs the passes' setups, and adds each graph extension's epoch passes at the graph's build.

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Api/Views.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <Render/RenderGraph/RenderGraph.h>
#include <rhi.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace DCLF::Draws
{
	struct SceneBuffers;

	/** @brief Main thread, during InstallFeatures: registers a copy of a_pass. Fails the install on a missing or duplicate key or a bad shape. */
	void RegisterPassDef(const Api::PassDef& a_pass, std::string_view a_who);
	/** @brief Main thread, before the features install: registers Core's passes. */
	void RegisterCorePasses();
	/**
	 * @brief Main thread, once at the end of InstallFeatures: resolves the epochs, the frame's orders and each epoch's pass keys.
	 * Fails the install on any inconsistency in the schedule, naming the key or epoch at fault.
	 */
	void ResolveSchedule(const Api::FrameSchedule& a_schedule);
	/** @brief Main thread, during InstallFeatures: the row of trigger a_id. Fails the install, naming a_who, for an unknown id. */
	Api::TriggerId FindTrigger(const Api::FrameSchedule& a_schedule, std::string_view a_id, std::string_view a_who);

	/** @brief Core's trigger points, resolved by id at ResolveSchedule. A schedule without one fails the install. */
	enum class CoreTrigger : std::uint32_t
	{
		DepthCommitted,
		Count,
	};
	/** @brief Any thread, after InstallFeatures: Core's trigger a_trigger. */
	Api::TriggerId CoreTriggerOf(CoreTrigger a_trigger);
	/** @brief Any thread, after InstallFeatures: epoch a_epoch's trigger row. */
	std::uint32_t EpochTrigger(std::uint32_t a_epoch);
	/** @brief Any thread, after InstallFeatures: trigger row a_trigger's id, "none" for Extensions::kNoTrigger. */
	std::string_view TriggerName(std::uint32_t a_trigger);
	/** @brief Render thread, after every table: runs each pass's setup in registration order. Null where a pass has none. */
	std::vector<std::shared_ptr<const void>> SetUpPassStates(rhi::Device a_device, const SceneBuffers& a_scene);
	/** @brief Render thread: the depth segment's latch stage word, 0 for none. */
	std::uint32_t PassLatchWord();
	/** @brief Render thread: the frame's copy of the animated material rows, 0 for none. */
	Api::FrameRows PassAnimatedRows();
	/** @brief Render thread: the frame rows of the pass that has them, 0 for none. */
	Api::FrameRows PassFrameRows();
	/**
	 * @brief Render thread, at the graph's build and never per frame: makes a_shape's epoch passes in schedule order. A Main
	 * epoch also makes its hosted views' passes. Only a_graphEpoch's passes are made, UINT32_MAX for every epoch.
	 */
	void GatherScheduled(Api::EpochShape a_shape, Api::EngineView a_view, const Api::PassContext& a_base, std::uint32_t a_graphEpoch,
		std::vector<org::RenderGraph::ExternalPassDesc>& a_out);
	/** @brief Render thread: the graph epochs an extension gathering a_shape is made into, including the Main epochs hosting a view. */
	std::vector<std::uint32_t> ExtensionEpochs(Api::EpochShape a_shape, Api::EngineView a_view = {});
	/** @brief Any thread, after InstallFeatures: whether epoch row a_epoch has CPU pipeline passes, which its site then executes. */
	bool CpuPipelineRecords(std::uint32_t a_epoch);
	/** @brief Any thread, after InstallFeatures: whether an epoch of a_segment has CPU pipeline passes. */
	bool CpuPipelineSegmentRecords(RenderGraphRuntime::Segment a_segment);
	/** @brief Any thread, after InstallFeatures: the ORG epochs that are external in the CPU pipeline. */
	std::vector<std::uint32_t> CpuPipelineEpochs();
}
