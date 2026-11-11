#include "RenderGraph/Engine/Bink/BinkGraph.h"

namespace RenderGraph::Engine::Bink
{
	Layout MakeLayout(std::uint32_t a_videoWidth, std::uint32_t a_videoHeight, std::uint32_t a_tileSize, std::uint32_t a_columns, std::uint32_t a_rows,
		std::uint32_t a_edgeWidth, std::uint32_t a_edgeHeight, float a_scale, float a_x, float a_y, std::uint32_t a_screenWidth, std::uint32_t a_screenHeight)
	{
		Layout layout;
		layout.videoWidth = a_videoWidth;
		layout.videoHeight = a_videoHeight;
		if (!a_tileSize || !a_screenWidth || !a_screenHeight)
			return layout;
		// BSMovie::CreateTileTextures's arithmetic, in its order, so the quads land on the same pixels: sizes as fractions of the
		// screen, positions stepped by adding one tile at a time.
		const float screenWidth = static_cast<float>(a_screenWidth);
		const float screenHeight = static_cast<float>(a_screenHeight);
		const float tile = static_cast<float>(a_tileSize);
		const float wholeWidth = (tile / screenWidth) * a_scale;
		const float wholeHeight = (tile / screenHeight) * a_scale;
		const float edgeWidth = (static_cast<float>(a_edgeWidth) / screenWidth) * a_scale;
		const float edgeHeight = (static_cast<float>(a_edgeHeight) / screenHeight) * a_scale;
		const float stepX = a_scale / screenWidth;
		const float stepY = a_scale / screenHeight;

		// BSMovie::CreateTileQuad (SE 0x1410a2720): a rectangle of the screen (0..1, y down) to clip space.
		auto add = [&](std::uint32_t a_column, std::uint32_t a_row, float a_left, float a_top, float a_width, float a_height) {
			TileDesc tileDesc;
			tileDesc.x = a_column * a_tileSize;
			tileDesc.y = a_row * a_tileSize;
			tileDesc.width = a_column < a_columns ? a_tileSize : a_edgeWidth;
			tileDesc.height = a_row < a_rows ? a_tileSize : a_edgeHeight;
			tileDesc.rect[0] = a_left * 2.0f - 1.0f;
			tileDesc.rect[1] = 1.0f - a_top * 2.0f;
			tileDesc.rect[2] = a_width * 2.0f + tileDesc.rect[0];
			tileDesc.rect[3] = tileDesc.rect[1] - a_height * 2.0f;
			layout.tiles.push_back(tileDesc);
		};

		const float startX = stepX * a_x;
		float top = stepY * a_y;
		for (std::uint32_t row = 0; row < a_rows; ++row) {
			float left = startX;
			for (std::uint32_t column = 0; column < a_columns; ++column) {
				add(column, row, left, top, wholeWidth, wholeHeight);
				left = left + tile * stepX;
			}
			if (a_edgeWidth)
				add(a_columns, row, left, top, edgeWidth, wholeHeight);
			top = top + tile * stepY;
		}
		if (a_edgeHeight) {
			float left = startX;
			for (std::uint32_t column = 0; column < a_columns; ++column) {
				add(column, a_rows, left, top, wholeWidth, edgeHeight);
				left = left + tile * stepX;
			}
			if (a_edgeWidth)
				add(a_columns, a_rows, left, top, edgeWidth, edgeHeight);
		}
		return layout;
	}
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Globals.h"
#	include "RenderGraph/Engine/Bink/Bink.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <atomic>
#	include <cstring>
#	include <map>
#	include <mutex>
#	include <optional>

namespace RenderGraph::Engine::Bink::Graph
{
	namespace
	{
		constexpr const char* kShader = "Bink/BinkCopy.hlsl";
		constexpr const char* kClientId = "cs.bink";
		// D3D12's copy rules, which Vulkan's satisfy: rows of a texture copy 256-byte aligned, a copy's start 512.
		constexpr std::uint32_t kRowAlignment = 256;
		constexpr std::uint64_t kPlacementAlignment = 512;
		// Frames queued in a row whose run never came: the graph path stops after this many.
		constexpr std::uint32_t kMaxMisses = 3;

		// Must match the push block of BinkCopy.hlsl: the TexBase and SampBase handles, then the BinkQuad PUSH entry on a 16-byte
		// boundary.
		struct QuadBlock
		{
			ShaderBindings::Handle texture = 0;
			ShaderBindings::Handle sampler = 0;
			std::uint32_t padding[2]{};
			float rect[4]{};
		};
		static_assert(sizeof(QuadBlock) == 32);
		constexpr std::uint32_t kQuadWords = ShaderBindings::Words<QuadBlock>();

