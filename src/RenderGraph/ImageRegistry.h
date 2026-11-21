#pragma once

// One registry of the graph resource of each image (cleanup plan B15, phase 3 design 4.2).
// An image has at most one live graph resource, since each resource tracks the image's layout and two would race.
// Registering a second live resource for one image is fatal. Owners retire their resource before its image can go,
// so a reused image handle never finds an old resource.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct IUnknown;

namespace org
{
	class Resource;
	struct TextureDescription;
	class VersionedTexture;
}

/** @brief Any thread. One lock. Entries hold their resource and slot weakly, so the owners decide their lives. */
namespace RenderGraph::ImageRegistry
{
	/** @brief Who made the resource, which decides who may take it. */
	enum class Kind : std::uint8_t
	{
		Import,  // a graph import of an image another owner made (frame anchors, DCLF). Found by its slot, or by ledger lookups in native mode
		Made,    // an image ORG made and handed to D3D11 (a twin's export, an engine or streamed texture, a native exempt texture). Importers take it
		Owned,   // a native graph-owned texture (Native::NoteOwnedImage)
	};

	struct Entry
	{
		std::shared_ptr<org::Resource> resource;
		std::shared_ptr<org::VersionedTexture> slot;  // the slot whose version the resource is (a versioned twin's, an import's), else null
		std::shared_ptr<const org::TextureDescription> views;  // the views the resource has; null when the registrant did not say
		std::string owner;  // who registered it, for the logs: "GraphResources", "native", "frame anchors"
		std::string name;   // the resource's name, for the logs
		Kind kind = Kind::Import;
	};

	/**
	* @brief Registers a_entry.resource as a_image's graph resource. Registering the same resource again updates its entry: a slot,
	* views, owner or name given replace the old ones, and a kind other than Import replaces the old kind. A second live resource
	* for one image is fatal. Any thread.
	*/
	void Register(std::uint64_t a_image, Entry a_entry);

	/** @brief Sets the slot of a registered resource; null clears it. Nothing when a_resource is not registered. Any thread. */
	void SetSlot(const org::Resource* a_resource, std::shared_ptr<org::VersionedTexture> a_slot);

	/** @brief a_image's live entry, or nothing. Any thread. */
	std::optional<Entry> Find(std::uint64_t a_image);

	/**
	* @brief The live entry of the image behind a D3D11 texture or tripwire handle, described through RenderGraphRuntime::DescribeResource
	* (dxvkGetInteropResourceInfo, or the native record in native mode). Describing synchronizes with DXVK's worker, so resolve once.
	* Any thread.
	*/
	std::optional<Entry> Find(const IUnknown* a_d3d11);

	/** @brief Forgets a_resource. Frames may still finish with it, but no lookup finds it. Call it before its image can go. Any thread. */
	void Retire(const org::Resource* a_resource);
}
