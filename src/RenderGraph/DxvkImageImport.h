#pragma once

// Importing DXVK's images into BasicRHI and the render graph, one rule for all of them (DXVK_ORG_INTEROP_RESTING_LAYOUTS): an image
// is imported in the layout DXVK rests it in. One resting in GENERAL (a storage image, or any image on a device with unified
// layouts, or every image when DXVK pins them for an older client) is shared with simultaneous access; any other is used
// exclusively, and the graph's external home states give it back in that layout after each execution.
//
// With CS_ORG_NATIVE_EXCLUSIVE_IMPORTS=1, native mode (with CS_ORG_NATIVE_OWNED) imports every image for the graph exclusively: the graph moves it into
// each pass's layout and gives it back in its home. That needs one graph resource per image (FrameAnchors' imports find an image's
// slot before making one), since two resources would each track the image's layout. Shared imports stay the default
// until the duplicate image check (CS_ORG_IMAGE_DUPLICATE_CHECK) is clean. Raw imports (ImportDxvkImage: the native context's own, Drawcall Limit Fix's bindless views) stay shared in GENERAL, where
// the context and the raw views use the image between graph executions.

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

	/**
	 * @brief Wraps a DXVK image for BasicRHI in the layout it rests in (simultaneous access only when that is GENERAL).
	 * @return Null when the layout has no BasicRHI equivalent or the import fails.
	 */
	rhi::ResourcePtr ImportDxvkImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, const char* a_name);

	/**
	 * @brief D3D12 (native mode): a BasicRHI resource over the ID3D12Resource a description carries where the VkImage or VkBuffer
	 * goes (Native::DescribeResource). Null on failure.
	 */
	rhi::ResourcePtr ImportD3D12Resource(rhi::Device a_device, void* a_resource, const char* a_name);

	/**
	 * @brief Imports a DXVK image as a graph texture whose home state is the layout it rests in.
	 *
	 * Sets a_desc.initialLayout from the image. A texture resting in GENERAL is Common-only (except in native mode, see above); any
	 * other uses the layouts its passes declare. Noted for the duplicate check (NoteImageResource).
	 */
	std::shared_ptr<org::ExternalTextureResource> ImportDxvkTexture(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image,
		org::TextureDescription a_desc, const char* a_name);

	/**
	 * @brief A buffer view ref into a described buffer range (DXVK's, or native mode's: Native::DescribeResource), for shaders that read
	 * it (Common/DeviceRef.hlsli). a_buffer is the VkBuffer (D3D12: the ID3D12Resource in its place), a_offset and a_bytes the range in
	 * it. The buffer is imported into BasicRHI once and shared by every range of it, so a DXVK chunk's buffers share one raw view pair;
	 * on Vulkan a range past what the import covers imports it again, larger. a_keep holds the import, and with it the pair, for as long
	 * as the caller reads the range. a_address is the range's device address from the description (0: unknown): a cached import whose
	 * buffer starts elsewhere is another buffer's, whose handle value this one reuses, and is imported again. Null ref when the import
	 * fails. Any thread.
	 */
	/**
	 * @brief The duplicate-image check (CS_ORG_IMAGE_DUPLICATE_CHECK=1): notes that a_resource is a graph resource of a_image (a
	 * VkImage, or the ID3D12Resource on D3D12) and logs when another live graph resource of the same image was noted and not retired.
	 * Two such resources track the image's state apart, and their layout moves race. ImportDxvkTexture notes its imports; native mode
	 * notes the textures it makes. Noting a retired resource again makes it current again. Cheap when off. Any thread.
	 */
	void NoteImageResource(std::uint64_t a_image, const std::shared_ptr<org::Resource>& a_resource, std::string_view a_name);

	/**
	 * @brief The duplicate-image check: a_resource is no longer its image's graph resource (a slot replaced it by a version of the same
	 * image, or an import stands for it), though frames may still finish with it. Any thread.
	 */
	void RetireImageResource(const org::Resource* a_resource);

	/** @brief An image ORG made that has a graph resource of its own (MadeImageOf). */
	struct MadeImage
	{
		std::shared_ptr<org::Resource> resource;               // the image's graph resource
		std::shared_ptr<org::VersionedTexture> slot;           // a versioned twin's slot, whose current version is resource; else null
		std::shared_ptr<const org::TextureDescription> views;  // the views resource has
		std::string name;                                      // its maker's name for it, for the logs
	};

	/**
	 * @brief Notes that a_resource is the graph resource of a_image, an image ORG made and handed to D3D11: a GraphResources twin's
	 * export (GraphResources::ExportTexture), an engine texture (Engine::Textures), a streaming window. a_views are the views it has.
	 * FrameAnchors' imports look the image up (MadeImageOf) and return this resource instead of importing the image again, so the
	 * image stays one graph resource. Any thread.
	 */
	void RegisterMadeImage(std::uint64_t a_image, const std::shared_ptr<org::Resource>& a_resource, const org::TextureDescription& a_views,
		std::string_view a_name);

	/** @brief A versioned twin's slot for its registered resource (RegisterMadeImage): imports then return the slot. Any thread. */
	void SetMadeImageSlot(const org::Resource* a_resource, const std::shared_ptr<org::VersionedTexture>& a_slot);

	/**
	 * @brief Forgets a registered resource. Its maker calls it before the image can go while something else still holds the resource
	 * (an import that does not own its image), so a new image with the same handle is never answered with it. Any thread.
	 */
	void ForgetMadeImage(const org::Resource* a_resource);

	/** @brief The registered resource of a_image, or an empty MadeImage. Any thread. */
	MadeImage MadeImageOf(std::uint64_t a_image);

	BufferRef ImportBufferRef(rhi::Device a_device, std::uint64_t a_buffer, std::uint64_t a_offset, std::uint64_t a_bytes, std::uint64_t a_address,
		std::shared_ptr<const void>& a_keep, std::source_location a_where = std::source_location::current());
}
