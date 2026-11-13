#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ImGuiBasicRHI.h"
#	include "RenderGraph/FrameStaging.h"

#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/UndeclaredAccess.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/VersionedTexture.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <bit>
#	include <cstring>
#	include <map>
#	include <mutex>
#	include <unordered_map>
#	include <unordered_set>
#	include <vector>

namespace ImGuiBasicRHI
{
	namespace
	{
		constexpr const char* kShader = "RenderGraph/ImGui.hlsl";
		constexpr const char* kBackendName = "imgui_impl_basicrhi";
		// What a texture's ImTextureID is: this bit and the texture's address. A D3D11 view's address (the old backend's ids, which
		// CS's own textures still are) never has it: user-mode addresses are below 2^47.
		constexpr std::uint64_t kOwnTag = 1ull << 63;
		// D3D12's copy rules, which Vulkan's satisfy: rows of a texture copy 256-byte aligned, a copy's start 512.
		constexpr std::uint32_t kRowAlignment = 256;
		constexpr std::uint64_t kPlacementAlignment = 512;
		constexpr std::uint32_t kMinimumStaging = 256u << 10;
		constexpr std::size_t kSpareStaging = 4;
		// ImGui's textures the passes declare: its font atlas pages. A font atlas has one page; a resize makes a new one while the old
		// is still drawn for a frame. A texture past the last page is not drawn (logged once).
		constexpr std::size_t kAtlasPages = 4;

		// Must match DrawConstants in ImGui.hlsl.
		struct DrawConstants
		{
			std::uint32_t geometryIndex;
			std::uint32_t vertexOffset;
			std::uint32_t indexOffset;
			std::uint32_t indexSize;
			std::uint32_t textureIndex;
			std::uint32_t samplerIndex;
			float scale[2];
			float translate[2];
			std::uint32_t padding[2];
		};
		static_assert(sizeof(DrawConstants) == 48);
		static_assert(sizeof(ImDrawVert) == 20, "ImGui.hlsl reads ImGui's default vertex layout");
		static_assert(sizeof(ImDrawIdx) == 2 || sizeof(ImDrawIdx) == 4);
		constexpr std::uint32_t kConstantWords = ShaderBindings::Words<DrawConstants>();

