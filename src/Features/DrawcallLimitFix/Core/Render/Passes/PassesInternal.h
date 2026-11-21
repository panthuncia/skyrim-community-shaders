#pragma once

// The passes Core registers, made by the sources that define them. Included only by this folder's sources, after Internal.h.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"

namespace DCLF::Draws
{
	// a_phase 0 follows the segment's shape, 2 is the phase 2 retest. a_conditions are those of the build that makes it.
	std::shared_ptr<org::RenderPass> MakeBuildDrawsPass(std::shared_ptr<Resources> a_resources, const GraphConditions& a_conditions, RenderGraphRuntime::Segment a_segment,
		std::uint32_t a_phase = 0, bool a_queries = false);
	std::shared_ptr<org::RenderPass> MakeSortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment);

	std::shared_ptr<org::RenderPass> MakeVisibilityEventsPass(std::shared_ptr<Resources> a_resources);


	std::vector<LatchedTarget> MainLatchedTargets(const Resources& a_resources);
}
