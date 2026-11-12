#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ImGuiBasicRHI.h"
#	include "RenderGraph/FrameStaging.h"

#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>

#	include <algorithm>
#	include <atomic>
#	include <bit>
#	include <cstring>
#	include <map>
#	include <mutex>
#	include <unordered_map>
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
		std::shared_ptr<rhi::ResourcePtr> resource;  // shared with its views, which own it
		std::uint32_t width = 0, height = 0, mips = 1;
		// views[0]: every level; views[1 + m]: level m alone (with more than one level).
		std::vector<org::OwnedDescriptorBinding> views;
		// Render thread: the first upload has been staged (it moves the texture out of its undefined first layout).
		bool staged = false;
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
			bool first = false;  // from the undefined layout; else from the shader-read one
			std::vector<Region> regions;
		};

		struct Draw
		{
			rhi::ScissorRect scissor;
			std::uint32_t texture = kNoIndex;
			std::uint32_t indexCount = 0;
			std::uint32_t indexOffset = 0;   // bytes
			std::uint32_t vertexOffset = 0;  // bytes
		};

		/** @brief What one execution of the pass records: immutable once Commit stored it. */
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

		/** @brief The frames Commit stored, one per run; Record takes its run's. */
		using Slots = ::RenderGraph::RunSlots<Frame>;

		/** @brief Host-visible staging buffers, reused once the frame that used one retired (its last owner gave it back). */
		struct StagingPool
		{
			std::mutex mutex;
			std::vector<std::shared_ptr<org::LatchBlock>> spare;
		};

		/** @brief A pending ImGui texture request's pixels: where they are now, before Commit copies them. */
		struct PendingRegion
		{
			Region region;
			const std::byte* source = nullptr;
			std::uint32_t sourcePitch = 0;
		};

		struct PendingUpload
		{
			std::shared_ptr<Texture> texture;
			std::vector<PendingRegion> regions;
			std::vector<std::vector<std::byte>> owned;  // MakeTexture's pixels, until they are staged
		};

		struct State
		{
			bool installed = false;
			bool callbackLogged = false;
			std::unordered_map<std::uint64_t, std::shared_ptr<Texture>> textures;    // ImGui's, by ImTextureID
			std::map<std::uint32_t, std::shared_ptr<const RasterProgram>> programs;  // by target format
			std::map<std::uint32_t, bool> failedPrograms;
			org::OwnedDescriptorBinding sampler;
			std::shared_ptr<StagingPool> pool = std::make_shared<StagingPool>();
			std::shared_ptr<Slots> slots;
			std::vector<PendingUpload> pending;  // MakeTexture's first uploads, for the next Commit
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

		std::shared_ptr<Texture> CreateTexture(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, const char* a_name)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !a_width || !a_height)
				return nullptr;
			auto texture = std::make_shared<Texture>();
			texture->width = a_width;
			texture->height = a_height;
			texture->mips = a_mips;
			rhi::ResourceDesc desc{};
			desc.type = rhi::ResourceType::Texture2D;
			desc.debugName = a_name;
			desc.texture.format = rhi::Format::R8G8B8A8_UNorm;
			desc.texture.width = a_width;
			desc.texture.height = a_height;
			desc.texture.mipLevels = static_cast<std::uint16_t>(a_mips);
			texture->resource = std::make_shared<rhi::ResourcePtr>();
			if (host->Device().CreateCommittedResource(desc, *texture->resource) != rhi::Result::Ok || !*texture->resource) {
				logger::error("[ORG] ImGui: the texture {} ({}x{}) could not be created", a_name, a_width, a_height);
				return nullptr;
			}
			const std::uint32_t viewCount = a_mips > 1 ? a_mips + 1 : 1;
			for (std::uint32_t v = 0; v < viewCount; ++v) {
				rhi::SrvDesc srv{};
				srv.dimension = rhi::SrvDim::Texture2D;
				srv.formatOverride = rhi::Format::R8G8B8A8_UNorm;
				srv.tex2D.mostDetailedMip = v == 0 ? 0 : v - 1;
				srv.tex2D.mipLevels = v == 0 ? a_mips : 1;
				auto view = ShaderBindings::CreateShaderResourceView(texture->resource->Get(), texture->resource, srv);
				if (!view)
					return nullptr;
				texture->views.push_back(std::move(view));
			}
			return texture;
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
			// The frames still in flight own what they draw with: the texture goes when the last of them retired.
			a_state.textures.erase(a_texture.GetTexID());
			a_texture.SetTexID(ImTextureID_Invalid);
			a_texture.BackendUserData = nullptr;
			a_texture.SetStatus(ImTextureStatus_Destroyed);
		}

		/** @brief An ImGui texture request answered: created (its pixels to stage), updated by rectangle, or destroyed. */
		void UpdateTexture(State& a_state, ImTextureData& a_texture, std::vector<PendingUpload>& a_uploads)
		{
			if (a_texture.Status == ImTextureStatus_WantCreate) {
				if (a_texture.Format != ImTextureFormat_RGBA32) {
					logger::error("[ORG] ImGui: texture {} is not RGBA32; not created", a_texture.UniqueID);
					return;
				}
				auto texture = CreateTexture(static_cast<std::uint32_t>(a_texture.Width), static_cast<std::uint32_t>(a_texture.Height), 1,
					fmt::format("cs.imgui.texture.{}", a_texture.UniqueID).c_str());
				if (!texture)
					return;  // asked for again next frame
				PendingUpload upload{ texture };
				upload.regions.push_back(RegionOf(a_texture, 0, 0, a_texture.Width, a_texture.Height));
				a_uploads.push_back(std::move(upload));
				const auto id = IdOf(texture.get());
				a_texture.SetTexID(id);
				a_texture.BackendUserData = texture.get();
				a_state.textures[id] = std::move(texture);
				a_texture.SetStatus(ImTextureStatus_OK);
			} else if (a_texture.Status == ImTextureStatus_WantUpdates) {
				const auto found = a_state.textures.find(a_texture.GetTexID());
				if (found == a_state.textures.end())
					return;
				PendingUpload upload{ found->second };
				for (const ImTextureRect& r : a_texture.Updates)
					upload.regions.push_back(RegionOf(a_texture, r.x, r.y, r.w, r.h));
				a_uploads.push_back(std::move(upload));
				a_texture.SetStatus(ImTextureStatus_OK);
			}
			if (a_texture.Status == ImTextureStatus_WantDestroy && a_texture.UnusedFrames > 0)
				DestroyTexture(a_state, a_texture);
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

		/** @brief The frame's textures' copies and their barriers, before the draws (the graph does not track these textures). */
		void RecordUploads(const Frame& a_frame, rhi::CommandList& a_commands)
		{
			if (a_frame.uploads.empty())
				return;
			std::vector<rhi::TextureBarrier> barriers;
			barriers.reserve(a_frame.uploads.size());
			for (const auto& upload : a_frame.uploads) {
				rhi::TextureBarrier barrier{};
				barrier.texture = (*upload.texture->resource)->GetHandle();
				barrier.range.mipCount = upload.texture->mips;
				barrier.beforeSync = upload.first ? rhi::ResourceSyncState::None : rhi::ResourceSyncState::PixelShading;
				barrier.afterSync = rhi::ResourceSyncState::Copy;
				barrier.beforeAccess = upload.first ? rhi::ResourceAccessType::None : rhi::ResourceAccessType::ShaderResource;
				barrier.afterAccess = rhi::ResourceAccessType::CopyDest;
				barrier.beforeLayout = upload.first ? rhi::ResourceLayout::Undefined : rhi::ResourceLayout::ShaderResource;
				barrier.afterLayout = rhi::ResourceLayout::CopyDest;
				barrier.discard = upload.first;
				barriers.push_back(barrier);
			}
			a_commands.Barriers({ .textures = { barriers.data(), static_cast<std::uint32_t>(barriers.size()) } });
			const auto staging = a_frame.staging->Resource()->GetAPIResource().GetHandle();
			for (const auto& upload : a_frame.uploads)
				for (const auto& region : upload.regions) {
					rhi::BufferTextureCopyFootprint copy{};
					copy.texture = (*upload.texture->resource)->GetHandle();
					copy.buffer = staging;
					copy.mip = region.mip;
					copy.arraySlice = 0;
					copy.x = region.x;
					copy.y = region.y;
					copy.footprint = { region.offset, region.rowPitch, region.width, region.height, 1 };
					a_commands.CopyBufferToTexture(copy);
				}
			for (auto& barrier : barriers) {
				barrier.beforeSync = rhi::ResourceSyncState::Copy;
				barrier.afterSync = rhi::ResourceSyncState::PixelShading;
				barrier.beforeAccess = rhi::ResourceAccessType::CopyDest;
				barrier.afterAccess = rhi::ResourceAccessType::ShaderResource;
				barrier.beforeLayout = rhi::ResourceLayout::CopyDest;
				barrier.afterLayout = rhi::ResourceLayout::ShaderResource;
				barrier.discard = false;
			}
			a_commands.Barriers({ .textures = { barriers.data(), static_cast<std::uint32_t>(barriers.size()) } });
		}

		struct PassBindings
		{
			org::DeclaredViewToken target;
		};

		struct PassFrame
		{
			std::shared_ptr<const Slots> slots;
		};

		class OverlayPass final : public org::TypedRenderGraphPass<OverlayPass, PassFrame, PassBindings>
		{
		public:
			OverlayPass(std::shared_ptr<org::ExternalTextureResource> a_target, std::shared_ptr<const Slots> a_slots) :
				target(std::move(a_target)), slots(std::move(a_slots)) {}

			PassBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(target).View() }; }

			PassFrame Prepare(const PassBindings&, const org::PassPrepareContext&) const { return { slots }; }

			static void Record(const PassBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots)
					return;
				const auto frame = a_pass.slots->Take(a_recording.Run());
				if (!frame || !frame->program || !frame->staging)
					return;
				// The staging buffer, textures and descriptors stay until this recording's GPU work retires.
				a_recording.Retain(frame);
				auto& commands = a_recording.Commands();
				RecordUploads(*frame, commands);
				if (frame->draws.empty())
					return;

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
				for (const auto& draw : frame->draws) {
					commands.SetScissors({ &draw.scissor, 1 });
					constants.vertexOffset = draw.vertexOffset;
					constants.indexOffset = draw.indexOffset;
					constants.textureIndex = draw.texture;
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
					commands.Draw(draw.indexCount, 1, 0, 0);
				}
				commands.EndPass();
			}

		private:
			std::shared_ptr<org::ExternalTextureResource> target;
			std::shared_ptr<const Slots> slots;
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
	}

	void Shutdown()
	{
		auto& state = Backend();
		if (!state.installed)
			return;
		InvalidateTextures();
		state.pending.clear();
		state.programs.clear();
		state.failedPrograms.clear();
		state.sampler = {};
		state.slots.reset();
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
		auto texture = CreateTexture(a_width, a_height, static_cast<std::uint32_t>(a_levels.size()), a_name);
		if (!texture)
			return nullptr;
		PendingUpload upload{ texture };
		for (std::uint32_t m = 0; m < a_levels.size(); ++m) {
			const std::uint32_t width = std::max(1u, a_width >> m), height = std::max(1u, a_height >> m);
			if (a_levels[m].size() < std::size_t(width) * height * 4) {
				logger::error("[ORG] ImGui: {} level {} has {} bytes, not {}", a_name, m, a_levels[m].size(), std::size_t(width) * height * 4);
				return nullptr;
			}
			auto& bytes = upload.owned.emplace_back(a_levels[m].begin(), a_levels[m].end());
			upload.regions.push_back({ { m, 0, 0, width, height }, bytes.data(), width * 4 });
		}
		state.pending.push_back(std::move(upload));
		return texture;
	}

	Binding BindingOf(const std::shared_ptr<Texture>& a_texture, int a_mip)
	{
		if (!a_texture || a_texture->views.empty())
			return {};
		const std::size_t view = a_mip < 0 || a_texture->views.size() == 1 ? 0 : std::min<std::size_t>(std::size_t(a_mip) + 1, a_texture->views.size() - 1);
		return { a_texture->views[view].Index(), a_texture };
	}

	bool Prepare(std::uint32_t a_format) { return ProgramFor(Backend(), a_format) != nullptr; }

	bool Commit(ImDrawData* a_drawData, const Target& a_target, const Resolver& a_resolve)
	{
		auto& state = Backend();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!state.installed || !host || !a_drawData)
			return false;
		if (!state.slots)
			state.slots = std::make_shared<Slots>();
		const RasterProgram* program = ProgramFor(state, a_target.format);
		if (!program)
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
		std::unordered_map<std::uint64_t, std::uint32_t> resolved;
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
				if (binding.owner)
					frame->owners.push_back(std::move(binding.owner));
				resolved.emplace(id, binding.index);
			}
		// MakeTexture's first uploads.
		for (auto& upload : state.pending)
			pending.push_back(std::move(upload));
		state.pending.clear();

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
			Upload staged{ upload.texture, !upload.texture->staged };
			upload.texture->staged = true;
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
				if (found == resolved.end() || found->second == kNoIndex)
					continue;
				Draw draw;
				// As D3D11 truncates its scissor rectangle's corners.
				draw.scissor.x = static_cast<std::int32_t>(minX);
				draw.scissor.y = static_cast<std::int32_t>(minY);
				draw.scissor.width = static_cast<std::uint32_t>(static_cast<std::int32_t>(maxX) - draw.scissor.x);
				draw.scissor.height = static_cast<std::uint32_t>(static_cast<std::int32_t>(maxY) - draw.scissor.y);
				if (!draw.scissor.width || !draw.scissor.height)
					continue;
				draw.texture = found->second;
				draw.indexCount = command.ElemCount;
				draw.indexOffset = static_cast<std::uint32_t>(indexBase + std::uint64_t(globalIndex + command.IdxOffset) * sizeof(ImDrawIdx));
				draw.vertexOffset = static_cast<std::uint32_t>(std::uint64_t(globalVertex + command.VtxOffset) * sizeof(ImDrawVert));
				frame->draws.push_back(draw);
			}
			globalVertex += static_cast<std::uint32_t>(list->VtxBuffer.Size);
			globalIndex += static_cast<std::uint32_t>(list->IdxBuffer.Size);
		}
		state.slots->Store(host->CurrentRun(), std::move(frame));
		return true;
	}

	std::shared_ptr<org::RenderPass> MakePass(std::shared_ptr<org::ExternalTextureResource> a_target)
	{
		auto& state = Backend();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!a_target || !host)
			return nullptr;
		if (!state.slots)
			state.slots = std::make_shared<Slots>();
		return std::make_shared<OverlayPass>(std::move(a_target), state.slots);
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
}

#endif
