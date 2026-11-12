// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/DxvkImageImport.h"

#include "RenderGraph/Backend.h"

#include <Resources/ExternalTextureResource.h>
#include <Resources/TextureDescription.h>

// BasicRHI's D3D12 import (rhi_interop_dx12.h), declared here: that header brings DirectX-Headers' d3d12.h, which this translation
// unit's Windows SDK headers would meet.
struct ID3D12Resource;
namespace rhi::dx12
{
	Result import_resource(rhi::Device device, ID3D12Resource* resource, rhi::ResourcePtr& out) noexcept;
}

namespace RenderGraph
{
	std::optional<rhi::ResourceLayout> HomeLayoutOf(VkImageLayout a_layout)
	{
		switch (a_layout) {
		case VK_IMAGE_LAYOUT_GENERAL:
			return rhi::ResourceLayout::Common;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return rhi::ResourceLayout::ShaderResource;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return rhi::ResourceLayout::RenderTarget;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return rhi::ResourceLayout::DepthReadWrite;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			return rhi::ResourceLayout::DepthRead;
		default:
			return std::nullopt;
		}
	}

	rhi::ResourcePtr ImportDxvkImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, const char* a_name)
	{
		if (!HomeLayoutOf(a_image.layout))
			return {};
		// D3D12 (native mode): the description carries the ID3D12Resource where the VkImage goes (Native::DescribeResource).
		if (Backend::IsD3D12()) {
			rhi::ResourcePtr resource;
			if (rhi::dx12::import_resource(a_device, reinterpret_cast<ID3D12Resource*>(a_image.image), resource) != rhi::Result::Ok)
				return {};
			if (a_name)
				resource->SetName(a_name);
			return resource;
		}
		rhi::vulkan::ImportedImageDesc import{};
		import.image = a_image.image;
		import.createInfo.flags = a_image.flags;
		import.createInfo.imageType = a_image.type;
		import.createInfo.format = a_image.format;
		import.createInfo.extent = a_image.extent;
		import.createInfo.mipLevels = a_image.mipLevels;
		import.createInfo.arrayLayers = a_image.arrayLayers;
		import.createInfo.samples = a_image.samples;
		import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		import.createInfo.usage = a_image.usage;
		import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		import.currentLayout = a_image.layout;
		// In GENERAL, DXVK and the graph share the image with memory barriers alone. In any other layout the graph uses it
		// exclusively and moves it between layouts, starting from and returning to this one.
		import.simultaneousAccess = a_image.layout == VK_IMAGE_LAYOUT_GENERAL;
		import.debugName = a_name;
		rhi::ResourcePtr resource;
		if (rhi::vulkan::import_image(a_device, import, resource) != rhi::Result::Ok)
			return {};
		return resource;
	}

	rhi::ResourcePtr ImportD3D12Resource(rhi::Device a_device, void* a_resource, const char* a_name)
	{
		rhi::ResourcePtr resource;
		if (!a_resource || rhi::dx12::import_resource(a_device, static_cast<ID3D12Resource*>(a_resource), resource) != rhi::Result::Ok || !resource)
			return {};
		if (a_name)
			resource->SetName(a_name);
		return resource;
	}

	std::shared_ptr<org::ExternalTextureResource> ImportDxvkTexture(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image,
		org::TextureDescription a_desc, const char* a_name)
	{
		const auto home = HomeLayoutOf(a_image.layout);
		if (!home)
			return nullptr;
		auto resource = ImportDxvkImage(a_device, a_image, a_name);
		if (!resource)
			return nullptr;
		// The home state: the graph finds the image in it and gives it back in it (external home states).
		a_desc.initialLayout = *home;
		// Common-only (simultaneous access, every pass in GENERAL) exists on Vulkan alone. D3D12's COMMON layout cannot be a render
		// target, depth or UAV, so there an image resting in Common is used exclusively: the graph moves it into each pass's layout
		// and gives it back in Common.
		const bool commonOnly = *home == rhi::ResourceLayout::Common && !Backend::IsD3D12();
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, commonOnly);
		if (result)
			result->SetName(a_name);
		return result;
	}
}
