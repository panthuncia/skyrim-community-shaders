#pragma once

// Why GpuTextures::ResolveBinding gave no binding for a view, in one line for a refusal's log (the water's and the screen blood's
// families). Render thread.

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "TextureStreaming/Streamer.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::Water
{
	/** @brief The sum of GpuTextures' permanent rejections so far: a failed ResolveBinding that raised it was a rejection. */
	inline std::uint64_t RejectionCount()
	{
		const auto stats = GpuTextures::Get().GetStats();
		std::uint64_t sum = 0;
		for (const auto count : stats.rejected)
			sum += count;
		return sum;
	}

	/**
	 * @brief a_view (the slot a_slot's view, whose ResolveBinding just failed) described: its format, dimension, size and mips, and why:
	 * a rejection (a_rejectionsBefore is RejectionCount() before the call), a streamed texture whose full window is not loaded yet
	 * (ResolveBinding pins every view full; the window is asked for and a later batch takes it), or no import context (outside an epoch's
	 * descriptor service).
	 */
	inline std::string DescribeTextureFailure(ID3D11ShaderResourceView* a_view, std::uint32_t a_slot, std::uint64_t a_rejectionsBefore)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		a_view->GetDesc(&view);
		std::uint32_t width = 0, height = 0, depth = 0, mips = 0;
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		if (const auto texture = resource.try_as<ID3D11Texture2D>()) {
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			width = desc.Width, height = desc.Height, depth = desc.ArraySize, mips = desc.MipLevels;
		} else if (const auto volume = resource.try_as<ID3D11Texture3D>()) {
			D3D11_TEXTURE3D_DESC desc{};
			volume->GetDesc(&desc);
			width = desc.Width, height = desc.Height, depth = desc.Depth, mips = desc.MipLevels;
		}
		const char* why = RejectionCount() > a_rejectionsBefore ? "GpuTextures rejected it (see its reject counts in the status)" :
		                  !::TextureStreaming::PinFull(a_view)  ? "a streamed texture without its full window yet (asked for; a later batch takes it)" :
		                                                          "no import context (outside an epoch's descriptor service)";
		return fmt::format("t{}: format {}, dimension {}, {}x{}x{}, {} mips: {}", a_slot, static_cast<std::uint32_t>(view.Format),
			static_cast<std::uint32_t>(view.ViewDimension), width, height, depth, mips, why);
	}
}
