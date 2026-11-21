#pragma once

// The frame facts that decide which passes the graph has. Passes are left out rather than run with nothing to do,
// because each pass costs its dispatch and barriers either way.

#include <memory>
#include <string>
#include <string_view>

namespace org
{
	class GraphHost;
}

namespace DCLF
{
	struct ToggleSet;
}

namespace DCLF::Draws
{
	struct Resources;
	struct PassFrame;

	struct GraphConditions
	{
		bool interior = false;  // interiors have no grass and no tree LOD
		bool twoPassOcclusion = true;  // cull mode 2, which needs the HZB and depth phase 2
		// The CPU-driven pipeline. DCLF epochs keep only their CPU-pipeline passes, and the direct sites do the draws.
		// In guest those epochs are external and the sites run them in DXVK's command buffer. In native the graph submits them.
		// The GPU-driven graph also draws the forward families, first person's included, on the CPU pipeline's forward sites.
		bool cpuPipeline = false;
		bool operator==(const GraphConditions&) const = default;
	};

	/** @brief Whether the device has no counted indirect draws, which makes every cell CPU-driven. */
	bool CpuPipelineForced();

	/** @brief Whether a_set draws a cell of a_interior CPU-driven, or the device forces it. */
	bool CpuPipelineForCell(const ToggleSet& a_set, bool a_interior);

	/**
	 * @brief Main thread, inside the adoption point, after the scene step. Makes the conditions from the adopted values. Nothing before
	 * DCLF's first build point (FirstGraphConditions). A change requests a rebuild of DCLF's epochs here, and frames keep the attached
	 * conditions until a scene recorded on those builds is adopted.
	 */
	void UpdateGraphConditions(org::GraphHost& a_host);

	/**
	 * @brief Render thread, at DCLF's build point. The first one makes the first conditions: at once inside the adoption point (native
	 * mode's build point), else at the next Begin's UpdateGraphConditions (guest mode's, at Main::Draw). Later ones do nothing.
	 */
	void FirstGraphConditions(org::GraphHost& a_host);

	/** @brief Render thread: the conditions the next builds are for. Immutable. */
	std::shared_ptr<const GraphConditions> RequestedGraphConditions();

	/** @brief Render thread: records the conditions the attached graphs were built for. */
	void AttachGraphConditions(std::shared_ptr<const GraphConditions> a_conditions);


	/** @brief Any thread: whether the attached graph is the CPU pipeline's, which decides whether direct sites or epochs draw. */
	bool GraphBuiltForCpu();

	/**
	 * @brief Any thread: the conditions of the attached graphs, which the frame draws with. For frame code only, such as the
	 * direct sites, coverage and diagnostics. A pass reads the conditions of the build that made it (Api::PassContext::Conditions),
	 * never these, so a staged build's recording never mixes two sets. The reference stays valid.
	 */
	const GraphConditions& AttachedGraphConditions();

	/**
	 * @brief Whether a phase 1 cull tests occlusion. Without the HZB pass in the graph the pyramid holds an old frame's depth.
	 * a_conditions are those of the build that made the pass.
	 */
	bool OcclusionTested(const GraphConditions& a_conditions, const Resources& a_resources, const PassFrame& a_frame);
}
