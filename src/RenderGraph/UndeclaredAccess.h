#pragma once

#include "RenderGraph/Switches.h"

#include <cstdint>
#include <memory>
#include <source_location>
#include <string_view>

struct ID3D11Buffer;
struct ID3D11ShaderResourceView;

namespace org
{
	class Resource;
}

/**
 * @brief The undeclared-access validator (native frame graph plan, Phase 0), CS_ORG_VALIDATE_UNDECLARED=1: logs every raw bindless
 * index or buffer ref CS hands to a shader for a resource that is not exempt from tracking. Log only: it never throws or refuses.
 *
 * A raw index bypasses the declarations: it comes from a second import (DCLF::GpuTextures::ResolveBinding, DCLF::GpuResources::Acquire),
 * from PublishedResources::ShaderResourceIndex after a view-less declare, or from a buffer's ref (BufferRefOf). So no pass declared the
 * access through the token it used, and the graph neither transitions the resource nor orders it after its writers. Every such access
 * to a resource the graph must track is Phase 1's work; the validator's log is that worklist, and Phase 1 is done when it is empty.
 *
 * Exempt (not reported), as BasicRenderer's GraphOwnership::ExternalImmutableShaderResource, approximately:
 * - an ORG resource marked ExternalImmutableShaderResource (org::RequireUndeclaredShaderRead's rule, without its throw);
 * - an ORG resource its creator noted immutable after creation (NoteImmutable): an engine DDS texture made with its contents and
 *   bound SRV-only, a static engine vertex or index buffer filled at creation. In native mode the write census promotes it back: one
 *   the immediate context wrote since is reported. ORG's graphs refuse an exempt ownership, so these keep GraphManaged until Phase 2;
 *   the note is the validator's only;
 * - a texture made with shader-resource binding only, and a buffer made with vertex or index binding only, that is IMMUTABLE, or in
 *   native mode was made with initial data and never written since (Native::ResourceFacts; a streaming shell is its window's), or in
 *   guest mode (where the initial data is not known) is DEFAULT.
 * Everything else is reported: ORG resources (graph-managed), other engine resources (the dynamic ring, render targets, written
 * textures), published twins and CS buffers by handle.
 *
 * Interim declared reads: a ShaderResourceIndex lookup made inside a DeclaredIndexScope (the reader checked that its own pass declared
 * the id: DCLF's DeclarePublishedRead and the readers of its declared-reads registries) is counted apart, as "declared (interim
 * index)": the graph orders the read, only the index does not come from the declared view yet (Phase 4 moves the tables into Prepare).
 *
 * Attribution: these sites resolve outside pass recording, mostly at capture. Each access is attributed to the graph epoch running on
 * the thread at the time (EpochScope: an anchor run's commit or a DCLF segment's inputs), else to capture on the render thread, else
 * to another thread (scene workers, the import thread, a pass recorded on the graph host's thread). The call site is the caller's
 * source location.
 *
 * Logs the first access of each site and resource (the first CS_ORG_VALIDATE_UNDECLARED_LOG, default 256), then a summary per window
 * (CensusWindow) every CS_ORG_CENSUS_FRAMES frames and when the game leaves it.
 */
namespace RenderGraph::UndeclaredAccess
{
	/** @brief Whether CS_ORG_VALIDATE_UNDECLARED=1. Any thread; a static after the first call. */
	inline bool On()
	{
		static const bool on = Switches::On("CS_ORG_VALIDATE_UNDECLARED");
		return on;
	}

	/** @brief A raw index of a_view's resource, made by a_what ("ResolveBinding material t0") at a_where. Any thread. */
	void NoteView(ID3D11ShaderResourceView* a_view, std::string_view a_what, std::source_location a_where);

	/** @brief A raw ref of a_buffer (a D3D11 buffer, the engine's or CS's), made at a_where. Any thread. */
	void NoteBuffer(ID3D11Buffer* a_buffer, std::string_view a_what, std::source_location a_where);

	/** @brief A raw index or ref of an ORG resource, made at a_where. Exempt when it is ExternalImmutableShaderResource. Any thread. */
	void NoteResource(const org::Resource& a_resource, std::string_view a_what, std::source_location a_where);

	/** @brief A raw index of the published twin a_id (PublishedResources::ShaderResourceIndex), at a_where. Any thread. */
	void NotePublished(std::string_view a_id, std::source_location a_where);

	/** @brief A raw ref of a BasicRHI buffer by its handle (a CS buffer; the graph's resource is not known here), at a_where. Any thread. */
	void NoteRhiBuffer(std::uint32_t a_index, std::uint32_t a_generation, std::source_location a_where);

	/**
	 * @brief Notes that a_resource is immutable after creation: made with its contents, readable only (a texture bound SRV-only, a
	 * buffer bound VB/IB-only), and written by nothing after. Its creator calls it, only while On(). Any thread.
	 */
	void NoteImmutable(const std::shared_ptr<org::Resource>& a_resource);

	/**
	 * @brief For the scope, ShaderResourceIndex lookups on this thread are reads the reader declared (it checked its own declared ids
	 * first): they count as "declared (interim index)", not as undeclared. Cheap: one thread-local flag.
	 */
	class DeclaredIndexScope
	{
	public:
		DeclaredIndexScope();
		~DeclaredIndexScope();
		DeclaredIndexScope(const DeclaredIndexScope&) = delete;
		DeclaredIndexScope& operator=(const DeclaredIndexScope&) = delete;

	private:
		bool previous = false;
	};

	/** @brief The epoch a_label runs on this thread for the scope (RenderGraphRuntime::TryExecuteEpoch): accesses meanwhile are its. */
	class EpochScope
	{
	public:
		explicit EpochScope(const char* a_label);
		~EpochScope();
		EpochScope(const EpochScope&) = delete;
		EpochScope& operator=(const EpochScope&) = delete;

	private:
		const char* previous = nullptr;
	};

	/** @brief The frame's end (RenderGraphRuntime::SealPresented): reports a window when due. */
	void EndFrame();
}
