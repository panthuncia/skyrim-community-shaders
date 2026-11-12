#pragma once

// Buffer view refs (RenderGraph/BufferRef.h) of the buffers DCLF names by version: what its shaders read through.

#include "RenderGraph/BufferRefs.h"

#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/VersionedBuffer.h>

namespace DCLF
{
	/** @brief A named version's buffer view ref (at its first byte); null without a buffer or a graph host. Any thread. */
	inline RenderGraph::BufferRef RefOf(const org::services::NamedEntry& a_entry)
	{
		return a_entry.version && a_entry.version->buffer ? RenderGraph::BufferRefOf(*a_entry.version->buffer) : RenderGraph::BufferRef{};
	}
}
