#include "RenderGraph/Engine/Scaleform/GfxTextures.h"

#include "RenderGraph/Engine/Scaleform/GRendererLayout.h"
#include "RenderGraph/Engine/Textures/EngineTextures.h"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>

namespace RenderGraph::Engine::Scaleform::GfxTextures
{
	namespace
	{
		/** @brief Set while GTextureD3D1x makes its texture on this thread: the device's CreateTexture2D is GFx's then. */
		thread_local bool t_gfxTexture = false;

		std::once_flag g_deviceHooked;
		std::atomic<std::uint64_t> g_created{ 0 };
		std::atomic<bool> g_loggedFirst{ false };

		/** @brief ID3D11Device::CreateTexture2D (slot 5) on the device the renderer holds. */
		struct CreateTexture2D
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data,
				ID3D11Texture2D** a_out)
			{
				if (t_gfxTexture && a_desc && a_out && Eligible(*a_desc)) {
					if (auto* texture = Make(*a_desc, a_data)) {
						*a_out = texture;
						return S_OK;
					}
				}
				return func(a_device, a_desc, a_data, a_out);
			}

			/** @brief A texture Engine::Textures::Create takes: one 2D shader-resource texture, DEFAULT, nothing else. */
			static bool Eligible(const D3D11_TEXTURE2D_DESC& a_desc)
			{
				return a_desc.Usage == D3D11_USAGE_DEFAULT && a_desc.BindFlags == D3D11_BIND_SHADER_RESOURCE && a_desc.CPUAccessFlags == 0 &&
				       a_desc.MiscFlags == 0 && a_desc.ArraySize == 1 && a_desc.SampleDesc.Count == 1 && a_desc.Width && a_desc.Height && Textures::Active();
			}

			/** @brief The texture made on ORG with GFx's contents (DXVK's texture, or a tripwire handle of it), or null. */
			static ID3D11Texture2D* Make(const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data)
			{
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
					return nullptr;
				if (!g_loggedFirst.exchange(true, std::memory_order_relaxed))
					logger::info("[ORG] Scaleform: GFx's textures are made on ORG (first: {})", info.name);
				g_created.fetch_add(1, std::memory_order_relaxed);
				// GFx makes its own view of it (CreateShaderResourceView): Textures::Find answers for that view through the texture.
				// The texture is a texture handle or DXVK's texture, an ID3D11Texture2D either way.
				return static_cast<ID3D11Texture2D*>(created.texture.detach());
			}

			using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
			static inline Fn func = nullptr;
		};

		/** @brief The device's slot 5 is CS's from the first GFx texture on: the device is the one GFx made the texture with. */
		void HookDevice(void* a_texture)
		{
			auto* renderer = Layout::Field<void*>(a_texture, Layout::kTextureOwner);
			auto* device = renderer ? Layout::Field<ID3D11Device*>(renderer, Layout::kDevice) : nullptr;
			if (!device)
				return;
			std::call_once(g_deviceHooked, [device] {
				auto* vtable = *reinterpret_cast<std::uintptr_t**>(device);
				CreateTexture2D::func = reinterpret_cast<CreateTexture2D::Fn>(vtable[5]);
				REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[5]), reinterpret_cast<std::uintptr_t>(&CreateTexture2D::thunk));
				logger::info("[ORG] Scaleform: GFx's textures go through Engine::Textures::Create (device {})", fmt::ptr(device));
			});
		}

		/** @brief Marks the calling thread as making a GFx texture for the scope. */
		struct Scope
		{
			explicit Scope(void* a_texture)
			{
				HookDevice(a_texture);
				previous = std::exchange(t_gfxTexture, CreateTexture2D::func != nullptr);
			}
			~Scope() { t_gfxTexture = previous; }
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;
			bool previous = false;
		};

		/** @brief GTextureD3D1x::InitTexture(GImageBase*), slot 1: images, gradients. */
		struct InitTexture
		{
			static std::uint64_t thunk(void* a_texture, void* a_image)
			{
				Scope scope(a_texture);
				return func(a_texture, a_image);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief GTextureD3D1x::InitDynamicTexture(width, height, format, mipmaps, usage), slot 2: the glyph cache. */
		struct InitDynamicTexture
		{
			static std::uint64_t thunk(void* a_texture, std::int32_t a_width, std::int32_t a_height, std::int32_t a_format, std::int32_t a_mipmaps, std::uint32_t a_usage)
			{
				Scope scope(a_texture);
				return func(a_texture, a_width, a_height, a_format, a_mipmaps, a_usage);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (!Textures::Requested())
			return;
		REL::Relocation<std::uintptr_t> vtable{ REL::RelocationID(292188, 243114) };
		InitTexture::func = vtable.write_vfunc(1, InitTexture::thunk);
		InitDynamicTexture::func = vtable.write_vfunc(2, InitDynamicTexture::thunk);
		logger::info("[ORG] Scaleform: GTextureD3D1x's InitTexture and InitDynamicTexture hooked (its textures on ORG once the graph can wrap images)");
	}
}
