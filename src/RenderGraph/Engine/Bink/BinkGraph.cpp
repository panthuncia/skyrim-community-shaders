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

#	include "Features/HDRDisplay.h"
#	include "Globals.h"
#	include "RenderGraph/Engine/Bink/Bink.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
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
		constexpr const char* kHdrUiName = "hdr.ui";  // ORGHDRDisplay's import of HDR's UI texture: one graph resource for the image
		// D3D12's copy rules, which Vulkan's satisfy: rows of a texture copy 256-byte aligned, a copy's start 512.
		constexpr std::uint32_t kRowAlignment = 256;
		constexpr std::uint64_t kPlacementAlignment = 512;
		// The movie texture the tiles are copied into, one chunk of the video at a time: a 1920x1080 movie's tiles (up to 1920x1088)
		// are one chunk. A larger movie is drawn in chunks, each a band of tile rows and columns that fits.
		constexpr std::uint32_t kAtlasWidth = 2048;
		constexpr std::uint32_t kAtlasHeight = 1152;
		// The largest movie the graph draws: 4096x2304 (a 3840x2160 movie fits). A larger one is the engine's: its staging buffers
		// would be over 37 MB each.
		constexpr std::uint32_t kMaxVideoWidth = 4096;
		constexpr std::uint32_t kMaxVideoHeight = 2304;
		// Staging buffers: the frame the decoder fills, one for each frame in flight, and the last decoded frame, kept for ticks that
		// decode nothing. With none to take (a capacity refusal, logged), a tick decodes on the engine's path.
		constexpr std::uint32_t kMaxStaging = RenderGraphRuntime::kGameFramesInFlight + 2;

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

	/** @brief One tile: its rectangle of the video, where its bytes sit in a staging buffer, and its place in the movie texture. */
	struct Tile
	{
		TileDesc desc;
		std::uint64_t offset = 0;
		std::uint32_t rowPitch = 0;
		std::uint32_t atlasX = 0, atlasY = 0;  // in its chunk's upload
		bool edge = false;                     // a power-of-two edge tile: the decoder fills only the video's part of it
	};

	class Movie
	{
	public:
		std::vector<Tile> tiles;                         // the engine's order: the decoder's tile index
		std::vector<std::vector<std::uint32_t>> chunks;  // tiles uploaded and drawn together, each chunk fitting the movie texture
		std::uint64_t stagingBytes = 0;
		// Render thread.
		std::shared_ptr<org::LatchBlock> decoding;  // this tick's staging buffer, from the first lock
		std::shared_ptr<org::LatchBlock> last;      // the newest decoded frame, while the engine's tiles hold no newer one
	};

	namespace
	{
		/** @brief What one run of a target's pass draws: immutable once the commit stored it. */
		struct Frame
		{
			std::shared_ptr<const Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;
			std::shared_ptr<const RasterProgram> program;
			ShaderBindings::Handle sampler = 0;
			std::shared_ptr<const void> samplerOwner;
			std::uint32_t width = 0, height = 0;
		};

		/** @brief A frame queued for the Present anchor's run, or for the MovieTiles anchor's (a movie inside another frame). */
		struct Pending
		{
			std::shared_ptr<Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;
			bool hdr = false;      // into HDR's UI texture (HDR Display redirected the framebuffer there for the movie)
			bool inFrame = false;  // player mode 2: drawn at the MovieTiles anchor, at the tile draw's point
		};

		FrameAnchors::Anchor AnchorFor(bool a_inFrame) { return a_inFrame ? FrameAnchors::Anchor::MovieTiles : FrameAnchors::Anchor::Present; }

		/** @brief A texture the tiles are drawn onto: the framebuffer, or HDR's UI texture while HDR output is on. */
		struct DrawTarget
		{
			const char* name = "";
			std::shared_ptr<org::ExternalTextureResource> resource;
			winrt::com_ptr<ID3D11Texture2D> texture;  // DXVK's: the target's identity
			std::uint32_t width = 0, height = 0;
			rhi::Format format = rhi::Format::Unknown;  // of its RTV
			std::shared_ptr<const RunSlots<Frame>> slots;         // the Present anchor's pass
			std::shared_ptr<const RunSlots<Frame>> inFrameSlots;  // the MovieTiles anchor's pass

			const std::shared_ptr<const RunSlots<Frame>>& SlotsAt(bool a_inFrame) const { return a_inFrame ? inFrameSlots : slots; }
		};

		struct BinkState
		{
			bool setUp = false;
			bool failed = false;
			std::unique_ptr<GraphResources::Texture2D> atlas;
			std::shared_ptr<FrameStaging> staging;
			DrawTarget framebuffer{ .name = "the framebuffer" };
			DrawTarget hdrUi{ .name = "HDR's UI texture" };
			org::OwnedDescriptorBinding sampler;
			std::optional<Pending> pending;
			std::uint64_t queued = 0;     // frames queued
			std::uint64_t committed = 0;  // the newest queued frame a commit stored
			std::uint64_t executed = 0;   // the newest committed frame whose run had the passes in its graph
			std::uint64_t lost = 0;       // queued frames that did not draw (logged as errors)
		};

		BinkState& Get()
		{
			static BinkState s_state;
			return s_state;
		}

		/** @brief The tile program per target format: built at setup for each target's format, and when a target's format changed. */
		ProgramCache<RasterProgram, rhi::Format>& Programs()
		{
			static ProgramCache<RasterProgram, rhi::Format> programs{ "Bink" };
			return programs;
		}

		/** @brief The tile program for a_format, built on the calling thread when it is not built yet (logged); null only when it failed. */
		std::shared_ptr<const RasterProgram> ProgramFor(rhi::Format a_format)
		{
			return Programs().Require(a_format, [&] {
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

		struct DrawBindings
		{
			org::DeclaredViewToken target;
			org::DeclaredViewToken atlas;
		};

		struct PassFrame
		{
			std::shared_ptr<const RunSlots<Frame>> slots;
		};

		/** @brief The movie texture between the state the graph keeps it in for these passes (a shader resource) and a copy's. */
		rhi::TextureBarrier AtlasBarrier(rhi::ResourceHandle a_texture, bool a_toCopy)
		{
			rhi::TextureBarrier barrier{};
			barrier.texture = a_texture;
			barrier.beforeSync = rhi::ResourceSyncState::All;
			barrier.afterSync = rhi::ResourceSyncState::All;
			const auto shader = rhi::ResourceAccessType::ShaderResource;
			const auto copy = rhi::ResourceAccessType::CopyDest;
			barrier.beforeAccess = a_toCopy ? shader : copy;
			barrier.afterAccess = a_toCopy ? copy : shader;
			barrier.beforeLayout = a_toCopy ? rhi::ResourceLayout::ShaderResource : rhi::ResourceLayout::CopyDest;
			barrier.afterLayout = a_toCopy ? rhi::ResourceLayout::CopyDest : rhi::ResourceLayout::ShaderResource;
			// Each chunk overwrites the tiles it draws, and nothing else of the texture is read.
			barrier.discard = a_toCopy;
			return barrier;
		}

		/**
		 * @brief The movie's tiles drawn onto one target, chunk by chunk: each chunk's tiles copied from the frame's staging buffer into
		 * the movie texture, then drawn from it. The movie texture is declared as a shader resource; the pass moves it to a copy
		 * destination and back around each chunk's copies, so the graph finds it as it declared it. Records nothing on a run with no
		 * frame for this target (the slot is empty). Recorded every run (no Revision): the slot decides.
		 */
		class DrawPass final : public org::TypedRenderGraphPass<DrawPass, PassFrame, DrawBindings>
		{
		public:
			DrawPass(std::shared_ptr<org::ExternalTextureResource> a_target, std::shared_ptr<org::Resource> a_atlas, std::shared_ptr<const RunSlots<Frame>> a_slots) :
				target(std::move(a_target)), atlas(std::move(a_atlas)), slots(std::move(a_slots)) {}

			DrawBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(target).View(), a_builder.ShaderResource(atlas).View() }; }

			PassFrame Prepare(const DrawBindings&, const org::PassPrepareContext&) const { return { slots }; }

			static void Record(const DrawBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				const auto frame = a_pass.slots->Take(a_recording.Run());
				if (!frame || !frame->program || !frame->movie || !frame->staging)
					return;
				// The staging buffer stays until this recording's GPU work retires.
				a_recording.Retain(frame);
				auto& commands = a_recording.Commands();
				const auto atlasTexture = a_recording.Resolve(a_bindings.atlas).GetHandle();
				const auto staging = frame->staging->Resource()->GetAPIResource().GetHandle();
				// The engine's tick cleared the target (its SetRenderTarget with the clear mode, or HDR Display's clear of its UI texture)
				// before this run: the tiles go over it.
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
				QuadBlock block{};
				block.texture = a_recording.Index(a_bindings.atlas);
				block.sampler = frame->sampler;
				for (const auto& chunk : frame->movie->chunks) {
					auto toCopy = AtlasBarrier(atlasTexture, true);
					commands.Barriers(rhi::BarrierBatch{ { &toCopy, 1 }, {}, {} });
					for (const auto index : chunk) {
						const auto& tile = frame->movie->tiles[index];
						rhi::BufferTextureCopyFootprint copy{};
						copy.texture = atlasTexture;
						copy.buffer = staging;
						copy.x = tile.atlasX;
						copy.y = tile.atlasY;
						copy.footprint = { tile.offset, tile.rowPitch, tile.desc.width, tile.desc.height, 1 };
						commands.CopyBufferToTexture(copy);
					}
					auto toShader = AtlasBarrier(atlasTexture, false);
					commands.Barriers(rhi::BarrierBatch{ { &toShader, 1 }, {}, {} });

					commands.BeginPass(begin);
					commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					commands.BindLayout(frame->program->layout->GetHandle());
					commands.BindPipeline(frame->program->Pipeline());
					for (const auto index : chunk) {
						const auto& tile = frame->movie->tiles[index];
						std::memcpy(block.rect, tile.desc.rect, sizeof(block.rect));
						// The tile's own texels of the movie texture; the pixel stage clamps to them, as the engine's separate textures clamp.
						block.uv[0] = static_cast<float>(tile.atlasX) / kAtlasWidth;
						block.uv[1] = static_cast<float>(tile.atlasY) / kAtlasHeight;
						block.uv[2] = static_cast<float>(tile.atlasX + tile.desc.width) / kAtlasWidth;
						block.uv[3] = static_cast<float>(tile.atlasY + tile.desc.height) / kAtlasHeight;
						commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kQuadWords, reinterpret_cast<const std::uint32_t*>(&block));
						commands.Draw(6, 1, 0, 0);
					}
					commands.EndPass();
				}
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
			// First in the Present anchor: the movie frame is the image the other Present clients draw over. The MovieTiles anchor is
			// Bink's alone.
			const auto add = [&](const DrawTarget& a_target, bool a_inFrame, const char* a_name, std::int32_t a_order) {
				if (!a_target.resource)
					return;
				client.passes.push_back({ .name = a_name,
					.anchor = AnchorFor(a_inFrame),
					.order = a_order,
					.kind = FrameAnchors::PassKind::Render,
					.make = [resource = a_target.resource, atlas = state.atlas->Graph(), slots = a_target.SlotsAt(a_inFrame)] {
						return std::make_shared<DrawPass>(resource, atlas, slots);
					} });
			};
			add(state.framebuffer, false, "cs.bink.tiles", -1000);
			add(state.hdrUi, false, "cs.bink.tiles.hdr", -999);
			if (FrameAnchors::HasEpoch(FrameAnchors::Anchor::MovieTiles)) {
				add(state.framebuffer, true, "cs.bink.tiles.in-frame", 0);
				add(state.hdrUi, true, "cs.bink.tiles.in-frame.hdr", 1);
			}
			client.registerResources = [](org::RenderGraph& a_graph) { Get().atlas->Register(a_graph); };
			client.commit = [](const FrameAnchors::Point& a_point) {
				auto& current = Get();
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const bool inFrame = a_point.anchor == FrameAnchors::Anchor::MovieTiles;
				const org::RunRef run = host->CurrentRun();
				// Every target's entry for this run emptied first, so nothing older is left there.
				current.framebuffer.SlotsAt(inFrame)->Store(run, nullptr);
				current.hdrUi.SlotsAt(inFrame)->Store(run, nullptr);
				// A frame is drawn by the anchor it was queued for.
				if (!current.pending || current.pending->inFrame != inFrame)
					return;
				auto pending = std::move(*current.pending);
				current.pending.reset();
				const DrawTarget& target = pending.hdr ? current.hdrUi : current.framebuffer;
				auto frame = std::make_shared<Frame>();
				frame->movie = pending.movie;
				frame->staging = std::move(pending.staging);
				frame->program = ProgramFor(target.format);
				frame->sampler = current.sampler.Index();
				frame->samplerOwner = current.sampler.Owner();
				frame->width = target.width;
				frame->height = target.height;
				target.SlotsAt(inFrame)->Store(run, std::move(frame));
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

		/** @brief a_rtv's texture (DXVK's, referenced) and view format; null for a view that is not of a 2D texture. */
		winrt::com_ptr<ID3D11Texture2D> TextureOf(ID3D11RenderTargetView* a_rtv, DXGI_FORMAT& a_format)
		{
			a_format = DXGI_FORMAT_UNKNOWN;
			if (!a_rtv)
				return nullptr;
			// The engine's tables hold tripwire handles with the guest device: DXVK's view is behind it.
			a_rtv = Tripwire::Unwrap(a_rtv);
			winrt::com_ptr<ID3D11Resource> resource;
			a_rtv->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return nullptr;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_rtv->GetDesc(&desc);
			a_format = desc.Format;
			return texture;
		}

		/** @brief The engine's back buffer RTV: HDR's saved one while HDR points kFRAMEBUFFER at its UI texture. */
		ID3D11RenderTargetView* FramebufferRtv()
		{
			auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].RTV);
			const auto& hdr = globals::features::hdrDisplay;
			if (hdr.uiTexture && hdr.uiTexture->rtv && rtv == hdr.uiTexture->rtv.get() && hdr.savedFramebufferRTV)
				rtv = hdr.savedFramebufferRTV;
			return rtv;
		}

		/**
		 * @brief a_target made from a_texture when that texture is not the one it has: imported (the framebuffer through FrameAnchors,
		 * HDR's UI texture by ORGHDRDisplay's name, with every view so both imports describe it alike), and its format's tile program
		 * built. Returns whether the target changed; a_ok is false when it cannot be drawn onto.
		 */
		bool Update(DrawTarget& a_target, winrt::com_ptr<ID3D11Texture2D> a_texture, DXGI_FORMAT a_format, bool a_isFramebuffer, bool& a_ok)
		{
			a_ok = true;
			if (!a_texture) {
				const bool had = a_target.resource != nullptr;
				a_target.resource.reset();
				a_target.texture = nullptr;
				return had;
			}
			if (a_texture == a_target.texture)
				return false;
			std::shared_ptr<org::ExternalTextureResource> resource;
			if (a_isFramebuffer) {
				resource = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
			} else {
				const auto& ui = *globals::features::hdrDisplay.uiTexture;
				resource = FrameAnchors::D3D11Texture(kHdrUiName, ui.resource.get(), ui.srv.get(), ui.uav.get(), ui.rtv.get());
			}
			const auto format = rhi::helpers::ToRHI(a_format);
			if (!resource || format == rhi::Format::Unknown || !ProgramFor(format)) {
				logger::warn("[ORG] Bink: {} cannot be drawn onto: {}", a_target.name,
					!resource ? "it cannot be imported" : (format == rhi::Format::Unknown ? "its view's format has no RHI form" : "its tile program cannot be built"));
				a_ok = false;
				return false;
			}
			D3D11_TEXTURE2D_DESC desc{};
			a_texture->GetDesc(&desc);
			a_target.resource = std::move(resource);
			a_target.texture = std::move(a_texture);
			a_target.width = desc.Width;
			a_target.height = desc.Height;
			a_target.format = format;
			logger::info("[ORG] Bink: drawing onto {} ({}x{}, DXGI format {})", a_target.name, desc.Width, desc.Height, static_cast<int>(a_format));
			return true;
		}

		/**
		 * @brief The framebuffer and, while HDR has one, HDR's UI texture as the targets, imported, with their formats' tile programs
		 * built. When either was made again (a resize, HDR's textures made again) the client is refreshed. a_build: the caller is
		 * between two frames (a blocking movie's tick before it begins its frame), where the graph is built at once, so no tick is the
		 * engine's for it. False when the framebuffer cannot be drawn onto.
		 */
		bool UpdateTargets(BinkState& a_state, bool a_build)
		{
			DXGI_FORMAT format{};
			bool ok = true;
			auto framebuffer = TextureOf(FramebufferRtv(), format);
			if (!framebuffer) {
				logger::warn("[ORG] Bink: kFRAMEBUFFER has no RTV of a 2D texture");
				return false;
			}
			bool changed = Update(a_state.framebuffer, std::move(framebuffer), format, true, ok);
			if (!ok || !a_state.framebuffer.resource)
				return false;
			const auto& hdr = globals::features::hdrDisplay;
			winrt::com_ptr<ID3D11Texture2D> ui;
			if (hdr.loaded && hdr.uiTexture && hdr.uiTexture->rtv && hdr.uiTexture->resource)
				ui = TextureOf(hdr.uiTexture->rtv.get(), format);
			bool hdrOk = true;
			changed = Update(a_state.hdrUi, std::move(ui), format, false, hdrOk) || changed;
			if (!hdrOk)
				EngineDraws().Report("HDR's UI texture cannot be imported, or its tile program cannot be made");
			if (!changed || !a_state.setUp)
				return true;
			logger::info("[ORG] Bink: a target was made again (a resize or an HDR change); the Present graph is built again with it");
			FrameAnchors::Refresh(kClientId);
			if (a_build)
				(void)FrameAnchors::BuildPending("Bink's tick after its target was made again");
			return true;
		}

		/**
		 * @brief The movie's tiles in chunks that each fit the movie texture: bands of whole columns and whole rows of the tile grid,
		 * each tile at its place in its band. A movie up to 2048x1152 (tiles included) is one chunk.
		 */
		void MakeChunks(Movie& a_movie, std::uint32_t a_tileSize)
		{
			std::uint32_t columns = 0, rows = 0;
			for (const auto& tile : a_movie.tiles) {
				columns = (std::max)(columns, tile.desc.x / a_tileSize + 1);
				rows = (std::max)(rows, tile.desc.y / a_tileSize + 1);
			}
			// A column's width is its tiles' (the last column's may be an edge tile's power of two); likewise a row's height.
			std::vector<std::uint32_t> widths(columns, 0), heights(rows, 0);
			for (const auto& tile : a_movie.tiles) {
				auto& width = widths[tile.desc.x / a_tileSize];
				auto& height = heights[tile.desc.y / a_tileSize];
				width = (std::max)(width, tile.desc.width);
				height = (std::max)(height, tile.desc.height);
			}
			// Consecutive columns (rows) while they fit: each one's band and its start there.
			const auto bands = [](const std::vector<std::uint32_t>& a_sizes, std::uint32_t a_limit, std::vector<std::uint32_t>& a_band, std::vector<std::uint32_t>& a_start) {
				a_band.assign(a_sizes.size(), 0);
				a_start.assign(a_sizes.size(), 0);
				std::uint32_t current = 0, used = 0;
				for (std::size_t i = 0; i < a_sizes.size(); ++i) {
					if (used && used + a_sizes[i] > a_limit) {
						++current;
						used = 0;
					}
					a_band[i] = current;
					a_start[i] = used;
					used += a_sizes[i];
				}
				return current + 1;
			};
			std::vector<std::uint32_t> columnBand, columnStart, rowBand, rowStart;
			const std::uint32_t columnBands = bands(widths, kAtlasWidth, columnBand, columnStart);
			(void)bands(heights, kAtlasHeight, rowBand, rowStart);
			a_movie.chunks.clear();
			std::vector<std::int32_t> chunkOf;  // a band pair's chunk
			for (std::uint32_t i = 0; i < a_movie.tiles.size(); ++i) {
				auto& tile = a_movie.tiles[i];
				const std::uint32_t column = tile.desc.x / a_tileSize;
				const std::uint32_t row = tile.desc.y / a_tileSize;
				tile.atlasX = columnStart[column];
				tile.atlasY = rowStart[row];
				const std::uint32_t key = rowBand[row] * columnBands + columnBand[column];
				if (chunkOf.size() <= key)
					chunkOf.resize(key + 1, -1);
				if (chunkOf[key] < 0) {
					chunkOf[key] = static_cast<std::int32_t>(a_movie.chunks.size());
					a_movie.chunks.emplace_back();
				}
				a_movie.chunks[static_cast<std::size_t>(chunkOf[key])].push_back(i);
			}
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
		// One texture for the session, filled again by every frame's chunks.
		state.atlas = std::make_unique<GraphResources::Texture2D>(atlas, "Bink::Movie", GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		state.staging = FrameStaging::Create({ .name = "cs.bink.staging",
			.minimumBytes = 1u << 20,
			// The Present anchor runs once a frame.
			.runsPerFrame = 1,
			.maxBlocks = kMaxStaging });
		for (auto* target : { &state.framebuffer, &state.hdrUi }) {
			target->slots = std::make_shared<const RunSlots<Frame>>();
			target->inFrameSlots = std::make_shared<const RunSlots<Frame>>();
		}
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
		// Each part named: a sampler needs the runtime's descriptor service, the framebuffer the engine's targets.
		const char* missing = !*state.atlas ? "its movie texture could not be made" :
		                      !state.staging ? "its staging pool could not be made" :
		                      !state.sampler ? "its sampler could not be made (no descriptor service)" :
		                      !UpdateTargets(state, false) ? "the framebuffer cannot be imported, or its tile program cannot be built" :
		                                                     nullptr;
		if (missing) {
			Fail(state, missing);
			return;
		}
		// One client for the session: every movie draws through it, so a movie never rebuilds the graph.
		if (!FrameAnchors::Register(MakeClient())) {
			Fail(state, "the Present anchor's client could not be registered");
			return;
		}
		state.setUp = true;
		logger::info("[ORG] Bink: on (CS_ORG_BINK); registered at the Present anchor for the session, joined at setup's build point");
	}

	bool Begin(bool a_betweenFrames, bool a_inFrame)
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
		// A frame queued last tick that did not draw (its run did not submit with the passes) is an error, not a reason to stop: the
		// next ticks draw on the graph as before.
		if (state.executed < state.queued) {
			state.lost += state.queued - state.executed;
			logger::error("[ORG] Bink: a movie frame queued for its anchor did not draw ({} so far)", state.lost);
			state.executed = state.queued;
		}
		state.pending.reset();
		if (!UpdateTargets(state, a_betweenFrames))
			return refuse("the framebuffer cannot be imported, or its tile program cannot be made");
		// The setup's build point put the client in its anchors' graphs; a target made again inside a frame waits for the next build.
		const auto anchor = AnchorFor(a_inFrame);
		if (a_inFrame && !FrameAnchors::HasEpoch(anchor))
			return refuse("a movie inside another frame without the MovieTiles anchor");
		if (!FrameAnchors::CanRun(kClientId, anchor))
			return refuse(FrameAnchors::InBuiltGraph(kClientId, anchor) ? "its anchor's ring is full this frame" :
			                                                              "a target was made again inside a frame (until the graph is built with it)");
		return true;
	}

	std::shared_ptr<Movie> CreateMovie(const Layout& a_layout, std::uint32_t a_tileSize)
	{
		if (!Get().setUp || a_layout.tiles.empty() || !a_tileSize || a_tileSize > kAtlasWidth || a_tileSize > kAtlasHeight)
			return nullptr;
		if (a_layout.videoWidth > kMaxVideoWidth || a_layout.videoHeight > kMaxVideoHeight) {
			logger::info("[ORG] Bink: a {}x{} movie is larger than the {}x{} the graph draws; the engine draws it", a_layout.videoWidth, a_layout.videoHeight,
				kMaxVideoWidth, kMaxVideoHeight);
			return nullptr;
		}
		auto movie = std::make_shared<Movie>();
		std::uint64_t bytes = 0;
		for (const auto& desc : a_layout.tiles) {
			if (!desc.width || !desc.height || desc.width > a_tileSize || desc.height > a_tileSize)
				return nullptr;
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
		MakeChunks(*movie, a_tileSize);
		logger::info("[ORG] Bink: a {}x{} movie in {} tiles ({} chunks of the movie texture), {} bytes of staging a frame", a_layout.videoWidth,
			a_layout.videoHeight, movie->tiles.size(), movie->chunks.size(), bytes);
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
		std::byte* const data = a_movie.decoding->Bytes().data() + tile.offset;
		// The decoder writes the video's part of an edge tile; the rest is drawn as the engine's zero-filled texels.
		if (tile.edge)
			std::memset(data, 0, std::size_t(tile.rowPitch) * tile.desc.height);
		a_rowPitch = tile.rowPitch;
		return data;
	}

	bool Queue(const std::shared_ptr<Movie>& a_movie, bool a_hdr, bool a_inFrame)
	{
		auto& state = Get();
		if (state.failed || !state.setUp || !a_movie)
			return false;
		if (a_hdr && !state.hdrUi.resource)
			return false;
		if (a_movie->decoding) {
			a_movie->last = std::move(a_movie->decoding);
			a_movie->decoding.reset();
		} else if (!a_movie->last) {
			return false;
		}
		// A tick that decoded nothing draws the last decoded frame again.
		state.pending = Pending{ a_movie, a_movie->last, a_hdr, a_inFrame };
		++state.queued;
		return true;
	}

	bool DrawInFrame()
	{
		auto& state = Get();
		if (!state.pending || !state.pending->inFrame)
			return false;
		const auto result = FrameAnchors::Run(FrameAnchors::Anchor::MovieTiles);
		state.pending.reset();
		if (result && state.executed == state.queued)
			return true;
		// A failure, not a fallback: logged, and the next ticks draw on the graph as before.
		state.lost += state.queued - state.executed;
		state.executed = state.queued;
		logger::error("[ORG] Bink: a movie frame inside another frame did not draw at the MovieTiles anchor ({} so far)", state.lost);
		return false;
	}

	void EndTick(Movie& a_movie)
	{
		a_movie.decoding.reset();
	}

	void MarkStale(Movie& a_movie)
	{
		// The engine's tiles hold a newer frame: the last decoded one is not what a tick that decodes nothing shows any more.
		a_movie.last.reset();
	}

	void MovieEnded()
	{
		auto& state = Get();
		state.pending.reset();
		// The client stays registered for the next movie; the next movie starts from a clean count.
		state.queued = state.committed = state.executed = 0;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::Bink::Graph
{
	class Movie
	{};
	void Setup() {}
	bool Begin(bool, bool) { return false; }
	std::shared_ptr<Movie> CreateMovie(const Layout&, std::uint32_t) { return nullptr; }
	std::byte* Lock(Movie&, std::uint32_t, std::uint32_t&) { return nullptr; }
	bool Queue(const std::shared_ptr<Movie>&, bool, bool) { return false; }
	bool DrawInFrame() { return false; }
	void EndTick(Movie&) {}
	void MarkStale(Movie&) {}
	void MovieEnded() {}
}

#endif
