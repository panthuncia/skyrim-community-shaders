#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "Features/HDRDisplay.h"
#include "Features/ScreenshotFeature.h"
#include "Globals.h"
#include "RenderGraph/Engine/Bink/BinkGraph.h"
#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/MainTick.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "Testing/Testing.h"
#include "RenderGraph/Switches.h"
#include "Utils/Logging.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <unordered_map>
#include <vector>

// The movie player's hooks. The RE is in docs/development/engine/bink.md. In graph mode a tick decodes and draws on the render
// graph, lags and draws nothing, or is a tap error and draws nothing. The engine never draws the tiles there.
namespace RenderGraph::Engine::Bink
{
	namespace
	{
		bool tickHooked = false;

		// BSMoviePlayer is 0x30 bytes.
		constexpr std::size_t kPlayerTiles = 0x08;
		constexpr std::size_t kPlayerFrames = 0x10;
		constexpr std::size_t kPlayerMode = 0x24;
		// Mode 1 is the blocking play loop, whose ticks present themselves. Mode 2 is ticked only by MoviePlayer::TickInFrame
		// (SE 87920, AE 90293), which nothing calls in SE 1.5.97 or AE 1.6.1170, so every tick is mode 1.
		constexpr std::uint32_t kModeBlocking = 1;

		// The tile set BSMovie::CreateTileTextures returns is 0x70 bytes, followed by the texture pointers.
		constexpr std::size_t kSetTileCount = 0x18;
		constexpr std::size_t kSetColumns = 0x1C;
		constexpr std::size_t kSetRows = 0x20;
		constexpr std::size_t kSetEdgeWidth = 0x24;
		constexpr std::size_t kSetEdgeHeight = 0x28;
		constexpr std::size_t kSetTileSize = 0x34;
		constexpr std::size_t kSetTextures = 0x38;

		template <class T>
		T Read(const void* a_base, std::size_t a_offset)
		{
			return *reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(a_base) + a_offset);
		}

