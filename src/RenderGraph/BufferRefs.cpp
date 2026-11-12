// Buffer view refs from the graph host's device (BufferRefs.h). Built with OpenRenderGraph only: nothing else includes the header.

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/BufferRefs.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Resources/Buffers/Buffer.h>

namespace RenderGraph
{
	BufferRef BufferRefOf(rhi::ResourceHandle a_buffer, std::uint64_t a_offset)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		return host ? BufferRefOf(host->Device(), a_buffer, a_offset) : BufferRef{};
	}

	BufferRef BufferRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset)
	{
		return BufferRefOf(const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), a_offset);
	}
}

#endif