		std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) / a_alignment * a_alignment; }
	}

	/** @brief Host-visible staging buffers of one size, reused once the frame that used one retired (its last owner gave it back). */
	struct StagingPool
	{
		std::mutex mutex;
		std::vector<std::shared_ptr<org::LatchBlock>> spare;
		std::uint32_t bytes = 0;
		std::uint32_t made = 0;
	};

	namespace
	{
		// Frames in flight that can each hold a buffer, and the one the decoder fills. A 1080p movie takes 8.3 MB a buffer; with
		// none spare, a tick decodes on the engine's path.
		constexpr std::uint32_t kMaxStaging = 4;

		std::shared_ptr<org::LatchBlock> AcquireStaging(const std::shared_ptr<StagingPool>& a_pool)
		{
			std::shared_ptr<org::LatchBlock> block;
			{
				std::scoped_lock lock(a_pool->mutex);
				if (!a_pool->spare.empty()) {
					block = std::move(a_pool->spare.back());
					a_pool->spare.pop_back();
				} else if (a_pool->made >= kMaxStaging) {
					return nullptr;
				}
			}
			if (!block) {
				try {
					block = org::LatchBlock::Create("cs.bink.staging", a_pool->bytes, 1);
				} catch (const std::exception& e) {
					logger::error("[ORG] Bink: a staging buffer of {} bytes could not be made: {}", a_pool->bytes, e.what());
					return nullptr;
				}
				std::scoped_lock lock(a_pool->mutex);
				++a_pool->made;
			}
			// The pool is held weakly: a buffer coming back after the movie ended is released.
			std::weak_ptr<StagingPool> pool = a_pool;
			auto* raw = block.get();
			return std::shared_ptr<org::LatchBlock>(raw, [pool, block = std::move(block)](org::LatchBlock*) mutable {
				if (auto owner = pool.lock()) {
					std::scoped_lock lock(owner->mutex);
					owner->spare.push_back(std::move(block));
				}
			});
		}
	}

	/** @brief One tile's ORG texture and where its bytes sit in a staging buffer. */
	struct Tile
	{
		TileDesc desc;
		std::shared_ptr<rhi::ResourcePtr> resource;  // shared with its view, which owns it
		org::OwnedDescriptorBinding view;
		std::uint64_t offset = 0;
		std::uint32_t rowPitch = 0;
		bool edge = false;  // a power-of-two edge tile: the decoder fills only the video's part of it
	};

	class Movie
	{
	public:
		std::vector<Tile> tiles;
		std::shared_ptr<StagingPool> pool = std::make_shared<StagingPool>();
		// Main thread.
		std::shared_ptr<org::LatchBlock> decoding;  // this tick's staging buffer, from the first lock
		bool uploaded = false;                      // a frame's upload was committed: the textures left their undefined layout
		bool current = false;                       // the textures (once uploaded) hold the newest decoded frame
	};

	namespace
	{
		/** @brief What one execution of the pass records: immutable once the commit stored it. */
		struct Frame
		{
			std::shared_ptr<const Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;  // null: draw the textures as they are
			bool first = false;                        // the upload starts from the undefined layout
			std::shared_ptr<const RasterProgram> program;
			ShaderBindings::Handle sampler = 0;
			std::shared_ptr<const void> samplerOwner;
			std::uint32_t width = 0, height = 0;
		};

		/** @brief The frames the commit stored, one per host frame slot; Record takes its slot's. */
		struct Slots
		{
			explicit Slots(std::uint32_t a_count) :
				frames(std::make_unique<std::atomic<std::shared_ptr<const Frame>>[]>(a_count)), count(a_count) {}

			std::shared_ptr<const Frame> Take(std::uint32_t a_slot) const { return frames[a_slot % count].exchange(nullptr, std::memory_order_acq_rel); }
			void Store(std::uint32_t a_slot, std::shared_ptr<const Frame> a_frame) const { frames[a_slot % count].store(std::move(a_frame), std::memory_order_release); }

			std::unique_ptr<std::atomic<std::shared_ptr<const Frame>>[]> frames;
			std::uint32_t count;
		};

		/** @brief A frame queued for the Present anchor's run. */
		struct Pending
		{
			std::shared_ptr<Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;
		};

		struct BinkState
		{
			bool failed = false;
			bool registered = false;
			std::shared_ptr<org::ExternalTextureResource> target;
			ID3D11Texture2D* targetTexture = nullptr;  // the framebuffer texture the client's pass declares
			std::uint32_t width = 0, height = 0;
			std::uint32_t format = 0;  // rhi::Format of the framebuffer's RTV
			std::map<std::uint32_t, std::shared_ptr<const RasterProgram>> programs;
			std::map<std::uint32_t, bool> failedPrograms;
			org::OwnedDescriptorBinding sampler;
			std::shared_ptr<Slots> slots;
			std::optional<Pending> pending;
			std::uint64_t queued = 0;     // frames queued
			std::uint64_t committed = 0;  // the newest queued frame a commit stored
			std::uint64_t executed = 0;   // the newest committed frame whose run had the pass in its graph
			std::uint64_t checked = 0;    // the newest queued frame Begin has checked
			std::uint32_t misses = 0;     // checked frames in a row that did not run
		};

		BinkState& Get()
		{
			static BinkState s_state;
			return s_state;
		}

		void Fail(BinkState& a_state, const char* a_why)
		{
			if (a_state.failed)
				return;
			a_state.failed = true;
			a_state.pending.reset();
			if (a_state.registered) {
				FrameAnchors::Unregister(kClientId);
				a_state.registered = false;
			}
			logger::error("[ORG] Bink: {}; the engine draws Bink from now on", a_why);
		}

		const RasterProgram* ProgramFor(BinkState& a_state, std::uint32_t a_format)
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
			desc.constantWords = kQuadWords;
			// BSMovie::DrawTiles sets blending off (alpha blend mode 0) and depth off (depth mode 0); the engine's rasterizer state at
			// that point culls nothing, and the quads wind counter-clockwise.
			desc.targets = { RasterProgram::Target{ .format = static_cast<rhi::Format>(a_format) } };
			auto program = RasterProgram::Load(host->Device(), desc);
			if (!program) {
				a_state.failedPrograms[a_format] = true;
				logger::error("[ORG] Bink: the pipeline for target format {} could not be made", a_format);
				return nullptr;
			}
			auto* raw = program.get();
			a_state.programs[a_format] = std::move(program);
			return raw;
		}

		/** @brief The frame's tile copies and their barriers, before the draws (the graph does not track these textures). */
		void RecordUpload(const Frame& a_frame, rhi::CommandList& a_commands)
		{
			if (!a_frame.staging)
				return;
			const auto& tiles = a_frame.movie->tiles;
			std::vector<rhi::TextureBarrier> barriers;
			barriers.reserve(tiles.size());
			for (const auto& tile : tiles) {
				rhi::TextureBarrier barrier{};
				barrier.texture = (*tile.resource)->GetHandle();
				barrier.range.mipCount = 1;
				barrier.beforeSync = a_frame.first ? rhi::ResourceSyncState::None : rhi::ResourceSyncState::PixelShading;
				barrier.afterSync = rhi::ResourceSyncState::Copy;
				barrier.beforeAccess = a_frame.first ? rhi::ResourceAccessType::None : rhi::ResourceAccessType::ShaderResource;
				barrier.afterAccess = rhi::ResourceAccessType::CopyDest;
				barrier.beforeLayout = a_frame.first ? rhi::ResourceLayout::Undefined : rhi::ResourceLayout::ShaderResource;
				barrier.afterLayout = rhi::ResourceLayout::CopyDest;
				// Every texel is written: the tile's whole texture is in the staging buffer.
				barrier.discard = true;
				barriers.push_back(barrier);
			}
			a_commands.Barriers({ .textures = { barriers.data(), static_cast<std::uint32_t>(barriers.size()) } });
			const auto staging = a_frame.staging->Resource()->GetAPIResource().GetHandle();
			for (const auto& tile : tiles) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = (*tile.resource)->GetHandle();
				copy.buffer = staging;
				copy.mip = 0;
				copy.arraySlice = 0;
				copy.x = 0;
				copy.y = 0;
				copy.footprint = { tile.offset, tile.rowPitch, tile.desc.width, tile.desc.height, 1 };
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

		/** @brief The tiles' upload and draw at the Present anchor, onto the framebuffer. Re-recorded every run (no Revision). */
		class TilePass final : public org::TypedRenderGraphPass<TilePass, PassFrame, PassBindings>
		{
		public:
			TilePass(std::shared_ptr<org::ExternalTextureResource> a_target, std::shared_ptr<const Slots> a_slots) :
				target(std::move(a_target)), slots(std::move(a_slots)) {}

			PassBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(target).View() }; }

			PassFrame Prepare(const PassBindings&, const org::PassPrepareContext&) const { return { slots }; }

			static void Record(const PassBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots)
					return;
				const auto frame = a_pass.slots->Take(a_recording.FrameSlot());
				if (!frame || !frame->program || !frame->movie)
					return;
				// The staging buffer, the tile textures and their views stay until this recording's GPU work retires.
				a_recording.Retain(frame);
				auto& commands = a_recording.Commands();
				RecordUpload(*frame, commands);

				// The engine's tick cleared the framebuffer (its SetRenderTarget with the clear mode) before this run: the tiles go
				// over it.
				rhi::ColorAttachment colour{};
				colour.rtv = a_recording.View(a_bindings.target);
				colour.resource = a_recording.Resolve(a_bindings.target).GetHandle();
				colour.loadOp = rhi::LoadOp::Load;
				colour.storeOp = rhi::StoreOp::Store;
				rhi::PassBeginInfo begin{};
				begin.colors = { &colour, 1 };
				begin.width = frame->width;
				begin.height = frame->height;
				begin.debugName = "CS Bink tiles";
				commands.BeginPass(begin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame->program->layout->GetHandle());
				commands.BindPipeline(frame->program->Pipeline());
				QuadBlock block{};
				block.sampler = frame->sampler;
				for (const auto& tile : frame->movie->tiles) {
					block.texture = tile.view.Index();
					std::memcpy(block.rect, tile.desc.rect, sizeof(block.rect));
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kQuadWords, reinterpret_cast<const std::uint32_t*>(&block));
					commands.Draw(6, 1, 0, 0);
				}
				commands.EndPass();
			}

		private:
			std::shared_ptr<org::ExternalTextureResource> target;
			std::shared_ptr<const Slots> slots;
		};

		FrameAnchors::ClientDesc MakeClient()
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			// First in the anchor: the movie frame is the image the other Present clients draw over.
			client.passes.push_back({ .name = "cs.bink.tiles",
				.anchor = FrameAnchors::Anchor::Present,
				.order = -1000,
				.kind = FrameAnchors::PassKind::Render,
				.make = [] {
					auto& state = Get();
					return state.target && state.slots ? std::make_shared<TilePass>(state.target, state.slots) : nullptr;
				} });
			client.commit = [](const FrameAnchors::Point&) {
				auto& state = Get();
				auto* host = RenderGraphRuntime::Get().Host();
				if (!state.pending || !host || !state.slots)
					return;
				auto pending = std::move(*state.pending);
				state.pending.reset();
				const auto found = state.programs.find(state.format);
				if (found == state.programs.end())
					return;
				auto frame = std::make_shared<Frame>();
				frame->movie = pending.movie;
				frame->staging = std::move(pending.staging);
				frame->first = frame->staging && !pending.movie->uploaded;
				frame->program = found->second;
				frame->sampler = state.sampler.Index();
				frame->samplerOwner = state.sampler.Owner();
				frame->width = state.width;
				frame->height = state.height;
				if (frame->staging)
					pending.movie->uploaded = true;
				state.slots->Store(host->CurrentFrameSlot(), std::move(frame));
				state.committed = state.queued;
			};
			client.active = [] { return Get().pending.has_value(); };
			client.executed = [](const FrameAnchors::Point&) {
				auto& state = Get();
				state.executed = state.committed;
			};
			client.onFailed = [] {
				auto& state = Get();
				state.registered = false;
				Fail(state, "the render graph failed");
			};
			return client;
		}

		/** @brief The framebuffer as the pass's target: imported, sized, and its program made. False: the engine draws this tick. */
		bool UpdateTarget(BinkState& a_state)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return false;
			const auto& framebuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
			auto* texture = reinterpret_cast<ID3D11Texture2D*>(framebuffer.texture);
			auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(framebuffer.RTV);
			// The back buffer: the engine keeps only its RTV (FrameAnchors::EngineTarget takes the texture from it too).
			winrt::com_ptr<ID3D11Texture2D> fromView;
			if (!texture && rtv) {
				winrt::com_ptr<ID3D11Resource> resource;
				rtv->GetResource(resource.put());
				if (resource)
					fromView = resource.try_as<ID3D11Texture2D>();
				texture = fromView.get();
			}
			if (!texture || !rtv)
				return false;
			if (texture == a_state.targetTexture && a_state.target)
				return true;
			D3D11_TEXTURE2D_DESC textureDesc{};
			texture->GetDesc(&textureDesc);
			D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
			rtv->GetDesc(&viewDesc);
			const auto format = static_cast<std::uint32_t>(rhi::helpers::ToRHI(viewDesc.Format));
			auto target = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
			if (!target) {
				Fail(a_state, "the framebuffer could not be imported");
				return false;
			}
			if (!ProgramFor(a_state, format)) {
				Fail(a_state, "the tile pipeline could not be made");
				return false;
			}
			const bool changed = a_state.targetTexture != nullptr;
			a_state.target = std::move(target);
			a_state.targetTexture = texture;
			a_state.width = textureDesc.Width;
			a_state.height = textureDesc.Height;
			a_state.format = format;
			logger::info("[ORG] Bink: drawing onto the framebuffer ({}x{}, DXGI format {})", textureDesc.Width, textureDesc.Height, static_cast<int>(viewDesc.Format));
			if (changed && a_state.registered) {
				// The pass declares the new target from the next build; this tick stays the engine's.
				FrameAnchors::Refresh(kClientId);
				return false;
			}
			return true;
		}
	}

	bool Begin()
	{
		// Why a tick stays the engine's, once per reason.
		const auto refuse = [](std::string_view a_reason) {
			static std::mutex mutex;
			static std::vector<std::string> logged;
			std::scoped_lock lock(mutex);
			if (std::ranges::find(logged, a_reason) == logged.end()) {
				logged.emplace_back(a_reason);
				logger::info("[ORG] Bink: the engine draws this frame: {}", a_reason);
			}
			return false;
		};
		auto& state = Get();
		if (state.failed)
			return refuse("the graph path failed earlier");
		auto& runtime = RenderGraphRuntime::Get();
		auto* host = runtime.Host();
		if (!runtime.IsActive() || !host)
			return refuse("the render graph is not active");
		if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::Present))
			return refuse("the Present anchor has no epoch");
		// Until the Present anchor's graph holds the pass (built again on the host's thread after the registration), the engine
		// draws: a queued frame would not be drawn.
		if (state.registered && !FrameAnchors::InBuiltGraph(kClientId, FrameAnchors::Anchor::Present)) {
			// Drawcall Limit Fix makes builds explicit, at its build point in each world frame, and a blocking movie runs no world
			// frame. A movie tick runs between frames (after a Present, before the next build point), where a build is safe.
			if (host->RebuildRequested())
				(void)host->BuildIfRequested();
			if (!FrameAnchors::InBuiltGraph(kClientId, FrameAnchors::Anchor::Present)) {
				state.queued = state.committed = state.executed = state.checked = 0;
				state.misses = 0;
				return refuse("waiting for the Present anchor's graph to hold the pass");
			}
		}
		// Once per queued frame: whether the run after it had the pass in its graph.
		if (state.queued != state.checked) {
			state.checked = state.queued;
			if (state.executed < state.queued) {
				if (++state.misses >= kMaxMisses) {
					Fail(state, "queued frames did not reach the Present anchor's runs");
					return false;
				}
			} else {
				state.misses = 0;
			}
		}
		state.pending.reset();
		if (!UpdateTarget(state))
			return refuse("the framebuffer cannot be imported");
		if (!state.sampler) {
			// The engine's PS sampler for this draw: address mode 0, filter mode 2 (CreateSamplerStates, SE 75831): trilinear,
			// clamped, no bias, every LOD.
			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxAnisotropy = 1;
			sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
			sampler.MinLOD = 0.0f;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			state.sampler = ShaderBindings::CreateSamplerFromD3D11(sampler);
			if (!state.sampler) {
				Fail(state, "the tile sampler could not be made");
				return false;
			}
		}
		if (!state.slots || state.slots->count != host->FrameSlots())
			state.slots = std::make_shared<Slots>(host->FrameSlots());
		if (!state.registered) {
			if (!FrameAnchors::Register(MakeClient())) {
				Fail(state, "the Present anchor's client could not be registered");
				return false;
			}
			state.registered = true;
			state.queued = state.committed = state.executed = state.checked = 0;
			state.misses = 0;
			logger::info("[ORG] Bink: registered at the Present anchor; the graph draws from the next frame");
			return false;
		}
		return true;
	}

	std::shared_ptr<Movie> CreateMovie(const Layout& a_layout)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || a_layout.tiles.empty())
			return nullptr;
		auto movie = std::make_shared<Movie>();
		std::uint64_t bytes = 0;
		for (std::size_t i = 0; i < a_layout.tiles.size(); ++i) {
			const auto& desc = a_layout.tiles[i];
			if (!desc.width || !desc.height)
				return nullptr;
			Tile tile;
			tile.desc = desc;
			tile.rowPitch = static_cast<std::uint32_t>(AlignUp(std::uint64_t(desc.width) * 4, kRowAlignment));
			tile.offset = bytes;
			tile.edge = desc.x + desc.width > a_layout.videoWidth || desc.y + desc.height > a_layout.videoHeight;
			bytes = AlignUp(bytes + std::uint64_t(tile.rowPitch) * desc.height, kPlacementAlignment);

			rhi::ResourceDesc resourceDesc{};
			resourceDesc.type = rhi::ResourceType::Texture2D;
			const auto name = fmt::format("cs.bink.tile.{}", i);
			resourceDesc.debugName = name.c_str();
			resourceDesc.texture.format = rhi::Format::R8G8B8A8_UNorm;
			resourceDesc.texture.width = desc.width;
			resourceDesc.texture.height = desc.height;
			resourceDesc.texture.mipLevels = 1;
			tile.resource = std::make_shared<rhi::ResourcePtr>();
			if (host->Device().CreateCommittedResource(resourceDesc, *tile.resource) != rhi::Result::Ok || !*tile.resource) {
				logger::error("[ORG] Bink: tile {} ({}x{}) could not be created", i, desc.width, desc.height);
				return nullptr;
			}
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::Texture2D;
			srv.formatOverride = rhi::Format::R8G8B8A8_UNorm;
			srv.tex2D.mostDetailedMip = 0;
			srv.tex2D.mipLevels = 1;
			tile.view = ShaderBindings::CreateShaderResourceView(tile.resource->Get(), tile.resource, srv);
			if (!tile.view)
				return nullptr;
			movie->tiles.push_back(std::move(tile));
		}
		if (bytes > 0xFFFFFFFFull)
			return nullptr;
		movie->pool->bytes = static_cast<std::uint32_t>(bytes);
		logger::info("[ORG] Bink: a {}x{} movie in {} tiles, {} bytes of staging a frame", a_layout.videoWidth, a_layout.videoHeight, movie->tiles.size(), bytes);
		return movie;
	}

	std::byte* Lock(Movie& a_movie, std::uint32_t a_tile, std::uint32_t& a_rowPitch)
	{
		if (a_tile >= a_movie.tiles.size())
			return nullptr;
		if (!a_movie.decoding)
			a_movie.decoding = AcquireStaging(a_movie.pool);
		if (!a_movie.decoding)
			return nullptr;
		const auto& tile = a_movie.tiles[a_tile];
		std::byte* const data = a_movie.decoding->Slot(0).data() + tile.offset;
		// The decoder writes the video's part of an edge tile; the rest is drawn as the engine's zero-filled texels.
		if (tile.edge)
			std::memset(data, 0, std::size_t(tile.rowPitch) * tile.desc.height);
		a_rowPitch = tile.rowPitch;
		return data;
	}

	bool Queue(const std::shared_ptr<Movie>& a_movie)
	{
		auto& state = Get();
		if (state.failed || !state.registered || !a_movie)
			return false;
		if (a_movie->decoding) {
			state.pending = Pending{ a_movie, std::move(a_movie->decoding) };
			a_movie->decoding.reset();
			a_movie->current = true;
		} else if (a_movie->uploaded && a_movie->current) {
			state.pending = Pending{ a_movie, nullptr };
		} else {
			return false;
		}
		++state.queued;
		return true;
	}

	void EndTick(Movie& a_movie)
	{
		a_movie.decoding.reset();
	}

	void MarkStale(Movie& a_movie)
	{
		a_movie.current = false;
	}

	void MovieEnded()
	{
		auto& state = Get();
		state.pending.reset();
		if (state.registered) {
			FrameAnchors::Unregister(kClientId);
			state.registered = false;
		}
		// The next movie starts from a clean count.
		state.queued = state.committed = state.executed = state.checked = 0;
		state.misses = 0;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::Bink::Graph
{
	class Movie
	{};
	bool Begin() { return false; }
	std::shared_ptr<Movie> CreateMovie(const Layout&) { return nullptr; }
	std::byte* Lock(Movie&, std::uint32_t, std::uint32_t&) { return nullptr; }
	bool Queue(const std::shared_ptr<Movie>&) { return false; }
	void EndTick(Movie&) {}
	void MarkStale(Movie&) {}
	void MovieEnded() {}
}

#endif
