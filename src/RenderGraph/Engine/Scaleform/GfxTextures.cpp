#include "RenderGraph/Engine/Scaleform/GfxTextures.h"

#include "RenderGraph/Engine/Textures/EngineTextures.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>

namespace RenderGraph::Engine::Scaleform::GfxTextures
{
	namespace
	{
		std::atomic<bool> g_loggedFirst{ false };

		/** @brief A texture Engine::Textures::Create takes: one 2D shader-resource texture, DEFAULT, nothing else. */
		bool Eligible(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			return a_desc.Usage == D3D11_USAGE_DEFAULT && a_desc.BindFlags == D3D11_BIND_SHADER_RESOURCE && a_desc.CPUAccessFlags == 0 &&
			       a_desc.MiscFlags == 0 && a_desc.ArraySize == 1 && a_desc.SampleDesc.Count == 1 && a_desc.Width && a_desc.Height && Textures::Active();
		}

		/**
		 * @brief The guest device's CreateTexture2D while GTextureD3D1x makes its texture (Tripwire::CreateTexture2DHook): the texture
		 * made on ORG with GFx's contents (a tripwire handle of DXVK's texture), or false to let DXVK make it.
		 */
		bool MakeOnOrg(const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out)
		{
			if (!Eligible(a_desc))
				return false;
			Textures::CreateInfo info;
			info.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			info.width = a_desc.Width;
			info.height = a_desc.Height;
			info.depthOrArraySize = 1;
			info.mipLevels = a_desc.MipLevels ? a_desc.MipLevels : static_cast<UINT>(std::bit_width((std::max)(a_desc.Width, a_desc.Height)));
			info.format = a_desc.Format;
			info.bindFlags = D3D11_BIND_SHADER_RESOURCE;
			info.initialData = a_data;
			info.srv.Format = a_desc.Format;
			info.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			info.srv.Texture2D.MostDetailedMip = 0;
			info.srv.Texture2D.MipLevels = ~0u;
			info.name = fmt::format("Scaleform::Texture2D {}x{} format {}", a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format));
			Textures::Created created;
			// Textures::Create logs why it refused (once per reason); GFx's own texture is then DXVK's.
			if (!Textures::Create(info, created) || !created.texture)
				return false;
			if (!g_loggedFirst.exchange(true, std::memory_order_relaxed))
				logger::info("[ORG] Scaleform: GFx's textures are made on ORG (first: {})", info.name);
			// GFx makes its own view of it (CreateShaderResourceView): Textures::Find answers for that view through the texture.
			// The texture is a texture handle or DXVK's texture, an ID3D11Texture2D either way.
			*a_out = static_cast<ID3D11Texture2D*>(created.texture.detach());
			return true;
		}

		/** @brief GTextureD3D1x::InitTexture(GImageBase*), slot 1: images, gradients. */
		struct InitTexture
		{
			static std::uint64_t thunk(void* a_texture, void* a_image)
			{
				const Tripwire::ScopedCreateTexture2DHook scope(&MakeOnOrg);
				return func(a_texture, a_image);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief GTextureD3D1x::InitDynamicTexture(width, height, format, mipmaps, usage), slot 2: the glyph cache. */
		struct InitDynamicTexture
		{
			static std::uint64_t thunk(void* a_texture, std::int32_t a_width, std::int32_t a_height, std::int32_t a_format, std::int32_t a_mipmaps, std::uint32_t a_usage)
			{
				const Tripwire::ScopedCreateTexture2DHook scope(&MakeOnOrg);
				return func(a_texture, a_width, a_height, a_format, a_mipmaps, a_usage);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (!Textures::Requested())
			return;
		// The interception point is the guest device handle (renderer +0x490 holds it): without the guest, the renderer's device is
		// DXVK's own, shared with CS, and nothing is patched there.
		if (!Tripwire::GuestRequested()) {
			logger::info("[ORG] Scaleform: GFx's textures stay DXVK's (making them on ORG needs CS_ORG_TRIPWIRE_GUEST=1)");
			return;
		}
		REL::Relocation<std::uintptr_t> vtable{ REL::RelocationID(292188, 243114) };
		InitTexture::func = vtable.write_vfunc(1, InitTexture::thunk);
		InitDynamicTexture::func = vtable.write_vfunc(2, InitDynamicTexture::thunk);
		logger::info("[ORG] Scaleform: GTextureD3D1x's InitTexture and InitDynamicTexture hooked (its textures on ORG once the graph can wrap images)");
	}
}
