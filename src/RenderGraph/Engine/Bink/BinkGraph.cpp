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
		// Follow BSMovie::CreateTileTextures's arithmetic in its order, so the quads land on the same pixels.
		const float screenWidth = static_cast<float>(a_screenWidth);
		const float screenHeight = static_cast<float>(a_screenHeight);
		const float tile = static_cast<float>(a_tileSize);
		const float wholeWidth = (tile / screenWidth) * a_scale;
		const float wholeHeight = (tile / screenHeight) * a_scale;
		const float edgeWidth = (static_cast<float>(a_edgeWidth) / screenWidth) * a_scale;
		const float edgeHeight = (static_cast<float>(a_edgeHeight) / screenHeight) * a_scale;
		const float stepX = a_scale / screenWidth;
		const float stepY = a_scale / screenHeight;

		// BSMovie::CreateTileQuad (SE 0x1410a2720) maps a screen rectangle, 0..1 with y down, to clip space.
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
#	include "Features/HDRDisplay/ORGHDRDisplay.h"
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
		constexpr const char* kHdrUiName = "hdr.ui";  // ORGHDRDisplay's import name, so both share one graph resource
		// D3D12's copy rules, which Vulkan's satisfy. Texture copy rows are 256-byte aligned and a copy starts at 512.
		constexpr std::uint32_t kRowAlignment = 256;
		constexpr std::uint64_t kPlacementAlignment = 512;
		// A 1920x1080 movie fits in one chunk. Larger movies are drawn in chunks of tile bands.
		constexpr std::uint32_t kAtlasWidth = 2048;
		constexpr std::uint32_t kAtlasHeight = 1152;

		// Must match the push block of BinkCopy.hlsl.
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

	struct Tile
	{
		TileDesc desc;
		std::uint64_t offset = 0;
		std::uint32_t rowPitch = 0;
		std::uint32_t atlasX = 0, atlasY = 0;  // in its chunk's upload
		bool edge = false;                     // the decoder fills only the video's part of a power-of-two edge tile
	};

	class Movie
	{
	public:
		std::vector<Tile> tiles;                         // the decoder's tile index
		std::vector<std::vector<std::uint32_t>> chunks;  // each chunk fits the movie texture
		std::uint64_t stagingBytes = 0;
		// Render thread.
		std::shared_ptr<org::LatchBlock> decoding;  // this tick's staging buffer, from the first lock
		std::shared_ptr<org::LatchBlock> last;      // cleared once the engine's tiles hold a newer one
	};

	namespace
	{
		/** @brief What one run of a target's pass draws. Immutable once stored. */
		struct Frame
		{
			std::shared_ptr<const Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;
			std::shared_ptr<const RasterProgram> program;
			ShaderBindings::Handle sampler = 0;
			std::shared_ptr<const void> samplerOwner;
			std::uint32_t width = 0, height = 0;
		};

		struct Pending
		{
			std::shared_ptr<Movie> movie;
			std::shared_ptr<org::LatchBlock> staging;
			bool hdr = false;  // HDR Display redirected the framebuffer there for the movie
		};

		/** @brief The framebuffer, or HDR's UI texture while HDR output is on. */
		struct DrawTarget
		{
			const char* name = "";
			FrameAnchors::Texture resource;
			winrt::com_ptr<ID3D11Texture2D> texture;  // the target's identity
			std::uint32_t width = 0, height = 0;
			rhi::Format format = rhi::Format::Unknown;
			std::shared_ptr<const RunSlots<Frame>> slots;
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
			std::uint64_t queued = 0;
			std::uint64_t committed = 0;
			std::uint64_t executed = 0;   // the newest committed frame whose run had the passes
			std::uint64_t lost = 0;       // logged as errors
		};

		BinkState& Get()
		{
			static BinkState s_state;
			return s_state;
		}

		/** @brief The tile program per target format. */
		ProgramCache<RasterProgram, rhi::Format>& Programs()
		{
			static ProgramCache<RasterProgram, rhi::Format> programs{ "Bink" };
			return programs;
		}

		/** @brief The tile program for a_format, built here if needed. Null only on failure. */
		std::shared_ptr<const RasterProgram> ProgramFor(rhi::Format a_format)
		{
			return Programs().Require(a_format, [&] {
				RasterProgram::Desc desc{};
				desc.vertexSource = kShader;
				desc.vertexEntry = L"VSMain";
				desc.pixelSource = kShader;
				desc.pixelEntry = L"PSMain";
				desc.constantWords = kQuadWords;
				// BSMovie::DrawTiles turns blending and depth off, the engine's rasterizer culls nothing, and the quads wind counter-clockwise.
				desc.targets = { RasterProgram::Target{ .format = a_format } };
				return desc;
			});
		}

		/**
		 * @brief Turns Bink's graph path off for the session, with one error line. Later ticks use the engine's tile draw,
		 * which only draws where engine draws run.
		 */
		void Fail(BinkState& a_state, const char* a_why)
		{
			if (a_state.failed)
				return;
			a_state.failed = true;
			a_state.pending.reset();
			FrameAnchors::Unregister(kClientId);
			logger::error("[ORG] Bink: off for the session: {}. Movies go to the engine's tile draw, which native mode and the flush latch skip", a_why);
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

		/** @brief Moves the movie texture between its declared shader-resource state and a copy destination. */
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
		 * @brief Draws the movie onto one target chunk by chunk, copying each chunk from staging and drawing from it.
		 * The pass returns the texture to shader-resource after each chunk, as declared. An empty slot records nothing.
		 */
		class DrawPass final : public org::TypedRenderGraphPass<DrawPass, PassFrame, DrawBindings>
		{
		public:
			DrawPass(FrameAnchors::Texture a_target, std::shared_ptr<org::Resource> a_atlas, std::shared_ptr<const RunSlots<Frame>> a_slots, bool a_late) :
				target(std::move(a_target)), atlas(std::move(a_atlas)), slots(std::move(a_slots)), late(a_late) {}

			DrawBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(target).View(), a_builder.ShaderResource(atlas).View() }; }

			// Recorded late in the UI stage, the slot is read at record time so the kept recording stays current.
			// Recorded ahead in an anchor's own epoch, every run is a new revision.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				if (!late)
					a_out.push_back(++preparations);
			}

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
				// The engine's tick cleared the target before this run, so the tiles draw over it.
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
						// The pixel stage clamps to the tile's own texels, as the engine's separate textures clamp.
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
			FrameAnchors::Texture target;
			std::shared_ptr<org::Resource> atlas;
			std::shared_ptr<const RunSlots<Frame>> slots;
			bool late = false;
			mutable std::uint64_t preparations = 0;
		};

		FrameAnchors::ClientDesc MakeClient()
		{
			auto& state = Get();
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			// First in the Present anchor, since the other Present clients draw over the movie frame.
			const bool late = FrameAnchors::HeldByStage(FrameAnchors::Anchor::Present);
			const auto add = [&](const DrawTarget& a_target, const char* a_name, std::int32_t a_order) {
				if (!a_target.resource)
					return;
				client.passes.push_back({ .name = a_name,
					.anchor = FrameAnchors::Anchor::Present,
					.order = a_order,
					.kind = FrameAnchors::PassKind::Render,
					.make = [resource = a_target.resource, atlas = state.atlas->Graph(), slots = a_target.slots, late] {
						return std::make_shared<DrawPass>(resource, atlas, slots, late);
					},
					.atExecution = late });
			};
			add(state.framebuffer, "cs.bink.tiles", -1000);
			add(state.hdrUi, "cs.bink.tiles.hdr", -999);
			client.registerResources = [](org::RenderGraph& a_graph) { Get().atlas->Register(a_graph); };
			client.commit = [](const FrameAnchors::Point&) {
				auto& current = Get();
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const org::RunRef run = host->CurrentRun();
				// Empty every target's entry for this run first, so nothing older is left there.
				current.framebuffer.slots->Store(run, nullptr);
				current.hdrUi.slots->Store(run, nullptr);
				if (!current.pending)
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
				target.slots->Store(run, std::move(frame));
				current.committed = current.queued;
			};
			client.active = [] { return Get().pending.has_value(); };
			client.executed = [](const FrameAnchors::Point&) {
				auto& current = Get();
				current.executed = current.committed;
			};
			return client;
		}

		winrt::com_ptr<ID3D11Texture2D> TextureOf(ID3D11RenderTargetView* a_rtv, DXGI_FORMAT& a_format)
		{
			a_format = DXGI_FORMAT_UNKNOWN;
			if (!a_rtv)
				return nullptr;
			// With the guest device the engine's tables hold tripwire handles, so unwrap to DXVK's view.
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

		/** @brief The back buffer RTV, which is HDR's saved one while HDR points kFRAMEBUFFER at its UI texture. */
		ID3D11RenderTargetView* FramebufferRtv()
		{
			auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].RTV);
			const auto& hdr = globals::features::hdrDisplay;
			if (hdr.uiTexture && hdr.uiTexture->rtv && rtv == hdr.uiTexture->rtv.get() && hdr.savedFramebufferRTV)
				rtv = hdr.savedFramebufferRTV;
			return rtv;
		}

		/** @brief Imports a_texture as a_target if it changed and builds its tile program. Returns whether the target changed. */
		bool Update(DrawTarget& a_target, winrt::com_ptr<ID3D11Texture2D> a_texture, DXGI_FORMAT a_format, bool a_isFramebuffer, bool& a_ok)
		{
			a_ok = true;
			if (!a_texture) {
				const bool had = static_cast<bool>(a_target.resource);
				a_target.resource = {};
				a_target.texture = nullptr;
				return had;
			}
			if (a_texture == a_target.texture)
				return false;
			FrameAnchors::Texture resource;
			if (a_isFramebuffer) {
				resource = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER));
			} else {
				const auto& ui = *globals::features::hdrDisplay.uiTexture;
				resource = ORGHDRDisplay::Get().GraphOf(ui.resource.get());
				if (!resource)
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
			// A new texture of the same import is the slot's next version, so only another graph resource changes the passes.
			const bool changed = resource != a_target.resource;
			a_target.resource = std::move(resource);
			a_target.texture = std::move(a_texture);
			a_target.width = desc.Width;
			a_target.height = desc.Height;
			a_target.format = format;
			logger::info("[ORG] Bink: drawing onto {} ({}x{}, DXGI format {}{})", a_target.name, desc.Width, desc.Height, static_cast<int>(a_format),
				changed ? "" : ", a new version of the same target");
			return changed;
		}

		/**
		 * @brief Imports the framebuffer and HDR's UI texture. The client is refreshed only when one becomes another graph resource.
		 * A resize is a new version and changes nothing in the graph. False when the framebuffer cannot be drawn onto.
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
			logger::info("[ORG] Bink: a target became another graph resource (HDR's UI texture); the Present graph is built again with it");
			FrameAnchors::Refresh(kClientId);
			if (a_build)
				(void)FrameAnchors::BuildPending("Bink's tick after its target was made again");
			return true;
		}

		/** @brief Splits the tiles into chunks of whole column and row bands that each fit the movie texture. */
		void MakeChunks(Movie& a_movie, std::uint32_t a_tileSize)
		{
			std::uint32_t columns = 0, rows = 0;
			for (const auto& tile : a_movie.tiles) {
				columns = (std::max)(columns, tile.desc.x / a_tileSize + 1);
				rows = (std::max)(rows, tile.desc.y / a_tileSize + 1);
			}
			// The last column or row may be an edge tile's power of two.
			std::vector<std::uint32_t> widths(columns, 0), heights(rows, 0);
			for (const auto& tile : a_movie.tiles) {
				auto& width = widths[tile.desc.x / a_tileSize];
				auto& height = heights[tile.desc.y / a_tileSize];
				width = (std::max)(width, tile.desc.width);
				height = (std::max)(height, tile.desc.height);
			}
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
			std::vector<std::int32_t> chunkOf;
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
		// One texture for the session, refilled by every frame's chunks.
		state.atlas = std::make_unique<GraphResources::Texture2D>(atlas, "Bink::Movie", GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		for (auto* target : { &state.framebuffer, &state.hdrUi })
			target->slots = std::make_shared<const RunSlots<Frame>>();
		// The engine's PS sampler for this draw, from CreateSamplerStates (SE 75831), is trilinear and clamped.
		D3D11_SAMPLER_DESC sampler{};
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.MaxAnisotropy = 1;
		sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		state.sampler = ShaderBindings::CreateSamplerFromD3D11(sampler);
		const char* missing = !*state.atlas ? "its movie texture could not be made" :
		                      !state.sampler ? "its sampler could not be made (no descriptor service)" :
		                      !UpdateTargets(state, false) ? "the framebuffer cannot be imported, or its tile program cannot be built" :
		                                                     nullptr;
		if (missing) {
			Fail(state, missing);
			return;
		}
		// One client for the session, so a movie never rebuilds the graph.
		if (!FrameAnchors::Register(MakeClient())) {
			Fail(state, "the Present anchor's client could not be registered");
			return;
		}
		state.setUp = true;
		logger::info("[ORG] Bink: on (CS_ORG_BINK); registered at the Present anchor for the session, joined at setup's build point");
	}

	bool Begin(bool a_betweenFrames)
	{
		const auto refuse = [](std::string_view a_reason) {
			EngineDraws().Report(a_reason);
			return false;
		};
		auto& state = Get();
		if (state.failed)
			return false;
		if (!state.setUp)
			return refuse("the graph path is not set up");
		if (!RenderGraphRuntime::Get().IsActive())
			return refuse("the render graph is not active");
		// A queued frame that did not draw is an error, but not a reason to stop.
		if (state.executed < state.queued) {
			state.lost += state.queued - state.executed;
			logger::error("[ORG] Bink: a movie frame queued for its anchor did not draw ({} so far)", state.lost);
			state.executed = state.queued;
		}
		state.pending.reset();
		if (!UpdateTargets(state, a_betweenFrames))
			return refuse("the framebuffer cannot be imported, or its tile program cannot be made");
		// The UI stage's last schedule must hold this registration, since a tick can come before the frame's Begin.
		// Until a build with HDR's new UI texture switches in, the tick stays the engine's.
		if (!FrameAnchors::HeldByStage(FrameAnchors::Anchor::Present))
			return refuse("no stage holds the Present anchor");
		if (!FrameAnchors::InStage(kClientId, FrameAnchors::Anchor::Present))
			return refuse("the UI stage's schedule does not hold the Bink client yet");
		return true;
	}

	std::shared_ptr<Movie> CreateMovie(const Layout& a_layout, std::uint32_t a_tileSize)
	{
		// The engine's tiles are 256 texels (set at +0x34), so a tile always fits the movie texture.
		if (!Get().setUp || a_layout.tiles.empty() || !a_tileSize || a_tileSize > kAtlasWidth || a_tileSize > kAtlasHeight)
			return nullptr;
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
		movie->stagingBytes = bytes;
		MakeChunks(*movie, a_tileSize);
		auto& state = Get();
		if (!state.staging)
			state.staging = FrameStaging::Create({ .name = "cs.bink.staging",
				.minimumBytes = 1u << 20,
				// Spares for each frame in flight and the last decoded frame. No cap, since larger movies need larger blocks.
				.runsPerFrame = 2 });
		logger::info("[ORG] Bink: a {}x{} movie in {} tiles ({} chunks of the movie texture), {} bytes of staging a frame", a_layout.videoWidth,
			a_layout.videoHeight, movie->tiles.size(), movie->chunks.size(), bytes);
		return movie;
	}

	std::byte* Lock(Movie& a_movie, std::uint32_t a_tile, std::uint32_t& a_rowPitch)
	{
		if (a_tile >= a_movie.tiles.size())
			return nullptr;
		if (!a_movie.decoding && Get().staging)
			a_movie.decoding = Get().staging->Acquire(a_movie.stagingBytes);
		if (!a_movie.decoding)
			return nullptr;
		const auto& tile = a_movie.tiles[a_tile];
		std::byte* const data = a_movie.decoding->Bytes().data() + tile.offset;
		// The decoder writes only the video's part of an edge tile, and the rest must match the engine's zero-filled texels.
		if (tile.edge)
			std::memset(data, 0, std::size_t(tile.rowPitch) * tile.desc.height);
		a_rowPitch = tile.rowPitch;
		return data;
	}

	bool Queue(const std::shared_ptr<Movie>& a_movie, bool a_hdr)
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
		state.pending = Pending{ a_movie, a_movie->last, a_hdr };
		++state.queued;
		return true;
	}

	void EndTick(Movie& a_movie)
	{
		a_movie.decoding.reset();
	}

	void MarkStale(Movie& a_movie)
	{
		// The engine's tiles hold a newer frame, so the last decoded one is stale.
		a_movie.last.reset();
	}

	void MovieEnded()
	{
		auto& state = Get();
		state.pending.reset();
		state.queued = state.committed = state.executed = 0;
		// Blocks still out are freed when their last holder lets go, not kept as spares.
		state.staging.reset();
		// The stages' kept recordings retain the movie's last frames, so drop them to free those frames once the GPU is past them.
		FrameAnchors::DropKeptRecordings(FrameAnchors::Anchor::Present, "a Bink movie ended");
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::Bink::Graph
{
	class Movie
	{};
	void Setup() {}
	bool Begin(bool) { return false; }
	std::shared_ptr<Movie> CreateMovie(const Layout&, std::uint32_t) { return nullptr; }
	std::byte* Lock(Movie&, std::uint32_t, std::uint32_t&) { return nullptr; }
	bool Queue(const std::shared_ptr<Movie>&, bool) { return false; }
	void EndTick(Movie&) {}
	void MarkStale(Movie&) {}
	void MovieEnded() {}
}

#endif
