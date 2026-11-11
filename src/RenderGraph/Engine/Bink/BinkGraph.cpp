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
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ProgramCache.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "Utils/Logging.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <cstring>
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
		// The movie texture every tile is copied into, at its place in the video: a 1920x1080 movie's tiles reach 1920x1088 (the
		// bottom row's power-of-two edge tiles). A larger movie keeps the engine's path.
		constexpr std::uint32_t kAtlasWidth = 2048;
		constexpr std::uint32_t kAtlasHeight = 1152;
		// Staging buffers: the frame the decoder fills and the frames in flight. A 1080p frame takes 8.3 MB; with none to take, a
		// tick decodes on the engine's path.
		constexpr std::uint32_t kMaxStaging = 4;

		// Must match the push block of BinkCopy.hlsl: the TexBase and SampBase handles, then the BinkQuad PUSH entry on a 16-byte
		// boundary.
		struct QuadBlock
		{
			ShaderBindings::Handle texture = 0;
			ShaderBindings::Handle sampler = 0;
			std::uint32_t padding[2]{};
			float rect[4]{};
			float uv[4]{};
		};
		static_assert(sizeof(QuadBlock) == 48);
		constexpr std::uint32_t kQuadWords = ShaderBindings::Words<QuadBlock>();

		std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) / a_alignment * a_alignment; }
	}

	/** @brief One tile: its rectangle of the video (and of the movie texture) and where its bytes sit in a staging buffer. */
	struct Tile
	{
		TileDesc desc;
		std::uint64_t offset = 0;
		std::uint32_t rowPitch = 0;
		bool edge = false;  // a power-of-two edge tile: the decoder fills only the video's part of it
	};

	class Movie
	{
	public:
		std::vector<Tile> tiles;
		std::uint64_t stagingBytes = 0;
		// Render thread.
		std::shared_ptr<org::LatchBlock> decoding;  // this tick's staging buffer, from the first lock
		bool current = false;                       // the movie texture (once uploaded) holds the newest decoded frame
	};

	namespace
	{
		/** @brief What one run of the passes draws: immutable once the commit stored it. */
		struct Frame
		{
			std::shared_ptr<const Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;  // null: draw the movie texture as it is
			std::shared_ptr<const RasterProgram> program;
			ShaderBindings::Handle sampler = 0;
			std::shared_ptr<const void> samplerOwner;
			std::uint32_t width = 0, height = 0;
		};

		/** @brief A frame queued for the Present anchor's run. */
		struct Pending
		{
			std::shared_ptr<Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;
		};

		struct BinkState
		{
			bool setUp = false;
			bool failed = false;
			bool joined = false;  // the Present anchor's built graph holds this registration (FrameAnchors::RequestJoin)
			std::unique_ptr<GraphResources::Texture2D> atlas;
			std::shared_ptr<FrameStaging> staging;
			// The frame each pass records, per host frame slot: the upload's and the draw's.
			std::shared_ptr<const RunSlots<Frame>> uploads, draws;
			std::shared_ptr<org::ExternalTextureResource> target;  // the framebuffer the pass draws (FrameAnchors::EngineTarget)
			std::uint32_t width = 0, height = 0;
			rhi::Format format = rhi::Format::Unknown;  // of the framebuffer's RTV
			org::OwnedDescriptorBinding sampler;
			std::optional<Pending> pending;
			const Movie* uploaded = nullptr;  // the movie whose frame the movie texture holds
			std::uint64_t queued = 0;         // frames queued
			std::uint64_t committed = 0;      // the newest queued frame a commit stored
			std::uint64_t executed = 0;       // the newest committed frame whose run had the passes in its graph
			bool missed = false;              // a queued frame did not draw: the engine draws the rest of this movie
		};

		BinkState& Get()
		{
			static BinkState s_state;
			return s_state;
		}

		/** @brief The tile program per framebuffer format, built off the render thread. */
		ProgramCache<RasterProgram, rhi::Format>& Programs()
		{
			static ProgramCache<RasterProgram, rhi::Format> programs{ "Bink" };
			return programs;
		}

		std::shared_ptr<const RasterProgram> ProgramFor(rhi::Format a_format)
		{
			return Programs().Get(a_format, [&] {
				RasterProgram::Desc desc{};
				desc.vertexSource = kShader;
				desc.vertexEntry = L"VSMain";
				desc.pixelSource = kShader;
				desc.pixelEntry = L"PSMain";
				desc.constantWords = kQuadWords;
				// BSMovie::DrawTiles sets blending off (alpha blend mode 0) and depth off (depth mode 0); the engine's rasterizer state at
				// that point culls nothing, and the quads wind counter-clockwise.
				desc.targets = { RasterProgram::Target{ .format = a_format } };
				return desc;
			});
		}

		void Fail(BinkState& a_state, const char* a_why)
		{
			if (a_state.failed)
				return;
			a_state.failed = true;
			a_state.pending.reset();
			FrameAnchors::Unregister(kClientId);
			logger::error("[ORG] Bink: {}; the engine draws Bink from now on", a_why);
		}

		Util::ReasonLog& EngineDraws()
		{
			static Util::ReasonLog s_log("[ORG] Bink: the engine draws this frame");
			return s_log;
		}

		struct UploadBindings
		{
			org::ResourceBindingToken atlas;
		};

		struct DrawBindings
		{
			org::DeclaredViewToken target;
			org::DeclaredViewToken atlas;
		};

		struct PassFrame
		{
			std::shared_ptr<const RunSlots<Frame>> slots;
		};

		/**
		 * @brief A decoded frame's tiles copied from its staging buffer into the movie texture, each at its place in the video. Records
		 * nothing on a run with no frame (the slot is empty). Recorded every run (no Revision): the slot decides.
		 */
		class UploadPass final : public org::TypedRenderGraphPass<UploadPass, PassFrame, UploadBindings>
		{
		public:
			UploadPass(std::shared_ptr<org::Resource> a_atlas, std::shared_ptr<const RunSlots<Frame>> a_slots) :
				atlas(std::move(a_atlas)), slots(std::move(a_slots)) {}

			UploadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.CopyDestination(atlas) };
			}

			PassFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { slots }; }

			static void Record(const UploadBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				const auto frame = a_pass.slots->Take(a_recording.FrameSlot());
				if (!frame || !frame->staging || !frame->movie)
					return;
				// The staging buffer stays until this recording's GPU work retires.
				a_recording.Retain(frame);
				const auto texture = a_recording.Resolve(a_bindings.atlas).GetHandle();
				const auto staging = frame->staging->Resource()->GetAPIResource().GetHandle();
				for (const auto& tile : frame->movie->tiles) {
					rhi::BufferTextureCopyFootprint copy{};
					copy.texture = texture;
					copy.buffer = staging;
					copy.x = tile.desc.x;
					copy.y = tile.desc.y;
					copy.footprint = { tile.offset, tile.rowPitch, tile.desc.width, tile.desc.height, 1 };
					a_recording.Commands().CopyBufferToTexture(copy);
				}
			}

		private:
			std::shared_ptr<org::Resource> atlas;
			std::shared_ptr<const RunSlots<Frame>> slots;
		};

		/** @brief The tiles drawn from the movie texture onto the framebuffer. Recorded every run (no Revision): the slot decides. */
		class DrawPass final : public org::TypedRenderGraphPass<DrawPass, PassFrame, DrawBindings>
		{
		public:
			DrawPass(std::shared_ptr<org::ExternalTextureResource> a_target, std::shared_ptr<org::Resource> a_atlas, std::shared_ptr<const RunSlots<Frame>> a_slots) :
				target(std::move(a_target)), atlas(std::move(a_atlas)), slots(std::move(a_slots)) {}

			DrawBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(target).View(), a_builder.ShaderResource(atlas).View() }; }

			PassFrame Prepare(const DrawBindings&, const org::PassPrepareContext&) const { return { slots }; }

			static void Record(const DrawBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				const auto frame = a_pass.slots->Take(a_recording.FrameSlot());
				if (!frame || !frame->program || !frame->movie)
					return;
				a_recording.Retain(frame);
				auto& commands = a_recording.Commands();
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
				block.texture = a_recording.Index(a_bindings.atlas);
				block.sampler = frame->sampler;
				for (const auto& tile : frame->movie->tiles) {
					std::memcpy(block.rect, tile.desc.rect, sizeof(block.rect));
					// The tile's own texels of the movie texture.
					block.uv[0] = static_cast<float>(tile.desc.x) / kAtlasWidth;
					block.uv[1] = static_cast<float>(tile.desc.y) / kAtlasHeight;
					block.uv[2] = static_cast<float>(tile.desc.x + tile.desc.width) / kAtlasWidth;
					block.uv[3] = static_cast<float>(tile.desc.y + tile.desc.height) / kAtlasHeight;
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kQuadWords, reinterpret_cast<const std::uint32_t*>(&block));
					commands.Draw(6, 1, 0, 0);
				}
				commands.EndPass();
			}

		private:
			std::shared_ptr<org::ExternalTextureResource> target;
			std::shared_ptr<org::Resource> atlas;
			std::shared_ptr<const RunSlots<Frame>> slots;
		};

		FrameAnchors::ClientDesc MakeClient()
		{
			auto& state = Get();
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			// First in the anchor: the movie frame is the image the other Present clients draw over.
			client.passes.push_back({ .name = "cs.bink.upload",
				.anchor = FrameAnchors::Anchor::Present,
				.order = -1001,
				.kind = FrameAnchors::PassKind::Copy,
				.make = [atlas = state.atlas->Graph(), slots = state.uploads] { return std::make_shared<UploadPass>(atlas, slots); } });
			client.passes.push_back({ .name = "cs.bink.tiles",
				.anchor = FrameAnchors::Anchor::Present,
				.order = -1000,
				.kind = FrameAnchors::PassKind::Render,
				.make = [] {
					auto& current = Get();
					return std::make_shared<DrawPass>(current.target, current.atlas->Graph(), current.draws);
				} });
			client.registerResources = [](org::RenderGraph& a_graph) { Get().atlas->Register(a_graph); };
			client.commit = [](const FrameAnchors::Point&) {
				auto& current = Get();
				auto* host = RenderGraphRuntime::Get().Host();
				if (!current.pending || !host)
					return;
				auto pending = std::move(*current.pending);
				current.pending.reset();
				auto frame = std::make_shared<Frame>();
				frame->movie = pending.movie;
				frame->staging = std::move(pending.staging);
				frame->program = ProgramFor(current.format);
				frame->sampler = current.sampler.Index();
				frame->samplerOwner = current.sampler.Owner();
				frame->width = current.width;
				frame->height = current.height;
				if (frame->staging)
					current.uploaded = pending.movie.get();
				const std::uint32_t slot = host->CurrentFrameSlot();
				current.uploads->Store(slot, frame);
				current.draws->Store(slot, std::move(frame));
				current.committed = current.queued;
			};
			client.active = [] { return Get().pending.has_value(); };
			client.executed = [](const FrameAnchors::Point&) {
				auto& current = Get();
				current.executed = current.committed;
			};
			client.onFailed = [] { Fail(Get(), "the render graph failed"); };
			return client;
		}

		/**
		 * @brief The framebuffer as the draw's target. Made again only when the engine's back buffer changed (a resize): the client
		 * is refreshed then and joins again; the engine draws until it has.
		 */
		bool UpdateTarget(BinkState& a_state)
		{
			auto target = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
			if (!target)
				return false;
			if (target == a_state.target)
				return true;
			auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].RTV);
			if (!rtv)
				return false;
			D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
			rtv->GetDesc(&viewDesc);
			const bool changed = a_state.target != nullptr;
			a_state.target = std::move(target);
			a_state.width = a_state.target->GetWidth();
			a_state.height = a_state.target->GetHeight();
			a_state.format = rhi::helpers::ToRHI(viewDesc.Format);
			(void)ProgramFor(a_state.format);
			logger::info("[ORG] Bink: drawing onto the framebuffer ({}x{}, DXGI format {})", a_state.width, a_state.height, static_cast<int>(viewDesc.Format));
			if (changed) {
				// The draw declares the new target from the next build.
				a_state.joined = false;
				FrameAnchors::Refresh(kClientId);
			}
			return true;
		}
	}

	void Setup()
	{
		auto& state = Get();
		if (!Requested() || state.setUp || state.failed)
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !RenderGraphRuntime::Get().IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Present) || !globals::game::renderer) {
			logger::warn("[ORG] Bink: CS_ORG_BINK=1, but the render graph or its Present anchor is not active; the engine draws Bink");
			return;
		}
		D3D11_TEXTURE2D_DESC atlas{};
		atlas.Width = kAtlasWidth;
		atlas.Height = kAtlasHeight;
		atlas.MipLevels = 1;
		atlas.ArraySize = 1;
		atlas.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		atlas.SampleDesc.Count = 1;
		atlas.Usage = D3D11_USAGE_DEFAULT;
		atlas.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		// Persistent: a tick that decoded nothing draws the movie texture as the last upload left it.
		state.atlas = std::make_unique<GraphResources::Texture2D>(atlas, "Bink::Movie", GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		state.staging = FrameStaging::Create({ .name = "cs.bink.staging",
			.minimumBytes = 1u << 20,
			.runsPerFrame = FrameAnchors::RunsPerFrame(FrameAnchors::Anchor::Present),
			.maxBlocks = kMaxStaging });
		state.uploads = std::make_shared<const RunSlots<Frame>>(host->FrameSlots());
		state.draws = std::make_shared<const RunSlots<Frame>>(host->FrameSlots());
		// The engine's PS sampler for this draw: address mode 0, filter mode 2 (CreateSamplerStates, SE 75831): trilinear, clamped, no
		// bias, every LOD.
		D3D11_SAMPLER_DESC sampler{};
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.MaxAnisotropy = 1;
		sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		state.sampler = ShaderBindings::CreateSamplerFromD3D11(sampler);
		if (!*state.atlas || !state.staging || !state.sampler || !UpdateTarget(state)) {
			Fail(state, "its movie texture, staging, sampler or framebuffer could not be made");
			return;
		}
		// One client for the session: every movie draws through it, so a movie never rebuilds the graph.
		if (!FrameAnchors::Register(MakeClient())) {
			Fail(state, "the Present anchor's client could not be registered");
			return;
		}
		state.setUp = true;
		logger::info("[ORG] Bink: on (CS_ORG_BINK); registered at the Present anchor for the session");
	}

	bool Begin()
	{
		// Why a tick stays the engine's, logged once per reason.
		const auto refuse = [](std::string_view a_reason) {
			EngineDraws().Report(a_reason);
			return false;
		};
		auto& state = Get();
		if (state.failed)
			return refuse("the graph path failed earlier");
		if (!state.setUp)
			return refuse("the graph path is not set up");
		if (!RenderGraphRuntime::Get().IsActive())
			return refuse("the render graph is not active");
		// The frame queued last tick did not draw (its run did not submit with the passes): its tiles were withheld, so the engine
		// draws the rest of this movie rather than miss another frame.
		if (state.executed < state.queued && !state.missed) {
			state.missed = true;
			logger::warn("[ORG] Bink: a movie frame queued for the Present anchor did not draw; the engine draws the rest of this movie");
		}
		if (state.missed)
			return refuse("a frame of this movie did not draw on the graph");
		state.pending.reset();
		if (!UpdateTarget(state))
			return refuse("the framebuffer cannot be imported");
		if (!ProgramFor(state.format))
			return refuse("the tile program is not built yet");
		// Joined once the Present anchor's built graph holds the client; the build happens at the next build point (the frame's end in
		// a blocking movie's loop). The engine draws meanwhile.
		if (!state.joined && !(state.joined = FrameAnchors::RequestJoin(kClientId, FrameAnchors::Anchor::Present)))
			return refuse("waiting for the Present anchor's graph to hold the client");
		if (!FrameAnchors::CanRun(kClientId, FrameAnchors::Anchor::Present))
			return refuse("the Present anchor cannot run this frame");
		return true;
	}

	std::shared_ptr<Movie> CreateMovie(const Layout& a_layout)
	{
		if (!Get().setUp || a_layout.tiles.empty())
			return nullptr;
		auto movie = std::make_shared<Movie>();
		std::uint64_t bytes = 0;
		for (const auto& desc : a_layout.tiles) {
			if (!desc.width || !desc.height)
				return nullptr;
			if (desc.x + desc.width > kAtlasWidth || desc.y + desc.height > kAtlasHeight) {
				logger::info("[ORG] Bink: a {}x{} movie is larger than the {}x{} movie texture; the engine draws it", a_layout.videoWidth, a_layout.videoHeight,
					kAtlasWidth, kAtlasHeight);
				return nullptr;
			}
			Tile tile;
			tile.desc = desc;
			tile.rowPitch = static_cast<std::uint32_t>(AlignUp(std::uint64_t(desc.width) * 4, kRowAlignment));
			tile.offset = bytes;
			tile.edge = desc.x + desc.width > a_layout.videoWidth || desc.y + desc.height > a_layout.videoHeight;
			bytes = AlignUp(bytes + std::uint64_t(tile.rowPitch) * desc.height, kPlacementAlignment);
			movie->tiles.push_back(tile);
		}
		if (bytes > 0xFFFFFFFFull)
			return nullptr;
		movie->stagingBytes = bytes;
		logger::info("[ORG] Bink: a {}x{} movie in {} tiles, {} bytes of staging a frame", a_layout.videoWidth, a_layout.videoHeight, movie->tiles.size(), bytes);
		return movie;
	}

	std::byte* Lock(Movie& a_movie, std::uint32_t a_tile, std::uint32_t& a_rowPitch)
	{
		if (a_tile >= a_movie.tiles.size())
			return nullptr;
		if (!a_movie.decoding)
			a_movie.decoding = Get().staging->Acquire(a_movie.stagingBytes);
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
		if (state.failed || !state.setUp || !a_movie)
			return false;
		if (a_movie->decoding) {
			state.pending = Pending{ a_movie, std::move(a_movie->decoding) };
			a_movie->decoding.reset();
			a_movie->current = true;
		} else if (a_movie->current && state.uploaded == a_movie.get()) {
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
		state.uploaded = nullptr;
		// The client stays registered for the next movie; the next movie starts from a clean count.
		state.queued = state.committed = state.executed = 0;
		state.missed = false;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::Bink::Graph
{
	class Movie
	{};
	void Setup() {}
	bool Begin() { return false; }
	std::shared_ptr<Movie> CreateMovie(const Layout&) { return nullptr; }
	std::byte* Lock(Movie&, std::uint32_t, std::uint32_t&) { return nullptr; }
	bool Queue(const std::shared_ptr<Movie>&) { return false; }
	void EndTick(Movie&) {}
	void MarkStale(Movie&) {}
	void MovieEnded() {}
}

#endif
