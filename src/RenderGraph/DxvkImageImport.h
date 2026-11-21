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
	*  A texture resting in GENERAL is Common-only, except in native mode. Registers it in ImageRegistry as an import, so a second
	*  live graph resource of the image is fatal. The caller retires it (ImageRegistry::Retire) before the image can go. */
	std::shared_ptr<org::ExternalTextureResource> ImportDxvkTexture(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image,
		org::TextureDescription a_desc, const char* a_name);

	/** @brief Whether raw work on graph-tracked images must take their layouts from the ledgers. True in native mode. Any thread. */
	bool NativeGraphMovesImages();

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
