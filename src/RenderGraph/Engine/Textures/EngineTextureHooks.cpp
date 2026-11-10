#include "RenderGraph/Engine/Textures/EngineTextures.h"

#include <DirectXTex.h>

#include <cstring>
#include <format>
#include <string>

// The engine's texture creators, hooked for CS_ORG_ENGINE_TEXTURES (docs/development/p4/textures.md has the RE). Each makes the
// 0x28-byte block the engine keeps (BSGraphics::Texture: texture +0x00, view +0x10, refcount +0x20) the way the engine's own
// function does, around Create's objects, or calls the engine's function when Create declines.
namespace RenderGraph::Engine::Textures
{
	namespace
	{
		// The file the calling thread's DDS load reads, for the texture's name. Set by the DDS loader's hook around the load.
		thread_local const std::string* t_loading = nullptr;

		std::string StreamName(RE::NiBinaryStream* a_stream)
		{
			if (!a_stream || *reinterpret_cast<std::uintptr_t*>(a_stream) != RE::VTABLE_BSResourceNiBinaryStream[0].address())
				return {};
			auto* stream = static_cast<RE::BSResourceNiBinaryStream*>(a_stream)->stream.get();
			RE::BSFixedString name;
			if (!stream || !stream->DoGetName(name) || !name.c_str())
				return {};
			return name.c_str();
		}

		/** @brief A zeroed block from the engine's heap, which its release (SE 75527, AE 77322) frees. */
		RE::BSGraphics::Texture* NewBlock()
		{
			auto* block = static_cast<RE::BSGraphics::Texture*>(RE::malloc(0x28));
			if (block)
				std::memset(static_cast<void*>(block), 0, 0x28);
			return block;
		}

