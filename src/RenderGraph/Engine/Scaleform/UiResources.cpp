#include "RenderGraph/Engine/Scaleform/UiResources.h"

#include "RenderGraph/Fatal.h"
#include "Snapshot/Retire.h"
#include "Utils/D3D.h"

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace RenderGraph::Engine::Scaleform::UiResources
{
	namespace
	{
		// GTextureD3D1x has 17 slots and GRenderTargetD3D1x 8. Their vtables sit 0x110 bytes apart on SE and AE, with no RTTI word
		// between them (GFx is built without it), so the target's is found from the texture's id.
		constexpr std::size_t kTextureSlots = 17;
		constexpr std::size_t kTargetSlots = 8;
		constexpr std::uintptr_t kTargetVtableOffset = 0x110;

		// GImage formats.
		constexpr std::uint32_t kImageARGB = 1;
		constexpr std::uint32_t kImageRGB = 2;
		constexpr std::uint32_t kImageL8 = 8;
		constexpr std::uint32_t kImageA8 = 9;
		constexpr std::uint32_t kImageDXT1 = 10;
		constexpr std::uint32_t kImageDXT3 = 11;
		constexpr std::uint32_t kImageDXT5 = 12;
		constexpr std::uint32_t kImageDepthStencil = 0x12E;  // InitDynamicTexture's depth-stencil format
		constexpr std::uint32_t kImageP8 = 100;
		// ImageTexUsage.
		constexpr std::uint32_t kUsageMap = 0x20;
		constexpr std::uint32_t kUsageRenderTarget = 0x40;
		// InitDynamicTexture fills a new texture with this byte.
		constexpr std::uint8_t kDynamicFill = 0x40;

		std::array<void*, kTextureSlots + 1> g_textureVtable{};
		std::array<void*, kTargetSlots + 1> g_targetVtable{};

		struct VideoMode
		{
			std::mutex mutex;
			winrt::com_ptr<ID3D11Device> device;
			winrt::com_ptr<ID3D11DeviceContext> context;
			std::atomic<bool> set{ false };
		};

		VideoMode& Mode()
		{
			static VideoMode s_mode;
			return s_mode;
		}

		/** @brief A write into a texture after its creation, applied with the stream it belongs to. */
		struct Upload
		{
			std::uint64_t sequence = 0;
			winrt::com_ptr<ID3D11Texture2D> texture;
			UINT subresource = 0;
			bool whole = false;
			D3D11_BOX box{};
			UINT rowPitch = 0;
			std::vector<std::byte> bytes;
		};

		struct Uploads
		{
			std::mutex mutex;
			std::deque<Upload> queue;
		};

		Uploads& Queue()
		{
			static Uploads s_uploads;
			return s_uploads;
		}

		// The stream an upload made now belongs to: the open one, or the next.
		std::atomic<std::uint64_t> g_uploadSequence{ 1 };

		struct Stats
		{
			std::atomic<std::uint64_t> textures{ 0 }, targets{ 0 }, live{ 0 }, uploads{ 0 }, uploadBytes{ 0 }, applied{ 0 };
		};

		Stats& Counts()
		{
			static Stats s_stats;
			return s_stats;
		}

		/** @brief PushTempRenderTarget's pools: the targets and the depth-stencil textures, each holding one reference. Render owner. */
		struct Pools
		{
			std::vector<RenderTarget*> targets;
			std::vector<Texture*> stencils;
		};

		Pools& Pool()
		{
			static Pools s_pools;
			return s_pools;
		}

		Texture* AsTexture(RE::GTexture* a_texture) { return reinterpret_cast<Texture*>(a_texture); }

		std::int32_t Ref(const void* a_object)
		{
			return a_object ? static_cast<const Texture*>(a_object)->refCount : 0;
		}

		void SwapRef(RE::GTexture*& a_slot, RE::GTexture* a_value)
		{
			AddRef(a_value);
			Release(a_slot);
			a_slot = a_value;
		}

		/** @brief GTexture's and GRenderTarget's CallHandlers (SE 0x1410344b0): each change handler's OnChange(renderer, event). */
		void CallHandlers(std::uint8_t a_multiple, void* a_handlers, void* a_renderer, std::int32_t a_event)
		{
			using OnChange = void (*)(void*, void*, std::int32_t);
			const auto call = [&](void* a_handler) { (*static_cast<OnChange**>(a_handler))[1](a_handler, a_renderer, a_event); };
			if (!a_multiple) {
				if (a_handlers)
					call(a_handlers);
				return;
			}
			// A handler may remove itself, so the array is copied first.
			const auto* array = static_cast<void* const*>(a_handlers);
			const auto* data = static_cast<void* const*>(array[0]);
			const auto size = reinterpret_cast<std::uintptr_t>(array[1]);
			const std::vector<void*> copy(data, data + size);
			for (auto* handler : copy)
				call(handler);
		}

		/** @brief The GFx base destructor's part: frees a handler array (GFx's heap). */
		void FreeHandlers(std::uint8_t a_multiple, void* a_handlers)
		{
			if (!a_multiple || !a_handlers)
				return;
			auto** array = static_cast<void**>(a_handlers);
			RE::GMemory::Free(array[0]);
			RE::GMemory::Free(array);
		}

		void ReleaseData(Texture& a_texture)
		{
			if (a_texture.texture)
				a_texture.texture->Release();
			if (a_texture.view)
				a_texture.view->Release();
			a_texture.texture = nullptr;
			a_texture.view = nullptr;
		}

		void DataChanged(Texture& a_texture)
		{
			CallHandlers(a_texture.multipleHandlers, a_texture.handlers, a_texture.renderer, 0);
		}

		/** @brief Makes the texture from a_texture's desc and the subresources, and its view. False leaves it without data. */
		bool Make(Texture& a_texture, const D3D11_SUBRESOURCE_DATA* a_data, bool a_view)
		{
			auto* device = Device();
			if (!device)
				return false;
			ID3D11Texture2D* made = nullptr;
			if (FAILED(device->CreateTexture2D(&a_texture.desc, a_data, &made)) || !made)
				return false;
			Util::SetResourceName(made, "Scaleform::Texture %ux%u format %u", a_texture.desc.Width, a_texture.desc.Height, static_cast<unsigned>(a_texture.desc.Format));
			a_texture.texture = made;
			if (a_view && FAILED(device->CreateShaderResourceView(made, nullptr, &a_texture.view))) {
				ReleaseData(a_texture);
				return false;
			}
			return true;
		}

		/** @brief GImageBase::GetMipMapLevelData (SE 0x140f65eb0): a level's data, size and pitch. Null past the image. */
		const std::uint8_t* LevelData(const RE::GImageBase& a_image, std::uint32_t a_level, std::uint32_t& a_width, std::uint32_t& a_height, std::uint32_t& a_pitch)
		{
			const std::uint32_t format = a_image.format.underlying();
			a_width = a_image.width;
			a_height = a_image.height;
			a_pitch = a_image.pitch;
			if (a_image.mipMapCount < a_level)
				return nullptr;
			const std::uint8_t* data = a_image.data;
			for (std::uint32_t i = 0; i < a_level; ++i) {
				std::uint64_t size = 0;
				const std::uint32_t blocksX = (std::max)((a_width + 3) >> 2, 1u), blocksY = (std::max)((a_height + 3) >> 2, 1u);
				if (format == kImageDXT1)
					size = std::uint64_t(blocksX) * blocksY * 8;
				else if (format == kImageDXT3 || format == kImageDXT5)
					size = std::uint64_t(blocksX) * blocksY * 16;
				else if (format == kImageARGB)
					size = std::uint64_t(a_width) * 4 * a_height;
				else if (format == kImageRGB)
					size = std::uint64_t(((a_width + 1) * 3) & ~3u) * a_height;
				else if (format == kImageL8 || format == kImageA8 || format == kImageP8)
					size = std::uint64_t(a_width) * a_height;
				data += size;
				a_width = (std::max)(a_width >> 1, 1u);
				a_height = (std::max)(a_height >> 1, 1u);
				a_pitch >>= 1;
			}
			if (a_level && data >= a_image.data + a_image.dataSize)
				return nullptr;
			return data;
		}

		/** @brief The D3D1x renderer's A8 mip step (SE 0x140f60df0): a 2x2 box filter when both sides halve exactly, else the bytes as they are. */
		std::vector<std::uint8_t> HalveA8(const std::vector<std::uint8_t>& a_level, std::uint32_t& a_width, std::uint32_t& a_height)
		{
			const std::uint32_t width = (std::max)(a_width >> 1, 1u), height = (std::max)(a_height >> 1, 1u);
			std::vector<std::uint8_t> out(std::size_t(width) * height);
			if (width * 2 == a_width && height * 2 == a_height) {
				for (std::uint32_t y = 0; y < height; ++y)
					for (std::uint32_t x = 0; x < width; ++x) {
						const std::uint8_t* p = a_level.data() + std::size_t(y) * 2 * a_width + std::size_t(x) * 2;
						out[std::size_t(y) * width + x] = static_cast<std::uint8_t>((p[0] + p[1] + p[a_width] + p[a_width + 1]) >> 2);
					}
			} else {
				std::memcpy(out.data(), a_level.data(), (std::min)(out.size(), a_level.size()));
			}
			a_width = width;
			a_height = height;
			return out;
		}

		std::uint32_t BytesPerPixel(DXGI_FORMAT a_format)
		{
			return a_format == DXGI_FORMAT_A8_UNORM ? 1u : 4u;
		}

		void Queue(Upload a_upload)
		{
			auto& counts = Counts();
			counts.uploads.fetch_add(1, std::memory_order_relaxed);
			counts.uploadBytes.fetch_add(a_upload.bytes.size(), std::memory_order_relaxed);
			auto& uploads = UiResources::Queue();
			std::scoped_lock lock(uploads.mutex);
			uploads.queue.push_back(std::move(a_upload));
		}

		// ---------------------------------------------------------------------------------------------------------------------
		// GTextureD3D1x's slots, CS's ports.

		void* TextureDestroy(Texture* a_self, std::uint32_t a_flags)
		{
			ReleaseData(*a_self);
			FreeHandlers(a_self->multipleHandlers, a_self->handlers);
			delete[] a_self->mapped;
			Counts().live.fetch_sub(1, std::memory_order_relaxed);
			if (a_flags & 1)
				delete a_self;
			return a_self;
		}

		/** @brief InitTexture (slot 1, SE 0x140f602c0): the image's levels, converted as the D3D1x renderer converts them. */
		bool TextureInit(Texture* a_self, const RE::GImageBase* a_image, std::uint32_t)
		{
			if (!a_self->renderer || !Device())
				return false;
			ReleaseData(*a_self);
			if (!a_image || !a_image->data) {
				DataChanged(*a_self);
				return false;
			}
			const auto& image = *a_image;
			const std::uint32_t format = image.format.underlying();
			DXGI_FORMAT dxgi;
			switch (format) {
			case kImageARGB:
			case kImageRGB:
				dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
				break;
			case kImageA8:
				dxgi = DXGI_FORMAT_A8_UNORM;
				break;
			case kImageDXT1:
				dxgi = DXGI_FORMAT_BC1_UNORM;
				break;
			case kImageDXT3:
				dxgi = DXGI_FORMAT_BC2_UNORM;
				break;
			case kImageDXT5:
				dxgi = DXGI_FORMAT_BC3_UNORM;
				break;
			default:
				return false;
			}
			std::vector<std::vector<std::uint8_t>> owned;
			std::vector<D3D11_SUBRESOURCE_DATA> data;
			if (format == kImageRGB) {
				// One RGBA level with opaque alpha, whatever levels the image has.
				auto& rgba = owned.emplace_back(std::size_t(image.width) * 4 * image.height);
				for (std::uint32_t y = 0; y < image.height; ++y) {
					const std::uint8_t* from = image.data + std::size_t(y) * image.pitch;
					std::uint8_t* to = rgba.data() + std::size_t(y) * image.width * 4;
					for (std::uint32_t x = 0; x < image.width; ++x, from += 3, to += 4) {
						to[0] = from[0];
						to[1] = from[1];
						to[2] = from[2];
						to[3] = 0xFF;
					}
				}
				data.push_back({ rgba.data(), image.width * 4, 0 });
			} else if ((format >= kImageDXT1 && format <= kImageDXT5) || image.mipMapCount > 1) {
				const std::uint32_t levels = (std::max)(image.mipMapCount, 1u);
				for (std::uint32_t level = 0; level < levels; ++level) {
					std::uint32_t width = 0, height = 0, pitch = 0;
					const auto* bytes = LevelData(image, level, width, height, pitch);
					if (!bytes)
						return false;
					data.push_back({ bytes, pitch, 0 });
				}
			} else if (format == kImageA8) {
				// A one-level alpha image gets the full chain, each level a box filter of the one before.
				std::uint32_t width = image.width, height = image.height;
				auto& top = owned.emplace_back(std::size_t(width) * height);
				for (std::uint32_t y = 0; y < height; ++y)
					std::memcpy(top.data() + std::size_t(y) * width, image.data + std::size_t(y) * image.pitch, width);
				std::uint32_t levels = 1;
				for (std::uint32_t w = width, h = height; w > 1 || h > 1; w >>= 1, h >>= 1)
					++levels;
				owned.reserve(levels);
				for (std::uint32_t level = 1; level < levels; ++level) {
					auto next = HalveA8(owned.back(), width, height);
					owned.push_back(std::move(next));
				}
				width = image.width;
				for (const auto& level : owned) {
					data.push_back({ level.data(), width, 0 });
					width = (std::max)(width >> 1, 1u);
				}
			} else {
				data.push_back({ image.data, image.pitch, 0 });
			}
			a_self->desc = {};
			a_self->desc.Width = image.width;
			a_self->desc.Height = image.height;
			a_self->desc.MipLevels = static_cast<UINT>(data.size());
			a_self->desc.ArraySize = 1;
			a_self->desc.Format = dxgi;
			a_self->desc.SampleDesc.Count = 1;
			a_self->desc.Usage = D3D11_USAGE_DEFAULT;
			a_self->desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if (!Make(*a_self, data.data(), true))
				return false;
			DataChanged(*a_self);
			return true;
		}

		/** @brief InitDynamicTexture (slot 2, SE 0x140f5f4f0): a texture filled with 0x40, mappable or a render target by usage. */
		bool TextureInitDynamic(Texture* a_self, std::int32_t a_width, std::int32_t a_height, std::uint32_t a_format, std::int32_t a_mipmaps, std::uint32_t a_usage)
		{
			if (!a_self->renderer || !Device())
				return false;
			ReleaseData(*a_self);
			auto& desc = a_self->desc;
			desc = {};
			if (a_format == kImageARGB)
				desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			else if (a_format == kImageA8)
				desc.Format = DXGI_FORMAT_A8_UNORM;
			else if (a_format == kImageDepthStencil)
				desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			else
				return false;
			const bool depth = a_format == kImageDepthStencil;
			desc.Width = static_cast<UINT>(a_width);
			desc.Height = static_cast<UINT>(a_height);
			desc.MipLevels = static_cast<UINT>(a_mipmaps + 1);
			desc.ArraySize = 1;
			desc.SampleDesc.Count = 1;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if (a_usage & kUsageMap) {
				desc.Usage = D3D11_USAGE_DYNAMIC;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			}
			if (a_usage & kUsageRenderTarget)
				desc.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			const std::uint32_t bpp = BytesPerPixel(desc.Format);
			std::vector<std::uint8_t> fill(std::size_t(desc.Width) * desc.Height * bpp, kDynamicFill);
			std::vector<D3D11_SUBRESOURCE_DATA> data;
			for (UINT level = 0, width = desc.Width; level < desc.MipLevels; ++level, width = (std::max)(width >> 1, 1u))
				data.push_back({ fill.data(), width * bpp, 0 });
			return Make(*a_self, depth ? nullptr : data.data(), !depth);
		}

		/**
		 * @brief Update (slot 3, SE 0x140f63b10): each rectangle of a_image into the level, as an upload in stream order.
		 * The glyph cache writes through it.
		 */
		void TextureUpdate(Texture* a_self, std::int32_t a_level, std::int32_t a_count, const RE::GTexture::UpdateRect* a_rects, const RE::GImageBase* a_image)
		{
			if (!a_self->texture || a_count <= 0 || !a_rects || !a_image)
				return;
			const std::uint32_t format = a_image->format.underlying();
			if (format >= kImageDXT1 && format <= kImageDXT5)
				RenderGraph::Fatal("Scaleform", "GFx updated a compressed texture by rectangles, which the D3D1x renderer cannot do either");
			const std::uint32_t bpp = format == kImageARGB ? 4u : format == kImageRGB ? 3u :
			                                                                            1u;
			const auto sequence = g_uploadSequence.load(std::memory_order_acquire);
			for (std::int32_t i = 0; i < a_count; ++i) {
				const auto& rect = a_rects[i];
				const auto width = static_cast<std::uint32_t>(rect.src.right - rect.src.left);
				const auto height = static_cast<std::uint32_t>(rect.src.bottom - rect.src.top);
				if (!width || !height || rect.src.right < rect.src.left || rect.src.bottom < rect.src.top)
					continue;
				Upload upload;
				upload.sequence = sequence;
				upload.texture.copy_from(a_self->texture);
				upload.subresource = static_cast<UINT>(a_level);
				upload.box = { static_cast<UINT>(rect.dest.x), static_cast<UINT>(rect.dest.y), 0, static_cast<UINT>(rect.dest.x) + width,
					static_cast<UINT>(rect.dest.y) + height, 1 };
				upload.rowPitch = width * bpp;
				upload.bytes.resize(std::size_t(upload.rowPitch) * height);
				const std::uint8_t* from = a_image->data + std::size_t(a_image->pitch) * rect.src.top + std::size_t(bpp) * rect.src.left;
				for (std::uint32_t row = 0; row < height; ++row)
					std::memcpy(upload.bytes.data() + std::size_t(row) * upload.rowPitch, from + std::size_t(row) * a_image->pitch, upload.rowPitch);
				UiResources::Queue(std::move(upload));
			}
		}

		std::uint32_t LevelOffset(const D3D11_TEXTURE2D_DESC& a_desc, std::uint32_t a_level, std::uint32_t& a_width, std::uint32_t& a_height)
		{
			const std::uint32_t bpp = BytesPerPixel(a_desc.Format);
			std::uint32_t offset = 0;
			a_width = a_desc.Width;
			a_height = a_desc.Height;
			for (std::uint32_t level = 0; level < a_level; ++level) {
				offset += a_width * a_height * bpp;
				a_width = (std::max)(a_width >> 1, 1u);
				a_height = (std::max)(a_height >> 1, 1u);
			}
			return offset;
		}

		/** @brief Map (slot 4): a CPU copy of each level from a_level, written back as uploads at Unmap. Keeps what was written before. */
		std::int32_t TextureMap(Texture* a_self, std::int32_t a_level, std::int32_t a_count, RE::GTexture::MapRect* a_maps, std::uint32_t)
		{
			if (!a_self->texture || a_count <= 0 || !a_maps || a_level < 0 || static_cast<UINT>(a_level + a_count) > a_self->desc.MipLevels)
				return 0;
			if (!a_self->mapped) {
				std::uint32_t width = 0, height = 0;
				a_self->mappedSize = LevelOffset(a_self->desc, a_self->desc.MipLevels, width, height);
				a_self->mapped = new std::uint8_t[a_self->mappedSize];
				std::memset(a_self->mapped, kDynamicFill, a_self->mappedSize);
			}
			const std::uint32_t bpp = BytesPerPixel(a_self->desc.Format);
			for (std::int32_t i = 0; i < a_count; ++i) {
				std::uint32_t width = 0, height = 0;
				const auto offset = LevelOffset(a_self->desc, static_cast<std::uint32_t>(a_level + i), width, height);
				a_maps[i] = { width, height, a_self->mapped + offset, width * bpp, 0 };
			}
			return 1;
		}

		bool TextureUnmap(Texture* a_self, std::int32_t a_level, std::int32_t a_count, RE::GTexture::MapRect*, std::uint32_t)
		{
			if (!a_self->texture || !a_self->mapped || a_count <= 0 || a_level < 0)
				return false;
			const std::uint32_t bpp = BytesPerPixel(a_self->desc.Format);
			const auto sequence = g_uploadSequence.load(std::memory_order_acquire);
			for (std::int32_t i = 0; i < a_count && static_cast<UINT>(a_level + i) < a_self->desc.MipLevels; ++i) {
				std::uint32_t width = 0, height = 0;
				const auto offset = LevelOffset(a_self->desc, static_cast<std::uint32_t>(a_level + i), width, height);
				Upload upload;
				upload.sequence = sequence;
				upload.texture.copy_from(a_self->texture);
				upload.subresource = static_cast<UINT>(a_level + i);
				upload.whole = true;
				upload.rowPitch = width * bpp;
				upload.bytes.assign(reinterpret_cast<const std::byte*>(a_self->mapped + offset), reinterpret_cast<const std::byte*>(a_self->mapped + offset + width * height * bpp));
				UiResources::Queue(std::move(upload));
			}
			return true;
		}

		/** @brief InitTexture from an engine texture (slot 12, SE 0x140f601d0): keeps it with a reference and makes a view. */
		bool TextureInitExternal(Texture* a_self, ID3D11Texture2D* a_texture)
		{
			if (!a_self->renderer || !Device())
				return false;
			ReleaseData(*a_self);
			if (a_texture) {
				a_texture->AddRef();
				a_self->texture = a_texture;
				a_texture->GetDesc(&a_self->desc);
				if (FAILED(Device()->CreateShaderResourceView(a_texture, nullptr, &a_self->view))) {
					ReleaseData(*a_self);
					return false;
				}
			}
			DataChanged(*a_self);
			return true;
		}

		void TextureReleaseData(Texture* a_self)
		{
			ReleaseData(*a_self);
		}

		/** @brief Bind (slot 16) sets the D3D11 context's stages. Only the D3D1x renderer calls it. */
		void TextureBind(Texture*, std::uint32_t, std::int32_t, std::int32_t)
		{
			RenderGraph::Fatal("Scaleform", "something bound a CS UI texture to the D3D11 context (GTextureD3D1x::Bind)");
		}

		// ---------------------------------------------------------------------------------------------------------------------
		// GRenderTargetD3D1x's slots, CS's ports. CS makes no views: the replay draws pushed targets into scratch images.

		void* TargetDestroy(RenderTarget* a_self, std::uint32_t a_flags)
		{
			Release(a_self->depth);
			Release(a_self->target);
			FreeHandlers(a_self->multipleHandlers, a_self->handlers);
			Counts().live.fetch_sub(1, std::memory_order_relaxed);
			if (a_flags & 1)
				delete a_self;
			return a_self;
		}

		/** @brief InitRenderTarget(target, depth, stencil) (slot 1, SE 0x140f5f920): the depth is the stencil texture when no depth is given. */
		bool TargetInit(RenderTarget* a_self, RE::GTexture* a_target, RE::GTexture* a_depth, RE::GTexture* a_stencil)
		{
			a_self->rtv = nullptr;
			a_self->dsv = nullptr;
			SwapRef(a_self->target, a_target);
			if (a_target) {
				a_self->width = AsTexture(a_target)->desc.Width;
				a_self->height = AsTexture(a_target)->desc.Height;
			}
			if (auto* depth = a_depth ? a_depth : a_stencil)
				SwapRef(a_self->depth, depth);
			CallHandlers(a_self->multipleHandlers, a_self->handlers, a_self->renderer, 0);
			return true;
		}

		/** @brief InitRenderTarget with the engine's views (slot 7, SE 0x140f5faa0): borrowed, as the D3D1x renderer keeps them. */
		bool TargetInitViews(RenderTarget* a_self, void* const* a_views)
		{
			a_self->rtv = static_cast<ID3D11RenderTargetView*>(a_views[0]);
			a_self->dsv = static_cast<ID3D11DepthStencilView*>(a_views[2]);
			CallHandlers(a_self->multipleHandlers, a_self->handlers, a_self->renderer, 0);
			return true;
		}

		template <class Fn>
		void Set(void** a_vtable, std::size_t a_slot, Fn a_function)
		{
			a_vtable[a_slot] = reinterpret_cast<void*>(a_function);
		}
	}

	void Install()
	{
		static bool s_installed = false;
		if (s_installed)
			return;
		s_installed = true;
		const auto textureVtable = REL::RelocationID(292188, 243114).address();
		std::memcpy(g_textureVtable.data(), reinterpret_cast<void* const*>(textureVtable) - 1, sizeof(g_textureVtable));
		std::memcpy(g_targetVtable.data(), reinterpret_cast<void* const*>(textureVtable + kTargetVtableOffset) - 1, sizeof(g_targetVtable));
		void** texture = g_textureVtable.data() + 1;
		Set(texture, 0, &TextureDestroy);
		Set(texture, 1, &TextureInit);
		Set(texture, 2, &TextureInitDynamic);
		Set(texture, 3, &TextureUpdate);
		Set(texture, 4, &TextureMap);
		Set(texture, 5, &TextureUnmap);
		Set(texture, 12, &TextureInitExternal);
		Set(texture, 14, &TextureReleaseData);
		Set(texture, 16, &TextureBind);
		void** target = g_targetVtable.data() + 1;
		Set(target, 0, &TargetDestroy);
		Set(target, 1, &TargetInit);
		Set(target, 7, &TargetInitViews);
		logger::info("[ORG] Scaleform: GFx's textures and render targets are CS objects; their contents and later writes go through CS");
	}

	RE::GTexture* CreateTexture(void* a_renderer)
	{
		auto* texture = new Texture{};
		texture->vtable = g_textureVtable.data() + 1;
		texture->refCount = 1;
		texture->renderer = a_renderer;
		Counts().textures.fetch_add(1, std::memory_order_relaxed);
		Counts().live.fetch_add(1, std::memory_order_relaxed);
		return reinterpret_cast<RE::GTexture*>(texture);
	}

	RenderTarget* CreateRenderTarget(void* a_renderer)
	{
		auto* target = new RenderTarget{};
		target->vtable = g_targetVtable.data() + 1;
		target->refCount = 1;
		target->renderer = a_renderer;
		Counts().targets.fetch_add(1, std::memory_order_relaxed);
		Counts().live.fetch_add(1, std::memory_order_relaxed);
		return target;
	}

	bool IsTexture(const RE::GTexture* a_texture)
	{
		return a_texture && *reinterpret_cast<void* const*>(a_texture) == g_textureVtable.data() + 1;
	}

	bool IsRenderTarget(const void* a_target)
	{
		return a_target && *static_cast<void* const*>(a_target) == g_targetVtable.data() + 1;
	}

	void AddRef(void* a_object)
	{
		if (a_object)
			_InterlockedIncrement(&static_cast<Texture*>(a_object)->refCount);
	}

	void Release(void* a_object)
	{
		if (!a_object || _InterlockedDecrement(&static_cast<Texture*>(a_object)->refCount) != 0)
			return;
		using Destroy = void* (*)(void*, std::uint32_t);
		(*static_cast<Destroy**>(a_object))[0](a_object, 1);
	}

	std::int32_t RefCount(const void* a_object)
	{
		return Ref(a_object);
	}

	void SetVideoMode(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		auto& mode = Mode();
		std::scoped_lock lock(mode.mutex);
		if (!a_device || !a_context)
			return;
		mode.device.copy_from(a_device);
		mode.context.copy_from(a_context);
		mode.set.store(true, std::memory_order_release);
	}

	void ResetVideoMode()
	{
		Mode().set.store(false, std::memory_order_release);
	}

	bool VideoModeSet()
	{
		return Mode().set.load(std::memory_order_acquire);
	}

	ID3D11Device* Device()
	{
		return Mode().device.get();
	}

	ID3D11DeviceContext* Context()
	{
		return Mode().context.get();
	}

	RenderTarget* AcquireTempTarget(void* a_renderer, std::uint32_t a_width, std::uint32_t a_height, bool a_stencil)
	{
		auto& pools = Pool();
		RenderTarget* target = nullptr;
		bool fresh = false;
		std::uint32_t width = 1, height = 1;
		// A free target that is large enough keeps its size.
		for (auto* candidate : pools.targets) {
			const auto* texture = AsTexture(candidate->target);
			if (Ref(texture) < 2 && a_width <= texture->desc.Width && a_height <= texture->desc.Height) {
				target = candidate;
				break;
			}
		}
		if (!target) {
			while (width < a_width)
				width *= 2;
			while (height < a_height)
				height *= 2;
			for (auto* candidate : pools.targets)
				if (Ref(candidate->target) < 2) {
					target = candidate;
					break;
				}
			if (!target) {
				auto* texture = CreateTexture(a_renderer);
				target = CreateRenderTarget(a_renderer);
				target->pooled = 1;
				SwapRef(target->target, texture);
				AddRef(target);
				pools.targets.push_back(target);
				Release(target);
				Release(texture);
			}
			fresh = true;
		}
		auto* texture = AsTexture(target->target);
		width = (std::max)(width, texture->desc.Width);
		height = (std::max)(height, texture->desc.Height);
		Texture* stencil = nullptr;
		const bool haveDepth = target->depth && Ref(target->depth) < 2;
		if (a_stencil && !haveDepth) {
			for (auto* candidate : pools.stencils)
				if (Ref(candidate) < 2 && a_width <= candidate->desc.Width && a_height <= candidate->desc.Height) {
					stencil = candidate;
					break;
				}
			if (!stencil) {
				for (auto* candidate : pools.stencils)
					if (Ref(candidate) < 2) {
						stencil = candidate;
						break;
					}
				if (!stencil) {
					stencil = AsTexture(CreateTexture(a_renderer));
					AddRef(stencil);
					pools.stencils.push_back(stencil);
					Release(stencil);
				}
				TextureInitDynamic(stencil, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), kImageDepthStencil, 0, kUsageRenderTarget);
			}
		} else if (!fresh) {
			target->width = a_width;
			target->height = a_height;
			return target;
		}
		if (width != texture->desc.Width || height != texture->desc.Height)
			TextureInitDynamic(texture, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), kImageARGB, 0, kUsageRenderTarget);
		auto* stencilTexture = reinterpret_cast<RE::GTexture*>(stencil);
		TargetInit(target, target->target, stencilTexture, stencilTexture);
		target->width = a_width;
		target->height = a_height;
		return target;
	}

	void ReleaseTempTargets(std::uint32_t a_keepPixels)
	{
		auto& pools = Pool();
		// The D3D1x renderer takes every kept target's area off the budget, in use or not, in unsigned arithmetic.
		std::uint32_t budget = a_keepPixels;
		std::vector<RenderTarget*> targets;
		for (auto* target : pools.targets) {
			const auto* texture = AsTexture(target->target);
			const std::uint32_t area = texture->desc.Width * texture->desc.Height;
			if (Ref(texture) > 1 || area <= budget) {
				targets.push_back(target);
				budget -= area;
			} else {
				Release(target);
			}
		}
		pools.targets = std::move(targets);
		budget = a_keepPixels;
		std::vector<Texture*> stencils;
		for (auto* stencil : pools.stencils) {
			const std::uint32_t area = stencil->desc.Width * stencil->desc.Height;
			if (Ref(stencil) > 1 || area <= budget) {
				stencils.push_back(stencil);
				budget -= area;
			} else {
				Release(stencil);
			}
		}
		pools.stencils = std::move(stencils);
	}

	void OpenStream(std::uint64_t a_sequence)
	{
		g_uploadSequence.store(a_sequence, std::memory_order_release);
	}

	void CloseStream(std::uint64_t a_sequence)
	{
		g_uploadSequence.store(a_sequence + 1, std::memory_order_release);
	}

	void ApplyUploads(std::uint64_t a_sequence)
	{
		auto& uploads = Queue();
		std::deque<Upload> ready;
		{
			std::scoped_lock lock(uploads.mutex);
			while (!uploads.queue.empty() && uploads.queue.front().sequence <= a_sequence) {
				ready.push_back(std::move(uploads.queue.front()));
				uploads.queue.pop_front();
			}
		}
		if (ready.empty())
			return;
		auto* context = Context();
		if (!context)
			RenderGraph::Fatal("Scaleform", "a UI texture upload before the video mode was set");
		auto textures = std::make_shared<std::vector<winrt::com_ptr<ID3D11Texture2D>>>();
		textures->reserve(ready.size());
		for (auto& upload : ready) {
			context->UpdateSubresource(upload.texture.get(), upload.subresource, upload.whole ? nullptr : &upload.box, upload.bytes.data(), upload.rowPitch, 0);
			textures->push_back(std::move(upload.texture));
		}
		Counts().applied.fetch_add(ready.size(), std::memory_order_relaxed);
		// The uploads' texture references go to the retire queue: the frame's copies may still read them.
		Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Render, std::move(textures));
	}

	std::string TakeStatsLine()
	{
		auto& counts = Counts();
		std::size_t queued = 0;
		{
			auto& uploads = Queue();
			std::scoped_lock lock(uploads.mutex);
			queued = uploads.queue.size();
		}
		return fmt::format("CS UI objects: {} textures and {} targets made, {} alive; {} uploads ({} KiB) queued, {} applied, {} waiting; temp pool {} targets, {} stencils",
			counts.textures.exchange(0), counts.targets.exchange(0), counts.live.load(), counts.uploads.exchange(0), counts.uploadBytes.exchange(0) / 1024,
			counts.applied.exchange(0), queued, Pool().targets.size(), Pool().stencils.size());
	}
}
