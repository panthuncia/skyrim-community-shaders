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
		bool cpuPipeline = false;
		// The GPU-driven graph sends the forward family through the CPU pipeline's cull, sort and forward site.
		// Only when the epoch records its late pass at execution.
		bool forwardDirect = false;
		// First person's forward part too, while its colour epoch also records at execution.
		bool firstPersonForwardDirect = false;
		// Whether DCLF can draw the whole first-person view. Without it the engine draws the view and its epochs have no passes.
		bool firstPerson = true;
		std::string cuts;  // dclf:cut= diagnostics, comma-separated pass key prefixes, "*" for every pass

		bool operator==(const GraphConditions&) const = default;
	};

	/** @brief Whether the device has no counted indirect draws, which makes every cell CPU-driven. */
	bool CpuPipelineForced();

	/** @brief Whether a_set draws a cell of a_interior CPU-driven, or the device forces it. */
	bool CpuPipelineForCell(const ToggleSet& a_set, bool a_interior);

	/**
	 * @brief Main thread, at install, before DCLF's toggles are first committed. The conditions follow frame variables (plan step 10c):
	 * DclfToggles, Interior and DclfPassCuts. A revision that carries a change of one applies it at its adoption.
	 */
	void RegisterConditionInputs();

	/**
	 * @brief Main thread, inside the adoption point, after the scene step. Makes the conditions again when a revision adopted since the
	 * last call changed one of their variables. Nothing before DCLF's first build point (FirstGraphConditions). A change requests a
	 * rebuild of DCLF's epochs here, and frames keep the attached conditions until a scene recorded on those builds is adopted.
	 */
	void UpdateGraphConditions(org::GraphHost& a_host);

	/**
	 * @brief Render thread, at DCLF's build point. The first one makes the first conditions, as the build point did before step 10c.
	 * Later ones do nothing: the conditions change only at a Begin (UpdateGraphConditions).
	 */
	void FirstGraphConditions(org::GraphHost& a_host);

	/** @brief Render thread: the conditions the next builds are for. Immutable. */
	std::shared_ptr<const GraphConditions> RequestedGraphConditions();

	/** @brief Render thread: records the conditions the attached graphs were built for. */
	void AttachGraphConditions(std::shared_ptr<const GraphConditions> a_conditions);

	/** @brief Any thread, diagnostics: the pass key prefixes the next conditions leave out. Raises DclfPassCuts. */
	void SetPassCuts(std::string a_cuts);

	/** @brief Whether the graph's conditions leave the pass a_key out. */
	bool PassCut(std::string_view a_key);

	/**
	 * @brief Any thread, diagnostics: the latched-copies passes record no copy. Unsafe with draws since their constants and
	 * counters are not reset. For timing an empty-epoch run only.
	 */
	bool LatchedRecordCut();

	/** @brief Any thread: whether the attached graph is the CPU pipeline's, which decides whether direct sites or epochs draw. */
	bool GraphBuiltForCpu();

	/**
	 * @brief Any thread: inside a graph build, the conditions it builds for, elsewhere the attached graphs' conditions.
	 * The reference stays valid.
	 */
	const GraphConditions& CurrentGraphConditions();

	/** @brief Whether a phase 1 cull tests occlusion. Without the HZB pass in the graph the pyramid holds an old frame's depth. */
	bool OcclusionTested(const Resources& a_resources, const PassFrame& a_frame);
}