		/**
		 * @brief The DDS loader (SE 75721, AE 77533): parses the header and calls CreateD3DResources. Hooked only to name the texture
		 * after its file. Texture streaming hooks the same function, and a shell it makes never reaches CreateD3DResources.
		 */
		struct LoadDDS
		{
			static HRESULT thunk(ID3D11Device* a_device, RE::NiBinaryStream* a_stream, RE::BSGraphics::Texture** a_out, void* a_header, std::uint32_t a_arg5,
				std::uint32_t a_arg6)
			{
				const std::string name = StreamName(a_stream);
				const auto* outer = std::exchange(t_loading, &name);
				const auto result = func(a_device, a_stream, a_out, a_header, a_arg5, a_arg6);
				t_loading = outer;
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief CreateD3DResources (SE 75724, AE 77538): CreateTexture1D/2D/3D (by a_dimension) with the file's subresources, then
		 * the SRV, into a new block. Loader threads, no lock. 1D textures stay the engine's.
		 */
		struct CreateD3DResources
		{
			static HRESULT thunk(ID3D11Device* a_device, std::uint32_t a_dimension, std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_depth,
				std::uint32_t a_mips, std::uint64_t a_arraySize, DXGI_FORMAT a_format, bool a_cube, const D3D11_SUBRESOURCE_DATA* a_data,
				RE::BSGraphics::Texture** a_out)
			{
				if (a_device && a_data && a_out &&
					(a_dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D || a_dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)) {
					CreateInfo info;
					info.dimension = static_cast<D3D11_RESOURCE_DIMENSION>(a_dimension);
					info.width = a_width;
					info.height = a_height;
					info.mipLevels = a_mips;
					info.format = a_format;
					info.initialData = a_data;
					info.srv.Format = a_format;
					if (info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
						info.depthOrArraySize = a_depth;
						info.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
						info.srv.Texture3D = { 0, a_mips };
					} else {
						// The engine's views: a cube below 7 slices, a cube array above; a plain 2D texture below 2, an array above.
						const auto slices = static_cast<UINT>(a_arraySize);
						info.depthOrArraySize = slices;
						if (a_cube) {
							info.miscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
							if (slices < 7) {
								info.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
								info.srv.TextureCube = { 0, a_mips };
							} else {
								info.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
								info.srv.TextureCubeArray = { 0, a_mips, 0, slices / 6 };
							}
						} else if (slices < 2) {
							info.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
							info.srv.Texture2D = { 0, a_mips };
						} else {
							info.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
							info.srv.Texture2DArray = { 0, a_mips, 0, slices };
						}
					}
					info.name = t_loading && !t_loading->empty() ? "Engine::" + *t_loading : std::format("Engine::Texture {}x{}", a_width, a_height);
					Created created;
					if (Create(info, created)) {
						if (auto* block = NewBlock()) {
							block->texture = static_cast<ID3D11Texture2D*>(created.texture.detach());
							block->resourceView = created.view.detach();
							*a_out = block;
							return S_OK;
						}
					}
				}
				return func(a_device, a_dimension, a_width, a_height, a_depth, a_mips, a_arraySize, a_format, a_cube, a_data, a_out);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Renderer::CreateTexture2D (SE 75511, AE 77303): one mip, a_data's rows packed, a_usage DEFAULT/DYNAMIC/STAGING, a
		 * UAV bind when a_uav. DEFAULT ones come here; DYNAMIC (Bink's planes, mapped every frame) and STAGING stay the engine's.
		 */
		struct CreateTexture2D
		{
			static RE::BSGraphics::Texture* thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_width, std::uint32_t a_height, const void* a_data,
				D3D11_USAGE a_usage, DXGI_FORMAT a_format, bool a_uav)
			{
				if (a_usage == D3D11_USAGE_DEFAULT) {
					std::size_t rowPitch = 0, slicePitch = 0;
					if (SUCCEEDED(DirectX::ComputePitch(a_format, a_width, a_height, rowPitch, slicePitch))) {
						const D3D11_SUBRESOURCE_DATA data{ a_data, static_cast<UINT>(rowPitch), static_cast<UINT>(slicePitch) };
						CreateInfo info;
						info.width = a_width;
						info.height = a_height;
						info.format = a_format;
						info.bindFlags = D3D11_BIND_SHADER_RESOURCE | (a_uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
						info.initialData = a_data ? &data : nullptr;
						info.srv.Format = a_format;
						info.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
						info.srv.Texture2D = { 0, 1 };
						info.name = std::format("Engine::Texture2D {}x{}", a_width, a_height);
						Created created;
						if (Create(info, created)) {
							if (auto* block = NewBlock()) {
								// As the engine fills it: height at +0x18, width at +0x1A, then 1, the format's low byte, 0, and one reference.
								auto* bytes = reinterpret_cast<std::uint8_t*>(block);
								*reinterpret_cast<std::uint16_t*>(bytes + 0x18) = static_cast<std::uint16_t>(a_height);
								*reinterpret_cast<std::uint16_t*>(bytes + 0x1A) = static_cast<std::uint16_t>(a_width);
								bytes[0x1C] = 1;
								bytes[0x1D] = static_cast<std::uint8_t>(a_format);
								*reinterpret_cast<std::uint32_t*>(bytes + 0x20) = 1;
								block->texture = static_cast<ID3D11Texture2D*>(created.texture.detach());
								block->resourceView = created.view.detach();
								return block;
							}
						}
					}
				}
				return func(a_renderer, a_width, a_height, a_data, a_usage, a_format, a_uav);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		if (!Requested())
			return;
		stl::detour_thunk<LoadDDS>(REL::RelocationID(75721, 77533));
		stl::detour_thunk<CreateD3DResources>(REL::RelocationID(75724, 77538));
		stl::detour_thunk<CreateTexture2D>(REL::RelocationID(75511, 77303));
		logger::info("[ORG] engine textures: on (CS_ORG_ENGINE_TEXTURES=1); file and DEFAULT textures are made on render graph images");
	}
}
