#pragma once

// Imports DXVK's images into BasicRHI and the render graph in the layout DXVK rests them in.
// An image resting in GENERAL is shared with simultaneous access. Any other is used exclusively, and the graph's
// home states give it back in that layout after each execution.
// Native mode imports every image exclusively and has no home states. The ledgers carry each image's layout between executions,
// which needs exactly one graph resource per image. Raw imports stay shared in GENERAL.

#include "RenderGraph/BufferRef.h"
#include "RenderGraph/DxvkOrgInterop.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

#include <rhi.h>

namespace org
{
	class ExternalTextureResource;
	class Resource;
	struct TextureDescription;
	class VersionedTexture;
}

namespace RenderGraph
{
	/** @brief The graph layout of a layout DXVK rests images in, or nothing for one the graph has no equivalent of. */
	std::optional<rhi::ResourceLayout> HomeLayoutOf(VkImageLayout a_layout);

	/** @brief Wraps a DXVK image for BasicRHI in its resting layout. Null when the layout has no equivalent or the import fails. */
	rhi::ResourcePtr ImportDxvkImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, const char* a_name);

	/** @brief D3D12 native mode. A BasicRHI resource over the ID3D12Resource a description carries. Null on failure. */
	rhi::ResourcePtr ImportD3D12Resource(rhi::Device a_device, void* a_resource, const char* a_name);

	/** @brief Imports a DXVK image as a graph texture whose home state is its resting layout. Sets a_desc.initialLayout.
	*  A texture resting in GENERAL is Common-only, except in native mode. */
	std::shared_ptr<org::ExternalTextureResource> ImportDxvkTexture(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image,
		org::TextureDescription a_desc, const char* a_name);

	/** @brief The duplicate-image check, CS_ORG_IMAGE_DUPLICATE_CHECK=1. Logs when two live graph resources track one image,
	*  since their layout moves would race. Cheap when off. Any thread. */
	void NoteImageResource(std::uint64_t a_image, const std::shared_ptr<org::Resource>& a_resource, std::string_view a_name);

	/** @brief The duplicate-image check. a_resource is no longer its image's graph resource, though frames may still finish with it. */
	void RetireImageResource(const org::Resource* a_resource);

	/** @brief An image ORG made that has a graph resource of its own (MadeImageOf). */
	struct MadeImage
	{
		std::shared_ptr<org::Resource> resource;               // the image's graph resource
		std::shared_ptr<org::VersionedTexture> slot;           // a versioned twin's slot, whose current version is resource; else null
		std::shared_ptr<const org::TextureDescription> views;  // the views resource has
		std::string name;                                      // its maker's name for it, for the logs
	};

	/** @brief Registers the graph resource of an image ORG made and handed to D3D11. Imports then return it instead of importing again.
	*  Any thread. */
	void RegisterMadeImage(std::uint64_t a_image, const std::shared_ptr<org::Resource>& a_resource, const org::TextureDescription& a_views,
		std::string_view a_name);

	/** @brief Sets a versioned twin's slot for its registered resource, so imports return the slot. Any thread. */
	void SetMadeImageSlot(const org::Resource* a_resource, const std::shared_ptr<org::VersionedTexture>& a_slot);

	/** @brief Forgets a registered resource before its image can go, so a reused handle never maps to it. Any thread. */
	void ForgetMadeImage(const org::Resource* a_resource);

	/** @brief The registered resource of a_image, or an empty MadeImage. Any thread. */
	MadeImage MadeImageOf(std::uint64_t a_image);

	/** @brief Whether raw work on graph-tracked images must take their layouts from the ledgers. True in native mode. Any thread. */
	bool NativeGraphMovesImages();

	/** @brief The graph resource whose ledger follows a_image, or null when no graph tracks it. Any thread. */
	std::shared_ptr<org::Resource> GraphResourceOfImage(std::uint64_t a_image);

	/** @brief The raw access check, CS_ORG_RAW_ACCESS_CHECK=1. Counts work on tracked images that bypasses graph state tracking. */
	namespace RawAccess
	{
		enum class Kind : std::uint32_t
		{
			ContextTracked,  // the native context's own path on an image the graph tracks
			ContextRace, // the same, where the graph may hold it in another layout
			ViewGeneral,     // a raw GENERAL view (Drawcall Limit Fix) of an image the graph moves between layouts
			ViewFromGraph,   // such a view made on the graph resource instead, in the read-only layout
			Routed,          // the native context's work on a tracked image sent through ORG's external client
			Count
		};

		/** @brief Whether the check is on. Any thread. */
		bool On();

		/** @brief Counts one access of a_kind. Any thread. */
		void Count(Kind a_kind);

		/** @brief Whether to log a line for a_key. True once per key, for the first 64 keys. Any thread. */
		bool LogOnce(std::uint64_t a_key);

		/** @brief Logs the counts when they changed since the last call (native Present calls it every 600 frames). */
		void Summary();
	}

	BufferRef ImportBufferRef(rhi::Device a_device, std::uint64_t a_buffer, std::uint64_t a_offset, std::uint64_t a_bytes, std::uint64_t a_address,
		std::shared_ptr<const void>& a_keep, std::source_location a_where = std::source_location::current());
}
