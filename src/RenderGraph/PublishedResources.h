#pragma once

#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>

struct ID3D11ShaderResourceView;

namespace org
{
	class Resource;
}

namespace GraphResources
{
	class Resource;
	class Texture2D;
}

namespace FrameAnchors
{
	class Texture;
}

/**
* @brief Published resources: graph resources a producer shares with the passes of every epoch.
*
* Publishing a twin registers it under its id with every epoch's graph, so a pass in any epoch declares a read by that id and
* ORG's sequencer orders it after the producer's write. Nothing is copied.
* Only resources the graph never aliases may be published. A reader earlier in the frame than the producer reads the previous frame.
* A versioned twin is published as its slot, so its new versions need no build. Publishing never rebuilds a graph that holds the
* id: another resource under a published id is fatal (cleanup plan A12). Publish ids once, at setup, as declared textures whose
* versions follow the owner's settings; an id that is off is one texel, never gone. A dependent reads the owner's adopted
* setting to learn whether it is on, not whether the id is published.
* Everything here runs on the render thread.
*/
namespace PublishedResources
{
	/** @brief Publishes a_resource under a_id for every epoch's graph from the next build on. Call after creating the twin.
	*  False, logged, when the twin is empty, aliasable or the runtime is inactive. */
	bool Publish(std::string_view a_id, const GraphResources::Resource& a_resource);

	/** @brief Publish for a 2D texture, whose shape picks the whole-resource view (ShaderResourceIndex). */
	bool Publish(std::string_view a_id, const GraphResources::Texture2D& a_texture);

	/** @brief Publish under the twin's own name. */
	bool Publish(const GraphResources::Resource& a_resource);
	bool Publish(const GraphResources::Texture2D& a_texture);

	/** @brief Publishes a FrameAnchors import of a D3D11 texture under its import id, so every epoch's readers use the writers' resource.
	*  a_srv is the view readers find bound. */
	bool PublishImport(std::string_view a_id, const FrameAnchors::Texture& a_import, const ID3D11ShaderResourceView* a_srv);

	/** @brief Whether a_id is published. */
	bool IsPublished(std::string_view a_id);

	/** @brief Whether a_id was ever published, so every graph built since registers it. */
	bool IsRegistered(std::string_view a_id);

	/** @brief IsRegistered for a pass's Declare at a build. An unregistered id is remembered, and its first publish rebuilds every graph.
	*  Use this, not IsRegistered, for an id published later. */
	bool IsDeclarable(std::string_view a_id);

	/** @brief Whether a_view is the D3D11 export of a_id's twin while a_id is published. Does not allocate. */
	bool IsExportOf(const ID3D11ShaderResourceView* a_view, std::string_view a_id);

	/** @brief The bindless index of a_id's whole-resource SRV. Not a declaration, so the undeclared-access check raises it at a_where.
	*  False when a_id is not published or has no such view. During a build it reads that graph's snapshot, so any thread. */
	bool ShaderResourceIndex(std::string_view a_id, std::uint32_t& a_index, std::source_location a_where = std::source_location::current());

	/**
	* @brief Whether a_id's resolver is a versioned slot (a texture's or a buffer's), whose descriptor follows its versions. A build's Declare reads the
	* snapshot that graph registers, so any thread then; otherwise the render thread.
	*/
	bool IsVersioned(std::string_view a_id);

	/** @brief The view variant ShaderResourceIndex hands out for a_id, so a declaration covers the descriptor it reads.
	*  UINT32_MAX for a primary view or an unknown id. Render thread, or a build on any thread (its snapshot). */
	std::uint32_t ShaderResourceVariant(std::string_view a_id);

	/** @brief The id whose twin exports a_view, or empty. A pass reading the export through its own import is not ordered after the producer.
	*  Allocates, so use IsExportOf on hot paths. */
	std::string ExportedId(const ID3D11ShaderResourceView* a_view);

	/** @brief Why ShaderResourceIndex(a_id) finds an index or not, for a refusal's log: the entry, its resource and its views. */
	std::string DescribeIndex(std::string_view a_id);
	/** @brief Whether a_view is a published resource's export. */
	bool IsExport(const ID3D11ShaderResourceView* a_view);
}
