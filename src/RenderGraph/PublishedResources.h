#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace GraphResources
{
	class Resource;
	class Texture2D;
}

/**
 * @brief Graph resources a producer shares with the passes of every epoch: published resources (docs/development/render-graph.md,
 * "Publishing a graph resource to other epochs").
 *
 * A feature on its ORG path writes its output in an anchor epoch (FrameAnchors). A client's registerResources registers its twins with
 * the anchor graphs only, so no other epoch's graph can name them. Publishing a twin registers it under its id with every epoch's graph
 * the host builds: the anchors' and Drawcall Limit Fix's alike. A pass of any epoch then declares a read of it by that id, and ORG's one
 * sequencer orders the read after the producer's write, whatever epoch each runs in. Nothing is copied: every graph sees the same
 * resource.
 *
 * Rules:
 * - Only a resource the graph never aliases is published (GraphResources::Resource::Aliasable false: History, Persistent, or exported).
 *   An aliased transient keeps no contents across executions, and another epoch's read is another execution.
 * - A reader in an epoch earlier in the frame than the producer's reads the previous frame's contents.
 * - The id is the twin's name ("Feature::Name") unless the producer gives another. Readers name the same id.
 * - Publishing a new resource under an id (the first time, or after a Resize) rebuilds every graph, once. A twin's Resize is published
 *   again by Refresh, which the frame anchors call when the resize generation moves: the producer need not publish it again.
 * - Withdraw marks the resource as no longer written: ShaderResourceIndex answers false and readers return to their D3D11 view. It
 *   stays registered, because a graph built earlier may still declare it, until the producer publishes again under that id.
 *
 * Everything here runs on the render thread; graph builds read a snapshot.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
namespace PublishedResources
{
	/**
	 * @brief Publishes a_resource under a_id, registered with every epoch's graph from the next build on. Call after creating the twin and
	 * after every Resize. Again with the same resource: nothing. False, logged, when the twin is empty, aliasable, or the runtime is not
	 * active.
	 */
	bool Publish(std::string_view a_id, const GraphResources::Resource& a_resource);

	/** @brief Publish for a 2D texture, whose shape picks the whole-resource view (ShaderResourceIndex). */
	bool Publish(std::string_view a_id, const GraphResources::Texture2D& a_texture);

	/** @brief Publish under the twin's own name. */
	bool Publish(const GraphResources::Resource& a_resource);
	bool Publish(const GraphResources::Texture2D& a_texture);

	/**
	 * @brief Publishes again every published twin whose graph resource changed since (a Resize), while the twin lives. Render thread;
	 * FrameAnchors::Run calls it when GraphResources::Resource::ResizeGeneration moved.
	 */
	void Refresh();

	/** @brief The producer left its ORG path: readers return to D3D11. The resource stays registered (see the rules). */
	void Withdraw(std::string_view a_id);

	/** @brief Whether a_id is published and not withdrawn. */
	bool IsPublished(std::string_view a_id);

	/**
	 * @brief The bindless index of a_id's whole-resource shader resource view (every mip; a cube's TextureCube view, a 2D array's full
	 * array view, a buffer's structured view), for a shader that reads it from the descriptor heap.
	 * @return False when a_id is not published, withdrawn, or has no such view.
	 */
	bool ShaderResourceIndex(std::string_view a_id, std::uint32_t& a_index);

	/**
	 * @brief The id whose twin exports a_view as its D3D11 SRV, while that id is published; empty otherwise. A diagnostic for readers
	 * that import D3D11 views: a graph pass reading the export through an import of its own is not ordered after the producer's
	 * writes, so it should read the id instead.
	 */
	std::string ExportedId(const ID3D11ShaderResourceView* a_view);
}
