#pragma once

// CS_DCLF_SCHEDULE_DUMP=1 logs each epoch's passes, with name, type and queue in add order, whenever the list changes.
// tools/dclf-schedule-diff.py compares the logs to prove a schedule change left every epoch's list alone.

#include <Render/RenderGraph/RenderGraph.h>

#include <memory>
#include <string_view>

namespace DCLF::Diagnostics
{
	/** @brief a_extension as it is, or wrapped so each gather logs its epochs' changed pass lists when the dump is on. */
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> ScheduleDumped(std::string_view a_extensionId,
		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> a_extension);
}
