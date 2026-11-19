#pragma once

// Buffer view refs for the buffers DCLF names by version.

#include "RenderGraph/BufferRefs.h"

#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/VersionedBuffer.h>

#include <source_location>

namespace DCLF
{
	/**
	 * @brief Any thread. A named version's buffer view ref, or null without a buffer or graph host.
	 * Every pass that reads one must declare the named buffer, so the ref is never a raw access.
	 */
	inline RenderGraph::BufferRef RefOf(const org::services::NamedEntry& a_entry, std::source_location a_where = std::source_location::current())
	{
		return a_entry.version && a_entry.version->buffer ? RenderGraph::DeclaredRefOf(*a_entry.version->buffer, 0, a_where) : RenderGraph::BufferRef{};
	}
}
