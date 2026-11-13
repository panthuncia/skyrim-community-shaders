#pragma once

// Buffer view refs (RenderGraph/BufferRef.h) of the buffers DCLF names by version: what its shaders read through.

#include "RenderGraph/BufferRefs.h"

#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/VersionedBuffer.h>

#include <source_location>

namespace DCLF
{
	/**
	 * @brief A named version's buffer view ref (at its first byte); null without a buffer or a graph host. Any thread.
	 *
	 * Every pass reading these refs declares the named buffer (no views), and the version the snapshot seals is the one its
	 * declaration binds for that frame:
	 * - the face lists: the view epochs' face passes (ViewEpochRecord's DrawFamilyDeclare, TreeLodFaces);
	 * - the vertex stream: the main view, view epoch, shadow, forward and CPU site passes;
	 * - grass's instances, owners, frame, visible, shapes, buckets and meshes: its cull and arguments passes (GrassPasses) and its main
	 *   draws (GrassDraws' MainDeclare);
	 * - tree LOD's records, shapes, meshes and visible table: its passes and its draws.
	 * So the ref is not a raw access (RenderGraph::DeclaredRefOf). Phase 4 writes it in Prepare from the declaration.
	 */
	inline RenderGraph::BufferRef RefOf(const org::services::NamedEntry& a_entry, std::source_location a_where = std::source_location::current())
	{
		return a_entry.version && a_entry.version->buffer ? RenderGraph::DeclaredRefOf(*a_entry.version->buffer, 0, a_where) : RenderGraph::BufferRef{};
	}
}