		std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) / a_alignment * a_alignment; }
	}

	class Texture
	{
	public:
		std::unique_ptr<GraphResources::Texture2D> twin;  // the graph texture
		std::shared_ptr<org::Resource> resource;          // twin's, kept by the views and the frames
		std::shared_ptr<org::PixelBuffer> image;          // the same resource, as a texture
		std::uint32_t width = 0, height = 0, mips = 1;
		// MakeTexture's: the exempt immutable class, never declared. Set before the texture is shared.
		bool exempt = false;
		// ImGui's: the atlas page it is a version of (kAtlasPages: none, a placeholder). Set before the texture is shared.
		std::size_t page = kAtlasPages;
		// The exempt class's reads: views[0] every level; views[1 + m] level m alone (with more than one level).
		std::vector<org::OwnedDescriptorBinding> views;
		std::vector<std::vector<std::byte>> pixels;  // render thread: MakeTexture's levels, until uploaded
		std::atomic<bool> uploaded{ false };         // a recorded upload pass copied its whole contents
	};

	namespace
	{
		/** @brief One copy into a texture: a rectangle of a level, from the frame's staging buffer. */
		struct Region
		{
			std::uint32_t mip = 0, x = 0, y = 0, width = 0, height = 0;
			std::uint64_t offset = 0;
			std::uint32_t rowPitch = 0;
		};

		struct Upload
		{
			std::shared_ptr<Texture> texture;
			bool whole = false;  // every level's whole contents: the texture counts as uploaded once a recorded pass copied them
			std::vector<Region> regions;
		};

		struct Draw
		{
			rhi::ScissorRect scissor;
			std::uint32_t texture = kNoIndex;  // a descriptor index, when own is null
			const Texture* own = nullptr;      // one of the backend's textures (kept by the frame's owners): exempt, or an atlas page
			int mip = -1;
			std::uint32_t indexCount = 0;
			std::uint32_t indexOffset = 0;   // bytes
			std::uint32_t vertexOffset = 0;  // bytes
		};

		/** @brief What one execution of the passes records: immutable once Commit stored it. */
		struct Frame
		{
			std::shared_ptr<const RasterProgram> program;
			std::shared_ptr<org::LatchBlock> staging;
			std::vector<Upload> uploads;
			std::vector<Draw> draws;
			std::uint32_t sampler = kNoIndex;
			float scale[2]{};
			float translate[2]{};
			std::uint32_t width = 0, height = 0;
			std::vector<std::shared_ptr<const void>> owners;  // the textures and descriptors the draws read
		};

		/** @brief The frames Commit stored, one per run; each pass takes its run's from its own slots. */
		using Slots = ::RenderGraph::RunSlots<Frame>;

		/**
		 * @brief The font atlas pages both passes declare: a fixed set of versioned textures. Each ImGui texture is a version of one
		 * page; a page no ImGui texture holds is its own 1x1 placeholder (never drawn). Every version is RGBA8 with one level, the
		 * pages' shape class. Made once, before the passes; the versions change on the render thread (Commit).
		 */
		struct Atlas
		{
			std::array<std::shared_ptr<org::VersionedTexture>, kAtlasPages> pages;
			std::array<std::shared_ptr<Texture>, kAtlasPages> placeholders;  // each page's own: a version's backing is its page's alone
		};

		/** @brief What the passes share with Commit: the run slots, and the atlas (once made). */
		struct Shared
		{
			std::shared_ptr<Slots> uploadSlots = std::make_shared<Slots>();
			std::shared_ptr<Slots> drawSlots = std::make_shared<Slots>();
			std::shared_ptr<const Atlas> atlas;  // render thread: set once, before any pass is made
		};

		/** @brief Host-visible staging buffers, reused once the frame that used one retired (its last owner gave it back). */
		struct StagingPool
		{
			std::mutex mutex;
			std::vector<std::shared_ptr<org::LatchBlock>> spare;
		};

		/** @brief A pending texture upload's pixels: where they are now, before Commit copies them. */
		struct PendingRegion
		{
			Region region;
			const std::byte* source = nullptr;
			std::uint32_t sourcePitch = 0;
		};

		struct PendingUpload
		{
			std::shared_ptr<Texture> texture;
			bool whole = false;
			std::vector<PendingRegion> regions;
		};

		struct State
		{
			bool installed = false;
			bool callbackLogged = false;
			std::unordered_map<std::uint64_t, std::shared_ptr<Texture>> textures;    // ImGui's, by ImTextureID
			std::vector<std::weak_ptr<Texture>> made;                                // MakeTexture's, until uploaded
			std::array<std::shared_ptr<Texture>, kAtlasPages> pages;                 // the ImGui texture each page holds
			bool pagesLogged = false;
			std::map<std::uint32_t, std::shared_ptr<const RasterProgram>> programs;  // by target format
			std::map<std::uint32_t, bool> failedPrograms;
			org::OwnedDescriptorBinding sampler;
			std::shared_ptr<StagingPool> pool = std::make_shared<StagingPool>();
			std::shared_ptr<Shared> shared = std::make_shared<Shared>();
		};

		State& Backend()
		{
			static State s_state;
			return s_state;
		}

		std::uint64_t IdOf(const Texture* a_texture) { return kOwnTag | reinterpret_cast<std::uintptr_t>(a_texture); }

		/** @brief A staging buffer of at least a_bytes, given back to the pool when its last owner lets it go. */
		std::shared_ptr<org::LatchBlock> AcquireStaging(const std::shared_ptr<StagingPool>& a_pool, std::uint64_t a_bytes)
		{
			std::shared_ptr<org::LatchBlock> block;
			{
				std::scoped_lock lock(a_pool->mutex);
				auto best = a_pool->spare.end();
				for (auto it = a_pool->spare.begin(); it != a_pool->spare.end(); ++it)
					if ((*it)->Stride() >= a_bytes && (best == a_pool->spare.end() || (*it)->Stride() < (*best)->Stride()))
						best = it;
				if (best != a_pool->spare.end()) {
					block = std::move(*best);
					a_pool->spare.erase(best);
				}
			}
			if (!block) {
				const std::uint64_t size = std::bit_ceil(std::max<std::uint64_t>(a_bytes, kMinimumStaging));
				if (size > 0xFFFFFFFFull)
					return nullptr;
				try {
					block = org::LatchBlock::CreateDedicated("cs.imgui.staging", static_cast<std::uint32_t>(size));
				} catch (const std::exception& e) {
					logger::error("[ORG] ImGui: a staging buffer of {} bytes could not be made: {}", size, e.what());
					return nullptr;
				}
			}
			// The pool is held weakly: after Shutdown a buffer coming back is released.
			std::weak_ptr<StagingPool> pool = a_pool;
			auto* raw = block.get();
			return std::shared_ptr<org::LatchBlock>(raw, [pool, block = std::move(block)](org::LatchBlock*) mutable {
				if (auto owner = pool.lock()) {
					std::scoped_lock lock(owner->mutex);
					if (owner->spare.size() < kSpareStaging)
						owner->spare.push_back(std::move(block));
				}
			});
		}

		/**
		 * @brief A persistent RGBA8 graph texture (GraphResources' twin, graph-only). a_views: with the views the exempt class reads
		 * through.
		 */
		std::shared_ptr<Texture> CreateTexture(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, const std::string& a_name, bool a_views)
		{
			if (!a_width || !a_height)
				return nullptr;
			auto texture = std::make_shared<Texture>();
			texture->width = a_width;
			texture->height = a_height;
			texture->mips = a_mips;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = a_mips;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			texture->twin = std::make_unique<GraphResources::Texture2D>(desc, a_name, GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
			auto pixels = std::dynamic_pointer_cast<org::PixelBuffer>(texture->twin->Graph());
			if (!pixels) {
				logger::error("[ORG] ImGui: the texture {} ({}x{}) could not be created", a_name, a_width, a_height);
				return nullptr;
			}
			texture->resource = texture->twin->Graph();
			texture->image = pixels;
			const std::uint32_t viewCount = a_views ? (a_mips > 1 ? a_mips + 1 : 1) : 0;
			for (std::uint32_t v = 0; v < viewCount; ++v) {
				rhi::SrvDesc srv{};
				srv.dimension = rhi::SrvDim::Texture2D;
				srv.formatOverride = rhi::Format::R8G8B8A8_UNorm;
				srv.tex2D.mostDetailedMip = v == 0 ? 0 : v - 1;
				srv.tex2D.mipLevels = v == 0 ? a_mips : 1;
				auto view = ShaderBindings::CreateShaderResourceView(pixels->GetAPIResource(), texture->resource, srv);
				if (!view)
					return nullptr;
				texture->views.push_back(std::move(view));
			}
			return texture;
		}

		/** @brief The atlas made (once per Shared): each page's placeholder as its first version. Render thread. False, logged. */
		bool EnsureAtlas(State& a_state)
		{
			if (a_state.shared->atlas)
				return true;
			auto atlas = std::make_shared<Atlas>();
			try {
				for (std::size_t page = 0; page < kAtlasPages; ++page) {
					atlas->placeholders[page] = CreateTexture(1, 1, 1, fmt::format("ImGui::AtlasPage {} placeholder", page), false);
					if (!atlas->placeholders[page])
						return false;
					atlas->pages[page] = org::VersionedTexture::Create(atlas->placeholders[page]->image, atlas->placeholders[page]);
				}
			} catch (const std::exception& e) {
				logger::error("[ORG] ImGui: the font atlas pages could not be made: {}", e.what());
				return false;
			}
			a_state.shared->atlas = std::move(atlas);
			return true;
		}

		/**
		 * @brief Page a_page's next version: a_texture, or its placeholder when null. The next preparation binds it: no graph build.
		 * Render thread, with the atlas made.
		 */
		bool SetPage(State& a_state, std::size_t a_page, const std::shared_ptr<Texture>& a_texture)
		{
			const auto& atlas = *a_state.shared->atlas;
			const auto& backing = a_texture ? a_texture : atlas.placeholders[a_page];
			try {
				atlas.pages[a_page]->Publish(backing->image, backing);
			} catch (const std::exception& e) {
				logger::error("[ORG] ImGui: atlas page {} could not take a new version: {}", a_page, e.what());
				return false;
			}
			a_state.pages[a_page] = a_texture;
			return true;
		}

		/** @brief One rectangle of ImGui's pixels to stage. */
		PendingRegion RegionOf(ImTextureData& a_texture, int a_x, int a_y, int a_width, int a_height)
		{
			PendingRegion region;
			region.region = { 0, static_cast<std::uint32_t>(a_x), static_cast<std::uint32_t>(a_y), static_cast<std::uint32_t>(a_width), static_cast<std::uint32_t>(a_height) };
			region.source = static_cast<const std::byte*>(a_texture.GetPixelsAt(a_x, a_y));
			region.sourcePitch = static_cast<std::uint32_t>(a_texture.GetPitch());
			return region;
		}

		void DestroyTexture(State& a_state, ImTextureData& a_texture)
		{
			// The frames still in flight own what they draw with, and their preparations the version they bound: the texture goes
			// when the last of them retired. Its page takes the placeholder.
			if (const auto found = a_state.textures.find(a_texture.GetTexID()); found != a_state.textures.end()) {
				if (const std::size_t page = found->second->page; page < kAtlasPages && a_state.pages[page] == found->second)
					SetPage(a_state, page, nullptr);
				a_state.textures.erase(found);
			}
			a_texture.SetTexID(ImTextureID_Invalid);
			a_texture.BackendUserData = nullptr;
			a_texture.SetStatus(ImTextureStatus_Destroyed);
		}

		/**
		 * @brief An ImGui texture request answered: created, updated by rectangle, or destroyed. A texture whose whole contents no
		 * recorded upload pass copied yet takes no rectangles: StageWholeTextures stages its whole contents, which hold them.
		 */
		void UpdateTexture(State& a_state, ImTextureData& a_texture, std::vector<PendingUpload>& a_uploads)
		{
			if (a_texture.Status == ImTextureStatus_WantCreate) {
				if (a_texture.Format != ImTextureFormat_RGBA32) {
					logger::error("[ORG] ImGui: texture {} is not RGBA32; not created", a_texture.UniqueID);
					return;
				}
				// A texture created again keeps its page (a new version of it); a new texture takes a free page.
				const auto previous = a_state.textures.find(a_texture.GetTexID());
				std::size_t page = previous != a_state.textures.end() ? previous->second->page : 0;
				if (previous == a_state.textures.end())
					while (page < kAtlasPages && a_state.pages[page])
						++page;
				if (page >= kAtlasPages) {
					if (!std::exchange(a_state.pagesLogged, true))
						logger::warn("[ORG] ImGui: texture {} needs more than {} atlas pages; it is not drawn while they are taken", a_texture.UniqueID, kAtlasPages);
					return;  // asked for again next frame
				}
				auto texture = CreateTexture(static_cast<std::uint32_t>(a_texture.Width), static_cast<std::uint32_t>(a_texture.Height), 1,
					fmt::format("ImGui::Texture {}", a_texture.UniqueID), false);
				if (!texture)
					return;  // asked for again next frame
				texture->page = page;
				if (!SetPage(a_state, page, texture))
					return;
				if (previous != a_state.textures.end())
					a_state.textures.erase(previous);
				const auto id = IdOf(texture.get());
				a_texture.SetTexID(id);
				a_texture.BackendUserData = texture.get();
				a_state.textures[id] = std::move(texture);
				a_texture.SetStatus(ImTextureStatus_OK);
			} else if (a_texture.Status == ImTextureStatus_WantUpdates) {
				const auto found = a_state.textures.find(a_texture.GetTexID());
				if (found == a_state.textures.end())
					return;
				if (found->second->uploaded.load(std::memory_order_acquire)) {
					PendingUpload upload{ found->second };
					for (const ImTextureRect& r : a_texture.Updates)
						upload.regions.push_back(RegionOf(a_texture, r.x, r.y, r.w, r.h));
					a_uploads.push_back(std::move(upload));
				}
				a_texture.SetStatus(ImTextureStatus_OK);
			}
			if (a_texture.Status == ImTextureStatus_WantDestroy && a_texture.UnusedFrames > 0)
				DestroyTexture(a_state, a_texture);
		}

		/**
		 * @brief The whole contents of every texture no recorded upload pass copied yet, staged again each frame until one did (an
		 * upload pass copies into a page only when the run binds that version). MakeTexture's textures leave the list once uploaded.
		 */
		void StageWholeTextures(State& a_state, ImDrawData& a_drawData, std::vector<PendingUpload>& a_uploads)
		{
			if (a_drawData.Textures)
				for (ImTextureData* data : *a_drawData.Textures) {
					if (data->Status != ImTextureStatus_OK || !data->BackendUserData || !data->GetPixels())
						continue;
					const auto found = a_state.textures.find(data->GetTexID());
					if (found == a_state.textures.end() || found->second->uploaded.load(std::memory_order_acquire))
						continue;
					PendingUpload upload{ found->second, true };
					upload.regions.push_back(RegionOf(*data, 0, 0, data->Width, data->Height));
					a_uploads.push_back(std::move(upload));
				}
			std::erase_if(a_state.made, [&](const std::weak_ptr<Texture>& a_weak) {
				const auto texture = a_weak.lock();
				if (!texture)
					return true;
				if (texture->uploaded.load(std::memory_order_acquire)) {
					// In the shader-read state from now on, and never written again.
					texture->pixels.clear();
					texture->pixels.shrink_to_fit();
					return true;
				}
				PendingUpload upload{ texture, true };
				for (std::uint32_t m = 0; m < texture->pixels.size(); ++m) {
					const std::uint32_t width = std::max(1u, texture->width >> m), height = std::max(1u, texture->height >> m);
					upload.regions.push_back({ { m, 0, 0, width, height }, texture->pixels[m].data(), width * 4 });
				}
				a_uploads.push_back(std::move(upload));
				return false;
			});
		}

		const RasterProgram* ProgramFor(State& a_state, std::uint32_t a_format)
		{
			if (const auto found = a_state.programs.find(a_format); found != a_state.programs.end())
				return found->second.get();
			if (a_state.failedPrograms.contains(a_format))
				return nullptr;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			RasterProgram::Desc desc{};
			desc.vertexSource = kShader;
			desc.vertexEntry = L"VSMain";
			desc.pixelSource = kShader;
			desc.pixelEntry = L"PSMain";
			desc.constantWords = kConstantWords;
			// imgui_impl_dx11's blending: colour over, alpha accumulated towards opaque.
			RasterProgram::Target target{ .format = static_cast<rhi::Format>(a_format) };
			target.blend.enable = true;
			target.blend.srcColor = rhi::BlendFactor::SrcAlpha;
			target.blend.dstColor = rhi::BlendFactor::InvSrcAlpha;
			target.blend.srcAlpha = rhi::BlendFactor::One;
			target.blend.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
			desc.targets = { target };
			auto program = RasterProgram::Load(host->Device(), desc);
			if (!program) {
				a_state.failedPrograms[a_format] = true;
				logger::error("[ORG] ImGui: the pipeline for target format {} could not be made", a_format);
				return nullptr;
			}
			auto* raw = program.get();
			a_state.programs[a_format] = std::move(program);
			return raw;
		}

		/** @brief Whether a run bound a_texture to its page: the handle its page's declaration resolves to is the texture's. */
		bool Bound(const rhi::ResourceHandle& a_bound, const Texture& a_texture)
		{
			const auto own = a_texture.image->GetAPIResource().GetHandle();
			return a_bound.index == own.index && a_bound.generation == own.generation;
		}

		struct PassFrame
		{
			std::shared_ptr<const Slots> slots;
		};

		/**
		 * @brief An exempt texture's transfer barrier: into a copy destination before its whole contents are copied (what it held
		 * is dropped), or from there into the shader-read state it rests in after.
		 */
		rhi::TextureBarrier ExemptBarrier(const Texture& a_texture, bool a_toCopy)
		{
			rhi::TextureBarrier barrier{};
			barrier.texture = a_texture.image->GetAPIResource().GetHandle();
			barrier.range = { 0, a_texture.mips, 0, 1, 0, 1 };
			if (a_toCopy) {
				barrier.afterSync = rhi::ResourceSyncState::Copy;
				barrier.afterAccess = rhi::ResourceAccessType::CopyDest;
				barrier.afterLayout = rhi::ResourceLayout::CopyDest;
				barrier.discard = true;
			} else {
				barrier.beforeSync = rhi::ResourceSyncState::Copy;
				barrier.beforeAccess = rhi::ResourceAccessType::CopyDest;
				barrier.beforeLayout = rhi::ResourceLayout::CopyDest;
				barrier.afterSync = rhi::ResourceSyncState::All;
				barrier.afterAccess = rhi::ResourceAccessType::ShaderResource;
				barrier.afterLayout = rhi::ResourceLayout::ShaderResource;
			}
			return barrier;
		}

		struct UploadBindings
		{
			std::vector<org::ResourceBindingToken> pages;  // by page
		};

		/**
		 * @brief The frame's texture copies from its staging buffer. A static pass: it declares every atlas page as a copy
		 * destination (the graph makes the barriers) and records nothing on a run whose frame has no upload (its run slot is empty).
		 * A copy into a page whose bound version is not the texture's is left out; Commit stages that texture's whole contents
		 * again until a copy of them was recorded. An exempt texture is not declared: its whole contents are copied between its own
		 * transfer barriers, and it rests in the shader-read state after. Both passes declare every page, so the graph orders the
		 * draw pass after this one, and a draw of the same run reads what it copied.
		 */
		class UploadPass final : public org::TypedRenderGraphPass<UploadPass, PassFrame, UploadBindings>
		{
		public:
			UploadPass(std::shared_ptr<const Shared> a_shared, std::shared_ptr<const Atlas> a_atlas) :
				shared(std::move(a_shared)), atlas(std::move(a_atlas)) {}

			UploadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				UploadBindings bindings;
				for (const auto& page : atlas->pages)
					bindings.pages.push_back(a_builder.CopyDestination(*page).Resource());
				return bindings;
			}

			PassFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { shared->uploadSlots }; }

			static void Record(const UploadBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots)
					return;
				const auto frame = a_pass.slots->Take(a_recording.Run());
				if (!frame || !frame->staging || frame->uploads.empty())
					return;
				// The staging buffer and the textures stay until this recording's GPU work retires.
				a_recording.Retain(frame);
				auto& commands = a_recording.Commands();
				const auto staging = frame->staging->Resource()->GetAPIResource().GetHandle();
				for (const auto& upload : frame->uploads) {
					const bool exempt = upload.texture->exempt;
					rhi::ResourceHandle texture{};
					if (exempt) {
						const auto barrier = ExemptBarrier(*upload.texture, true);
						commands.Barriers(rhi::BarrierBatch{ { &barrier, 1 }, {}, {} });
						texture = barrier.texture;
					} else {
						const std::size_t page = upload.texture->page;
						if (page >= a_bindings.pages.size())
							continue;
						texture = a_recording.Resolve(a_bindings.pages[page]).GetHandle();
						if (!Bound(texture, *upload.texture))
							continue;
					}
					for (const auto& region : upload.regions) {
						rhi::BufferTextureCopyFootprint copy{};
						copy.texture = texture;
						copy.buffer = staging;
						copy.mip = region.mip;
						copy.arraySlice = 0;
						copy.x = region.x;
						copy.y = region.y;
						copy.footprint = { region.offset, region.rowPitch, region.width, region.height, 1 };
						commands.CopyBufferToTexture(copy);
					}
					if (exempt) {
						const auto barrier = ExemptBarrier(*upload.texture, false);
						commands.Barriers(rhi::BarrierBatch{ { &barrier, 1 }, {}, {} });
					}
					if (upload.whole)
						upload.texture->uploaded.store(true, std::memory_order_release);
				}
			}

		private:
			std::shared_ptr<const Shared> shared;
			std::shared_ptr<const Atlas> atlas;
		};

		struct DrawBindings
		{
			org::DeclaredViewToken target;
			std::vector<org::DeclaredViewToken> pages;  // by page
		};

		/**
		 * @brief The frame's draws onto the target. A static pass: it declares every atlas page as a shader resource (after the
		 * upload pass) and reads ImGui's textures through those views; a draw of a texture its page does not bind this run is left
		 * out. Exempt textures are read through their own views (Commit draws one only once its upload was recorded, or in the run
		 * that uploads it).
		 */
		class OverlayPass final : public org::TypedRenderGraphPass<OverlayPass, PassFrame, DrawBindings>
		{
		public:
			OverlayPass(std::shared_ptr<org::ExternalTextureResource> a_target, std::shared_ptr<const Shared> a_shared, std::shared_ptr<const Atlas> a_atlas) :
				target(std::move(a_target)), shared(std::move(a_shared)), atlas(std::move(a_atlas)) {}

			DrawBindings Declare(org::PassBuilder& a_builder)
			{
				DrawBindings bindings{ a_builder.RenderTarget(target).View(), {} };
				for (const auto& page : atlas->pages)
					bindings.pages.push_back(a_builder.ShaderResource(*page).View());
				return bindings;
			}

			PassFrame Prepare(const DrawBindings&, const org::PassPrepareContext&) const { return { shared->drawSlots }; }

			static void Record(const DrawBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots)
					return;
				const auto frame = a_pass.slots->Take(a_recording.Run());
				if (!frame || !frame->program || !frame->staging)
					return;
				// The staging buffer, textures and descriptors stay until this recording's GPU work retires.
				a_recording.Retain(frame);
				if (frame->draws.empty())
					return;

				auto& commands = a_recording.Commands();
				rhi::ColorAttachment colour{};
				colour.rtv = a_recording.View(a_bindings.target);
				colour.resource = a_recording.Resolve(a_bindings.target).GetHandle();
				colour.loadOp = rhi::LoadOp::Load;
				colour.storeOp = rhi::StoreOp::Store;
				rhi::PassBeginInfo begin{};
				begin.colors = { &colour, 1 };
				begin.width = frame->width;
				begin.height = frame->height;
				begin.debugName = "CS ImGui";
				commands.BeginPass(begin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame->program->layout->GetHandle());
				commands.BindPipeline(frame->program->Pipeline());
				DrawConstants constants{};
				constants.geometryIndex = frame->staging->SrvIndex();
				constants.indexSize = sizeof(ImDrawIdx);
				constants.samplerIndex = frame->sampler;
				std::memcpy(constants.scale, frame->scale, sizeof(constants.scale));
				std::memcpy(constants.translate, frame->translate, sizeof(constants.translate));
				// What each page binds this run.
				std::array<rhi::ResourceHandle, kAtlasPages> bound{};
				for (std::size_t page = 0; page < a_bindings.pages.size() && page < kAtlasPages; ++page)
					bound[page] = a_recording.Resolve(a_bindings.pages[page]).GetHandle();
				for (const auto& draw : frame->draws) {
					std::uint32_t textureIndex = draw.texture;
					if (draw.own && draw.own->exempt) {
						const auto& views = draw.own->views;
						const std::size_t view = draw.mip < 0 || views.size() == 1 ? 0 : std::min<std::size_t>(std::size_t(draw.mip) + 1, views.size() - 1);
						textureIndex = views[view].Index();
					} else if (draw.own) {
						const std::size_t page = draw.own->page;
						if (page >= a_bindings.pages.size() || !Bound(bound[page], *draw.own))
							continue;
						textureIndex = a_recording.Index(a_bindings.pages[page]);
					}
					commands.SetScissors({ &draw.scissor, 1 });
					constants.vertexOffset = draw.vertexOffset;
					constants.indexOffset = draw.indexOffset;
					constants.textureIndex = textureIndex;
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
					commands.Draw(draw.indexCount, 1, 0, 0);
				}
				commands.EndPass();
			}

		private:
			std::shared_ptr<org::ExternalTextureResource> target;
			std::shared_ptr<const Shared> shared;
			std::shared_ptr<const Atlas> atlas;
		};
	}

	void Init()
	{
		auto& state = Backend();
		auto& io = ImGui::GetIO();
		io.BackendRendererName = kBackendName;
		io.BackendRendererUserData = &state;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
		state.installed = true;
		logger::info("[ORG] ImGui: drawn by the render graph ({})", kBackendName);
	}

	void InvalidateTextures()
	{
		auto& state = Backend();
		if (!state.installed)
			return;
		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
			if (texture->RefCount == 1 && texture->BackendUserData)
				DestroyTexture(state, *texture);
		state.textures.clear();
		if (state.shared->atlas)
			for (std::size_t page = 0; page < kAtlasPages; ++page)
				if (state.pages[page])
					SetPage(state, page, nullptr);
	}

	void Shutdown()
	{
		auto& state = Backend();
		if (!state.installed)
			return;
		InvalidateTextures();
		state.made.clear();
		state.programs.clear();
		state.failedPrograms.clear();
		state.sampler = {};
		state.shared = std::make_shared<Shared>();
		state.pages = {};
		state.pagesLogged = false;
		state.pool = std::make_shared<StagingPool>();
		auto& io = ImGui::GetIO();
		io.BackendRendererName = nullptr;
		io.BackendRendererUserData = nullptr;
		io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
		state.installed = false;
	}

	bool Installed() { return Backend().installed; }

	bool IsOwnTexture(std::uint64_t a_texture) { return (a_texture & kOwnTag) != 0; }

	std::shared_ptr<Texture> MakeTexture(std::uint32_t a_width, std::uint32_t a_height, std::span<const std::span<const std::byte>> a_levels, const char* a_name)
	{
		auto& state = Backend();
		if (a_levels.empty())
			return nullptr;
		for (std::uint32_t m = 0; m < a_levels.size(); ++m) {
			const std::uint32_t width = std::max(1u, a_width >> m), height = std::max(1u, a_height >> m);
			if (a_levels[m].size() < std::size_t(width) * height * 4) {
				logger::error("[ORG] ImGui: {} level {} has {} bytes, not {}", a_name, m, a_levels[m].size(), std::size_t(width) * height * 4);
				return nullptr;
			}
		}
		auto texture = CreateTexture(a_width, a_height, static_cast<std::uint32_t>(a_levels.size()), a_name, true);
		if (!texture)
			return nullptr;
		// Uploaded once by the upload pass between its own barriers, then only read: the exempt class, never declared.
		texture->exempt = true;
		texture->resource->SetGraphOwnership(org::Resource::GraphOwnership::ExternalImmutableShaderResource);
		if (RenderGraph::UndeclaredAccess::On())
			RenderGraph::UndeclaredAccess::NoteImmutable(texture->resource);
		for (const auto& level : a_levels)
			texture->pixels.emplace_back(level.begin(), level.end());
		state.made.push_back(texture);
		return texture;
	}

	Binding BindingOf(const std::shared_ptr<Texture>& a_texture, int a_mip)
	{
		// An exempt texture is read through its own views; ImGui's are read through the passes' declarations.
		if (!a_texture || (a_texture->exempt && a_texture->views.empty()))
			return {};
		return { kNoIndex, a_texture, a_texture, a_mip };
	}

	bool Prepare(std::uint32_t a_format) { return ProgramFor(Backend(), a_format) != nullptr; }

	bool Commit(ImDrawData* a_drawData, const Target& a_target, const Resolver& a_resolve)
	{
		auto& state = Backend();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!state.installed || !host || !a_drawData)
			return false;
		const RasterProgram* program = ProgramFor(state, a_target.format);
		if (!program || !EnsureAtlas(state))
			return false;
		if (!state.sampler) {
			// imgui_impl_dx11's: bilinear, clamped, the top level only (an icon's view starts at the level it wants).
			rhi::SamplerDesc sampler{};
			sampler.maxLod = 0.0f;
			state.sampler = ShaderBindings::CreateSampler(sampler);
			if (!state.sampler)
				return false;
		}

		// ImGui's texture requests.
		std::vector<PendingUpload> pending;
		if (a_drawData->Textures)
			for (ImTextureData* texture : *a_drawData->Textures)
				if (texture->Status != ImTextureStatus_OK)
					UpdateTexture(state, *texture, pending);

		auto frame = std::make_shared<Frame>();
		// Every texture the draws name, before staging: the resolver may make textures (MakeTexture) whose first upload is this frame's.
		struct Resolved
		{
			std::uint32_t index = kNoIndex;
			const Texture* own = nullptr;
			int mip = -1;
		};
		std::unordered_map<std::uint64_t, Resolved> resolved;
		for (const ImDrawList* list : a_drawData->CmdLists)
			for (const ImDrawCmd& command : list->CmdBuffer) {
				if (command.UserCallback || !command.ElemCount)
					continue;
				const std::uint64_t id = command.GetTexID();
				if (resolved.contains(id))
					continue;
				Binding binding;
				if (IsOwnTexture(id)) {
					if (const auto texture = state.textures.find(id); texture != state.textures.end())
						binding = BindingOf(texture->second, -1);
				} else if (a_resolve) {
					binding = a_resolve(id);
				}
				resolved.emplace(id, Resolved{ binding.index, binding.texture.get(), binding.mip });
				if (binding.owner)
					frame->owners.push_back(std::move(binding.owner));
			}
		// The whole contents of the textures not copied yet (MakeTexture's included).
		StageWholeTextures(state, *a_drawData, pending);
		// What the draws may read: a texture whose whole contents a recorded upload pass copied, or that this run's upload pass
		// copies before them. Nothing reads a texture before its first upload.
		std::unordered_set<const Texture*> staged;
		for (const auto& upload : pending)
			if (upload.whole)
				staged.insert(upload.texture.get());
		for (auto& entry : resolved)
			if (const Texture* own = entry.second.own; own && !own->uploaded.load(std::memory_order_acquire) && !staged.contains(own))
				entry.second = {};

		// The staging buffer: vertices, indices, then the uploads' rows.
		const std::uint64_t vertexBytes = std::uint64_t(a_drawData->TotalVtxCount) * sizeof(ImDrawVert);
		const std::uint64_t indexBase = AlignUp(vertexBytes, 4);
		std::uint64_t bytes = AlignUp(indexBase + std::uint64_t(a_drawData->TotalIdxCount) * sizeof(ImDrawIdx), kPlacementAlignment);
		for (auto& upload : pending)
			for (auto& region : upload.regions) {
				region.region.rowPitch = static_cast<std::uint32_t>(AlignUp(std::uint64_t(region.region.width) * 4, kRowAlignment));
				region.region.offset = bytes;
				bytes = AlignUp(bytes + std::uint64_t(region.region.rowPitch) * region.region.height, kPlacementAlignment);
			}

		frame->staging = AcquireStaging(state.pool, std::max<std::uint64_t>(bytes, 4));
		if (!frame->staging)
			return false;
		const auto span = frame->staging->Bytes();
		std::byte* const base = span.data();
		for (auto& upload : pending) {
			Upload staged{ upload.texture, upload.whole, {} };
			for (const auto& region : upload.regions) {
				const std::size_t rowBytes = std::size_t(region.region.width) * 4;
				for (std::uint32_t row = 0; row < region.region.height; ++row)
					std::memcpy(base + region.region.offset + std::size_t(row) * region.region.rowPitch, region.source + std::size_t(row) * region.sourcePitch, rowBytes);
				staged.regions.push_back(region.region);
			}
			frame->uploads.push_back(std::move(staged));
		}

		const ImVec2 position = a_drawData->DisplayPos;
		const ImVec2 size = a_drawData->DisplaySize;
		const ImVec2 scale = a_drawData->FramebufferScale;
		frame->program = state.programs[a_target.format];
		frame->sampler = state.sampler.Index();
		frame->owners.push_back(state.sampler.Owner());
		frame->width = a_target.width;
		frame->height = a_target.height;
		// imgui_impl_dx11's projection: the display rectangle onto clip space, y down.
		frame->scale[0] = 2.0f / size.x;
		frame->scale[1] = -2.0f / size.y;
		frame->translate[0] = -1.0f - position.x * frame->scale[0];
		frame->translate[1] = 1.0f - position.y * frame->scale[1];

		std::uint64_t vertexCursor = 0, indexCursor = indexBase;
		std::uint32_t globalVertex = 0, globalIndex = 0;
		for (const ImDrawList* list : a_drawData->CmdLists) {
			std::memcpy(base + vertexCursor, list->VtxBuffer.Data, std::size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert));
			std::memcpy(base + indexCursor, list->IdxBuffer.Data, std::size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
			vertexCursor += std::uint64_t(list->VtxBuffer.Size) * sizeof(ImDrawVert);
			indexCursor += std::uint64_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
			for (const ImDrawCmd& command : list->CmdBuffer) {
				if (command.UserCallback) {
					if (command.UserCallback != ImDrawCallback_ResetRenderState && !std::exchange(state.callbackLogged, true))
						logger::warn("[ORG] ImGui: a draw callback was skipped (callbacks are D3D11 code); the draws around it are drawn");
					continue;
				}
				const float minX = std::max((command.ClipRect.x - position.x) * scale.x, 0.0f);
				const float minY = std::max((command.ClipRect.y - position.y) * scale.y, 0.0f);
				const float maxX = std::min((command.ClipRect.z - position.x) * scale.x, static_cast<float>(a_target.width));
				const float maxY = std::min((command.ClipRect.w - position.y) * scale.y, static_cast<float>(a_target.height));
				if (maxX <= minX || maxY <= minY || !command.ElemCount)
					continue;
				const auto found = resolved.find(command.GetTexID());
				if (found == resolved.end() || (!found->second.own && found->second.index == kNoIndex))
					continue;
				Draw draw;
				// As D3D11 truncates its scissor rectangle's corners.
				draw.scissor.x = static_cast<std::int32_t>(minX);
				draw.scissor.y = static_cast<std::int32_t>(minY);
				draw.scissor.width = static_cast<std::uint32_t>(static_cast<std::int32_t>(maxX) - draw.scissor.x);
				draw.scissor.height = static_cast<std::uint32_t>(static_cast<std::int32_t>(maxY) - draw.scissor.y);
				if (!draw.scissor.width || !draw.scissor.height)
					continue;
				draw.texture = found->second.index;
				draw.own = found->second.own;
				draw.mip = found->second.mip;
				draw.indexCount = command.ElemCount;
				draw.indexOffset = static_cast<std::uint32_t>(indexBase + std::uint64_t(globalIndex + command.IdxOffset) * sizeof(ImDrawIdx));
				draw.vertexOffset = static_cast<std::uint32_t>(std::uint64_t(globalVertex + command.VtxOffset) * sizeof(ImDrawVert));
				frame->draws.push_back(draw);
			}
			globalVertex += static_cast<std::uint32_t>(list->VtxBuffer.Size);
			globalIndex += static_cast<std::uint32_t>(list->IdxBuffer.Size);
		}
		const auto run = host->CurrentRun();
		state.shared->uploadSlots->Store(run, frame);
		state.shared->drawSlots->Store(run, std::move(frame));
		return true;
	}

	std::shared_ptr<org::RenderPass> MakePass(std::shared_ptr<org::ExternalTextureResource> a_target)
	{
		auto& state = Backend();
		if (!a_target || !RenderGraphRuntime::Get().Host() || !EnsureAtlas(state))
			return nullptr;
		return std::make_shared<OverlayPass>(std::move(a_target), state.shared, state.shared->atlas);
	}

	std::shared_ptr<org::RenderPass> MakeUploadPass()
	{
		auto& state = Backend();
		if (!RenderGraphRuntime::Get().Host() || !EnsureAtlas(state))
			return nullptr;
		return std::make_shared<UploadPass>(state.shared, state.shared->atlas);
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/ImGuiBasicRHI.h"

namespace ImGuiBasicRHI
{
	class Texture
	{};
	void Init() {}
	void Shutdown() {}
	bool Installed() { return false; }
	void InvalidateTextures() {}
	bool IsOwnTexture(std::uint64_t) { return false; }
	std::shared_ptr<Texture> MakeTexture(std::uint32_t, std::uint32_t, std::span<const std::span<const std::byte>>, const char*) { return nullptr; }
	Binding BindingOf(const std::shared_ptr<Texture>&, int) { return {}; }
	bool Prepare(std::uint32_t) { return false; }
	bool Commit(ImDrawData*, const Target&, const Resolver&) { return false; }
	std::shared_ptr<org::RenderPass> MakePass(std::shared_ptr<org::ExternalTextureResource>) { return nullptr; }
	std::shared_ptr<org::RenderPass> MakeUploadPass() { return nullptr; }
}

#endif