		/** @brief Whether this thread owns Renderer::Lock, at Renderer +0x2790 SE or +0x27F0 AE. The startup logo holds it for the whole sequence. */
		bool HoldsRendererLock()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer)
				return false;
			const auto* lock = reinterpret_cast<const CRITICAL_SECTION*>(reinterpret_cast<const std::byte*>(renderer) + REL::Relocate(0x2790, 0x27F0));
			return lock->OwningThread == reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(::GetCurrentThreadId()));
		}

		/**
		 * @brief Claims rendering at the start of the logo's first tick, since its thread only takes rendering at Renderer::Begin, after
		 * Eligible ran. The thread holds Renderer::Lock for the whole sequence, so no other thread renders meanwhile. Once per ownership change.
		 */
		void ClaimForBlockingTick(std::uintptr_t a_player)
		{
			if (!Engine::GraphMode() || !a_player || !Engine::GraphMode() || FrameAnchors::IsRenderOwner())
				return;
			if (Read<std::uint32_t>(reinterpret_cast<const void*>(a_player), kPlayerMode) != kModeBlocking || !HoldsRendererLock())
				return;
			FrameAnchors::ClaimRenderOwnership();
			logger::info("[ORG] Bink: a blocking movie on thread {} under Renderer::Lock takes rendering at its tick's start{}", ::GetCurrentThreadId(),
				FrameAnchors::IsRenderOwner() ? "" : " (the claim did not take: the tick is a tap error)");
		}

		/** @brief The engine's in-frame flag (SE 0x14302c8e0, set by Renderer::Begin), from the BGSMoviePlayer's vfunc +0x58 as the tick reads it. */
		bool EngineInFrame()
		{
			static REL::Relocation<void**> player{ REL::RelocationID(525733, 412214) };
			void* const object = *player;
			if (!object)
				return false;
			using InFrame = bool (*)(void*);
			const auto* const vtable = *reinterpret_cast<InFrame* const*>(object);
			return vtable[0x58 / sizeof(void*)](object);
		}

		/**
		 * @brief A blocking tick must start between frames, so its own Begin starts a Bink frame. One that starts inside an open frame
		 * skips that Begin and would draw and present inside a frame described as another kind, so it is a tap error.
		 */
		void CheckFrameBoundary(std::uintptr_t a_player)
		{
			if (!Engine::GraphMode() || !a_player || !FrameAnchors::IsRenderOwner())
				return;
			if (Read<std::uint32_t>(reinterpret_cast<const void*>(a_player), kPlayerMode) != kModeBlocking || !EngineInFrame())
				return;
			const auto& frame = Frame::Driver::Current();
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NotDeclared, Frame::SegmentId::Count,
				fmt::format("a blocking movie tick on thread {} began inside frame {} ({}), so its frames cannot be Bink frames; a movie must start between frames",
					::GetCurrentThreadId(), frame.frame, Frame::KindName(frame.kind)));
		}

		/** @brief A tile set the engine made, with its layout, its tile textures and its graph side once made. */
		struct Record
		{
			Layout layout;
			std::uint32_t tileSize = 0;
			std::vector<const void*> blocks;
			std::shared_ptr<Graph::Movie> movie;
			bool movieFailed = false;
			std::uint64_t graphFrames = 0;
		};

		struct Records
		{
			std::mutex mutex;  // tile sets are made and freed on the thread that plays them, which for the logo is not the main thread
			std::unordered_map<const void*, Record> sets;
		};

		Records& GetRecords()
		{
			static Records s_records;
			return s_records;
		}

		/** @brief One tick of the player on this thread. */
		struct Tick
		{
			std::uintptr_t player = 0;
			const void* set = nullptr;
			std::shared_ptr<Graph::Movie> movie;  // null means nothing draws the tiles this tick
			bool lags = false;                    // decoded on the graph, but its frame is not queued
			std::vector<const void*> blocks;

			std::uint32_t TileOf(const void* a_block) const
			{
				const auto found = std::find(blocks.begin(), blocks.end(), a_block);
				return found == blocks.end() ? UINT32_MAX : static_cast<std::uint32_t>(found - blocks.begin());
			}
		};

		thread_local Tick* t_tick = nullptr;

		/** @brief Diagnostic. Decoded frames whose Present is saved as dclf-shots/bink-<n>.png. */
		const std::set<std::uint32_t>& ShotFrames()
		{
			static const std::set<std::uint32_t> frames = [] {
				std::set<std::uint32_t> result;
				for (const auto& item : Switches::List("CS_ORG_BINK_SHOT")) {
					std::uint32_t frame = 0;
					if (const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), frame); error == std::errc{} && end == item.data() + item.size())
						result.insert(frame);
					else
						logger::warn("[ORG] Bink: CS_ORG_BINK_SHOT lists {}, which is not a frame number", item);
				}
				return result;
			}();
			return frames;
		}

		void RequestShot(std::uintptr_t a_player, bool a_graph)
		{
			const auto& frames = ShotFrames();
			if (frames.empty() || !a_player || !FrameAnchors::IsRenderOwner())
				return;
			static std::set<std::uint32_t> taken;
			const auto frame = Read<std::uint32_t>(reinterpret_cast<const void*>(a_player), kPlayerFrames);
			if (!frames.contains(frame) || !taken.insert(frame).second)
				return;
			const auto dir = logger::log_directory().value_or(std::filesystem::path{}) / "dclf-shots";
			logger::info("[ORG] Bink: capturing decoded frame {} ({}) as bink-{}.png", frame, a_graph ? "render graph" : "engine", frame);
			globals::features::screenshotFeature.RequestCapture(dir / fmt::format("bink-{}.png", frame));
		}

		/** @brief A tick the graph has no place for. A tap error, and nothing draws its tiles. */
		void RefuseBinkTick(std::string_view a_why, const std::source_location& a_where = std::source_location::current())
		{
			Frame::Driver::NoteTapError(Frame::Driver::TapError::EngineDraw, Frame::SegmentId::Count, fmt::format("a Bink movie tick: {}", a_why), a_where);
		}

		/** @brief Whether this tick may draw on the graph. Every case the port does not cover is a tap error. */
		bool Eligible(std::uintptr_t a_player, const void* a_set)
		{
			if (!Engine::GraphMode() || !a_player || !a_set)
				return false;
			if (Read<std::uint32_t>(reinterpret_cast<const void*>(a_player), kPlayerMode) != kModeBlocking) {
				RefuseBinkTick("its player is not in mode 1 (no caller ticks another mode in SE or AE)");
				return false;
			}
			// A blocking movie takes rendering at its tick's start (ClaimForBlockingTick), the startup logo too.
			if (!FrameAnchors::IsRenderOwner()) {
				RefuseBinkTick(fmt::format("it plays on thread {}, which does not own rendering", ::GetCurrentThreadId()));
				return false;
			}
			return true;
		}

		/** @brief Makes a tile set's graph side the first time a tick of it draws on the graph. Render thread. */
		bool StartGraphTick(Tick& a_tick)
		{
			auto& records = GetRecords();
			std::scoped_lock lock(records.mutex);
			const auto found = records.sets.find(a_tick.set);
			if (found == records.sets.end()) {
				RefuseBinkTick("its tile set was not recorded at creation (a tile layout the port does not expect)");
				return false;
			}
			auto& record = found->second;
			if (!record.movie && !record.movieFailed) {
				record.movie = Graph::CreateMovie(record.layout, record.tileSize);
				record.movieFailed = !record.movie;
			}
			if (!record.movie) {
				RefuseBinkTick("the graph's movie could not be made");
				return false;
			}
			a_tick.movie = record.movie;
			a_tick.blocks = record.blocks;
			return true;
		}

		void GraphDrew(const void* a_set)
		{
			auto& records = GetRecords();
			std::scoped_lock lock(records.mutex);
			if (const auto found = records.sets.find(a_set); found != records.sets.end())
				++found->second.graphFrames;
		}

		/** @brief BSMoviePlayer::Tick (SE 87890, AE 90259). Decodes, clears kFRAMEBUFFER, draws the tiles and presents unless in mode 2. */
		struct PlayerTick
		{
			static std::uint64_t thunk(std::uintptr_t a_player, std::uintptr_t a_arg2, std::uintptr_t a_arg3, std::uintptr_t a_arg4)
			{
				const Frame::Driver::MovieTickScope frameKind;
				Tick tick;
				tick.player = a_player;
				tick.set = a_player ? Read<const void*>(reinterpret_cast<const void*>(a_player), kPlayerTiles) : nullptr;
				ClaimForBlockingTick(a_player);
				CheckFrameBoundary(a_player);
				// A lagging tick decodes into the graph's staging too, so the engine's tile textures are not mapped. It queues nothing.
				if (Eligible(a_player, tick.set)) {
					tick.lags = !Graph::Begin();
					StartGraphTick(tick);
				}
				auto* const outer = std::exchange(t_tick, &tick);
				const auto result = func(a_player, a_arg2, a_arg3, a_arg4);
				t_tick = outer;
				if (tick.movie)
					Graph::EndTick(*tick.movie);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Renderer::LockTexture (SE 75514, AE 77306). A graph tick's tile gets its place in the staging buffer instead. */
		struct LockTexture
		{
			static bool thunk(void* a_renderer, const void* a_block, std::uint32_t a_mapType, D3D11_MAPPED_SUBRESOURCE* a_mapped)
			{
				if (auto* tick = t_tick; tick && tick->movie && a_block && a_mapped) {
					if (const auto tile = tick->TileOf(a_block); tile != UINT32_MAX) {
						std::uint32_t rowPitch = 0;
						if (auto* data = Graph::Lock(*tick->movie, tile, rowPitch)) {
							a_mapped->pData = data;
							a_mapped->RowPitch = rowPitch;
							a_mapped->DepthPitch = 0;
							return true;
						}
						// Only an allocation failure leaves no staging buffer.
						RenderGraph::Fatal("Bink", "no staging buffer for a movie frame");
					}
				}
				return func(a_renderer, a_block, a_mapType, a_mapped);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Renderer::UnlockTexture (SE 75515, AE 77307). Nothing for a tile a graph tick locked. */
		struct UnlockTexture
		{
			static void thunk(void* a_renderer, const void* a_block)
			{
				if (auto* tick = t_tick; tick && tick->movie && tick->TileOf(a_block) != UINT32_MAX)
					return;
				func(a_renderer, a_block);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSMovie::DrawTiles (SE 87907, AE 90279). Withheld on a graph tick, whose frame goes to the Present anchor. */
		struct DrawTiles
		{
			static void thunk(const void* a_set)
			{
				auto* tick = t_tick;
				bool graph = false;
				if (tick && tick->movie && !tick->lags && a_set == tick->set) {
					// A graph tick does not call through HDR Display's detour of this function, so its preparation is done here.
					// The tiles go where the interface draws (Engine::UiTargets): with HDR on, the UI texture the composite reads.
					if (auto& hdr = globals::features::hdrDisplay; hdr.loaded)
						hdr.PrepareMovieDraw();
					graph = Graph::Queue(tick->movie);
				}
				RequestShot(tick ? tick->player : 0, graph);
				if (graph) {
					GraphDrew(a_set);
					return;
				}
				// Graph mode: the tick lagged or was a tap error where it left the graph, so nothing draws its tiles.
				if (Engine::GraphMode()) {
					if (!tick)
						RefuseBinkTick("a tile draw outside a movie tick");
					else if (tick->movie && !tick->lags) {
						static Util::ReasonLog s_nothing("[ORG] Bink: a tick drew nothing");
						s_nothing.Report("no decoded frame, or HDR's UI texture is not in the registration yet");
					}
					return;
				}
				// CS_ORG_BINK_SHOT outside graph mode: the engine draws.
				func(a_set);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSMovie::CreateTileTextures (SE 87904, AE 90275), which makes the tile set's DYNAMIC textures and quads. */
		struct CreateTileTextures
		{
			static void* thunk(std::uint32_t a_width, std::uint32_t a_height, std::int32_t a_bytesPerPixel, std::uint64_t a_tileSize, float a_scale, float a_x, float a_y)
			{
				void* const set = func(a_width, a_height, a_bytesPerPixel, a_tileSize, a_scale, a_x, a_y);
				if (!set || !Engine::GraphMode())
					return set;
				const auto [screenWidth, screenHeight] = RE::BSGraphics::Renderer::GetScreenSize();
				Record record;
				record.tileSize = Read<std::uint32_t>(set, kSetTileSize);
				record.layout = MakeLayout(a_width, a_height, Read<std::uint32_t>(set, kSetTileSize), Read<std::uint32_t>(set, kSetColumns), Read<std::uint32_t>(set, kSetRows),
					Read<std::uint32_t>(set, kSetEdgeWidth), Read<std::uint32_t>(set, kSetEdgeHeight), a_scale, a_x, a_y, screenWidth, screenHeight);
				const auto count = Read<std::uint32_t>(set, kSetTileCount);
				const auto* textures = Read<void* const*>(set, kSetTextures);
				if (a_bytesPerPixel != 4 || !textures || count != record.layout.tiles.size()) {
					logger::warn("[ORG] Bink: a {}x{} movie's tiles are not the layout the port expects ({} tiles, {} computed); its ticks are tap errors", a_width,
						a_height, count, record.layout.tiles.size());
					return set;
				}
				record.blocks.assign(textures, textures + count);
				auto& records = GetRecords();
				std::scoped_lock lock(records.mutex);
				records.sets[set] = std::move(record);
				return set;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Drops a tile set's record before it is freed. */
		void Forget(const void* a_set)
		{
			if (!a_set)
				return;
			bool graphMovie = false;
			{
				auto& records = GetRecords();
				std::scoped_lock lock(records.mutex);
				const auto found = records.sets.find(a_set);
				if (found == records.sets.end())
					return;
				const auto& record = found->second;
				graphMovie = record.movie != nullptr;
				if (graphMovie || record.graphFrames)
					logger::info("[ORG] Bink: a {}x{} movie ended: {} frames drawn by the render graph", record.layout.videoWidth, record.layout.videoHeight,
						record.graphFrames);
				records.sets.erase(found);
			}
			// Frames in flight keep the graph's textures until their GPU work retires.
			if (graphMovie && FrameAnchors::IsRenderOwner())
				Graph::MovieEnded();
		}

		/** @brief BSMovie::FreeTiles (SE 87905, AE 90276). SE's play loop calls it directly, while on AE only Close frees tiles, with this inlined. */
		struct FreeTiles
		{
			static void thunk(void* a_set)
			{
				Forget(a_set);
				func(a_set);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSMoviePlayer::Close (SE 87891, AE 90260), which frees tiles with FreeTiles inlined on AE. */
		struct ClosePlayer
		{
			static void thunk(std::uintptr_t a_player)
			{
				if (a_player)
					Forget(Read<const void*>(reinterpret_cast<const void*>(a_player), kPlayerTiles));
				func(a_player);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
		struct PendingPlay
		{
			std::mutex mutex;
			std::vector<std::string> files;
			std::atomic<bool> any{ false };
		};

		PendingPlay& Pending()
		{
			static PendingPlay s_pending;
			return s_pending;
		}

		/** @brief MoviePlayer::PlayMovie (SE 87918, AE 90291) with Console::PlayBink's defaults. */
		void Play(const std::string& a_file)
		{
			using PlayMovie = bool(void* a_player, const char* a_file, bool, bool, bool, bool, bool, bool, bool, bool);
			static REL::Relocation<void**> player{ REL::RelocationID(525733, 412214) };
			static REL::Relocation<PlayMovie*> play{ REL::RelocationID(87918, 90291) };
			if (!*player) {
				logger::warn("[ORG] Bink: no movie player to play {}", a_file);
				return;
			}
			logger::info("[ORG] Bink: playing {} on thread {} (render thread: {})", a_file, ::GetCurrentThreadId(), FrameAnchors::IsRenderOwner());
			const bool played = play(*player, a_file.c_str(), false, false, false, false, true, false, true, true);
			logger::info("[ORG] Bink: {} {}", a_file, played ? "played" : "did not play to its end");
		}

		/** @brief Plays the files the test harness asked for. Main thread. */
		void PlayRequested()
		{
			auto& pending = Pending();
			if (!pending.any.load(std::memory_order_acquire))
				return;
			std::vector<std::string> files;
			{
				std::scoped_lock lock(pending.mutex);
				files.swap(pending.files);
				pending.any.store(false, std::memory_order_release);
			}
			for (const auto& file : files)
				Play(file);
		}

		bool HarnessRuns()
		{
			// Only a cs-test run takes the bink command.
			static const bool runs = ::Testing::Configured();
			return runs;
		}

		std::atomic<bool> s_playTick{ false };
	}

	void RequestPlay(std::string a_file)
	{
		if (!s_playTick.load(std::memory_order_acquire)) {
			logger::warn("[ORG] Bink: no main-thread tick for the test harness's movies; {} is not played", a_file);
			return;
		}
		auto& pending = Pending();
		std::scoped_lock lock(pending.mutex);
		pending.files.push_back(std::move(a_file));
		pending.any.store(true, std::memory_order_release);
	}

	bool TickHooked()
	{
		return tickHooked;
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR())
			return;
		if (HarnessRuns())
			s_playTick.store(MainTick::Add("Bink::RequestPlay", &PlayRequested), std::memory_order_release);
		const bool shots = !ShotFrames().empty();
		if (!Engine::GraphMode() && !shots)
			return;
		logger::info("Hooking the Bink movie player ({})", Engine::GraphMode() ? "the graph mode" : "CS_ORG_BINK_SHOT");
		stl::detour_thunk<PlayerTick>(REL::RelocationID(87890, 90259));
		tickHooked = true;
		stl::detour_thunk<LockTexture>(REL::RelocationID(75514, 77306));
		stl::detour_thunk<UnlockTexture>(REL::RelocationID(75515, 77307));
		stl::detour_thunk<DrawTiles>(REL::RelocationID(87907, 90279));
		stl::detour_thunk<CreateTileTextures>(REL::RelocationID(87904, 90275));
		stl::detour_thunk<FreeTiles>(REL::RelocationID(87905, 90276));
		stl::detour_thunk<ClosePlayer>(REL::RelocationID(87891, 90260));
	}
}
