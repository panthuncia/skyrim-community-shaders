#include "RenderGraph/Engine/Bink/Bink.h"

#include "Features/HDRDisplay.h"
#include "Features/ScreenshotFeature.h"
#include "Globals.h"
#include "RenderGraph/Engine/Bink/BinkGraph.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Switches.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// The movie player's hooks (docs/development/p5/bink.md has the RE, SE 1.5.97 and AE 1.6.1170). Every hook calls the engine's
// function unless this tick of the player decodes and draws on the render graph (BinkGraph.h).
namespace RenderGraph::Engine::Bink
{
	namespace
	{
		// BSMoviePlayer (0x30 bytes): the BINK handle, the tile set, the decoded frame count, the scale and offsets, the mode.
		constexpr std::size_t kPlayerTiles = 0x08;
		constexpr std::size_t kPlayerFrames = 0x10;
		constexpr std::size_t kPlayerMode = 0x24;
		// Mode 1: the blocking play loop (BSMoviePlayer::PlayBlocking), whose ticks present themselves. Mode 2 draws inside another
		// frame (MoviePlayer::TickInFrame) and does not present.
		constexpr std::uint32_t kModeBlocking = 1;

		// The tile set BSMovie::CreateTileTextures returns (0x70 bytes, then the texture pointers).
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

		/** @brief A tile set the engine made: its layout, its tile textures (to know the locks), and its graph side once made. */
		struct Record
		{
			Layout layout;
			std::vector<const void*> blocks;
			std::shared_ptr<Graph::Movie> movie;
			bool movieFailed = false;
			std::uint64_t graphFrames = 0;
			std::uint64_t engineFrames = 0;
		};

		struct Records
		{
			std::mutex mutex;  // tile sets are made and freed on the thread that plays them (the logo's is not the main thread)
			std::unordered_map<const void*, Record> sets;
		};

		Records& GetRecords()
		{
			static Records s_records;
			return s_records;
		}

		/** @brief One tick of the player on this thread: whether it decodes and draws on the graph, and the tiles it locks. */
		struct Tick
		{
			std::uintptr_t player = 0;
			const void* set = nullptr;
			std::shared_ptr<Graph::Movie> movie;  // null: the engine's path this tick
			std::vector<const void*> blocks;
			bool locked = false;

			std::uint32_t TileOf(const void* a_block) const
			{
				const auto found = std::find(blocks.begin(), blocks.end(), a_block);
				return found == blocks.end() ? UINT32_MAX : static_cast<std::uint32_t>(found - blocks.begin());
			}
		};

		thread_local Tick* t_tick = nullptr;

		/** @brief CS_ORG_BINK_SHOT=<n>[,<n>...]: decoded frames whose Present is saved as dclf-shots/bink-<n>.png. */
		const std::set<std::uint32_t>& ShotFrames()
		{
			static const std::set<std::uint32_t> frames = [] {
				std::set<std::uint32_t> result;
				const std::string value = Switches::Value("CS_ORG_BINK_SHOT");
				std::size_t start = 0;
				while (start < value.size()) {
					const auto end = std::min(value.find(',', start), value.size());
					const auto number = value.substr(start, end - start);
					if (!number.empty())
						result.insert(static_cast<std::uint32_t>(std::strtoul(number.c_str(), nullptr, 10)));
					start = end + 1;
				}
				return result;
			}();
			return frames;
		}

		void RequestShot(std::uintptr_t a_player, bool a_graph)
		{
			const auto& frames = ShotFrames();
			if (frames.empty() || !a_player || !FrameAnchors::OnMainThread())
				return;
			static std::set<std::uint32_t> taken;
			const auto frame = Read<std::uint32_t>(reinterpret_cast<const void*>(a_player), kPlayerFrames);
			if (!frames.contains(frame) || !taken.insert(frame).second)
				return;
			const auto dir = logger::log_directory().value_or(std::filesystem::path{}) / "dclf-shots";
			logger::info("[ORG] Bink: capturing decoded frame {} ({}) as bink-{}.png", frame, a_graph ? "render graph" : "engine", frame);
			globals::features::screenshotFeature.RequestCapture(dir / fmt::format("bink-{}.png", frame));
		}

		/** @brief Whether this tick may draw on the graph, before Graph::Begin: the engine's path in every case the port does not cover. */
		bool Eligible(std::uintptr_t a_player, const void* a_set)
		{
			if (!Requested() || !a_player || !a_set)
				return false;
			if (Read<std::uint32_t>(reinterpret_cast<const void*>(a_player), kPlayerMode) != kModeBlocking)
				return false;
			if (!FrameAnchors::OnMainThread()) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::info("[ORG] Bink: a movie plays on thread {}, not the main thread (the startup sequence): the engine draws it", ::GetCurrentThreadId());
				return false;
			}
			// HDR output routes the movie into its UI texture (HDRDisplay's DrawTiles hook); the graph's pass draws the framebuffer.
			if (globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.IsHDREnabledForFrame())
				return false;
			return true;
		}

		/** @brief The graph side of a tile set, made the first time a tick of it draws on the graph. Main thread. */
		bool StartGraphTick(Tick& a_tick)
		{
			auto& records = GetRecords();
			std::scoped_lock lock(records.mutex);
			const auto found = records.sets.find(a_tick.set);
			if (found == records.sets.end())
				return false;
			auto& record = found->second;
			if (!record.movie && !record.movieFailed) {
				record.movie = Graph::CreateMovie(record.layout);
				record.movieFailed = !record.movie;
			}
			if (!record.movie)
				return false;
			a_tick.movie = record.movie;
			a_tick.blocks = record.blocks;
			return true;
		}

		/** @brief After the engine drew a tick: the graph's textures are older than the engine's now, and the count. */
		void EngineDrew(const void* a_set)
		{
			auto& records = GetRecords();
			std::scoped_lock lock(records.mutex);
			const auto found = records.sets.find(a_set);
			if (found == records.sets.end())
				return;
			++found->second.engineFrames;
			if (found->second.movie)
				Graph::MarkStale(*found->second.movie);
		}

		void GraphDrew(const void* a_set)
		{
			auto& records = GetRecords();
			std::scoped_lock lock(records.mutex);
			if (const auto found = records.sets.find(a_set); found != records.sets.end())
				++found->second.graphFrames;
		}

		/**
		 * @brief BSMoviePlayer::Tick (SE 87890, AE 90259): waits for the next frame, decodes it into the tiles (SE: DecodeFrameToTiles
		 * 87911; inlined on AE), clears kFRAMEBUFFER, draws the tiles (DrawTiles) and presents (Renderer::End) unless in mode 2.
		 */
		struct PlayerTick
		{
			static std::uint64_t thunk(std::uintptr_t a_player, std::uintptr_t a_arg2, std::uintptr_t a_arg3, std::uintptr_t a_arg4)
			{
				Tick tick;
				tick.player = a_player;
				tick.set = a_player ? Read<const void*>(reinterpret_cast<const void*>(a_player), kPlayerTiles) : nullptr;
				if (Eligible(a_player, tick.set) && Graph::Begin())
					StartGraphTick(tick);
				auto* const outer = std::exchange(t_tick, &tick);
				const auto result = func(a_player, a_arg2, a_arg3, a_arg4);
				t_tick = outer;
				if (tick.movie)
					Graph::EndTick(*tick.movie);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSGraphics::Renderer::LockTexture (SE 75514, AE 77306): Map of the block's texture on the immediate context. For a
		 * tile of a graph tick, the tile's place in the tick's staging buffer instead.
		 */
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
							tick->locked = true;
							return true;
						}
						// No staging buffer to take (the first lock of the tick takes it): the engine decodes and draws this tick.
						if (!tick->locked) {
							Graph::MarkStale(*tick->movie);
							tick->movie.reset();
						}
					}
				}
				return func(a_renderer, a_block, a_mapType, a_mapped);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSGraphics::Renderer::UnlockTexture (SE 75515, AE 77307): Unmap; nothing for a tile a graph tick locked. */
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

		/**
		 * @brief BSMovie::DrawTiles (SE 87907, AE 90279): the copy shaders, then per tile its texture at PS t0 (clamp, linear) and
		 * its quad (DrawTriShape), with blending and depth off. Withheld on a graph tick: the frame is queued for the Present anchor.
		 */
		struct DrawTiles
		{
			static void thunk(const void* a_set)
			{
				auto* tick = t_tick;
				const bool graph = tick && tick->movie && a_set == tick->set && Graph::Queue(tick->movie);
				RequestShot(tick ? tick->player : 0, graph);
				if (graph) {
					GraphDrew(a_set);
					return;
				}
				EngineDrew(a_set);
				func(a_set);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSMovie::CreateTileTextures (SE 87904, AE 90275): the tile set, its DYNAMIC R8G8B8A8 textures and quads, for a movie
		 * of a_width x a_height in tiles of a_tileSize, scaled by a_scale and placed at a_x, a_y (pixels).
		 */
		struct CreateTileTextures
		{
			static void* thunk(std::uint32_t a_width, std::uint32_t a_height, std::int32_t a_bytesPerPixel, std::uint64_t a_tileSize, float a_scale, float a_x, float a_y)
			{
				void* const set = func(a_width, a_height, a_bytesPerPixel, a_tileSize, a_scale, a_x, a_y);
				if (!set || !Requested())
					return set;
				const auto [screenWidth, screenHeight] = RE::BSGraphics::Renderer::GetScreenSize();
				Record record;
				record.layout = MakeLayout(a_width, a_height, Read<std::uint32_t>(set, kSetTileSize), Read<std::uint32_t>(set, kSetColumns), Read<std::uint32_t>(set, kSetRows),
					Read<std::uint32_t>(set, kSetEdgeWidth), Read<std::uint32_t>(set, kSetEdgeHeight), a_scale, a_x, a_y, screenWidth, screenHeight);
				const auto count = Read<std::uint32_t>(set, kSetTileCount);
				const auto* textures = Read<void* const*>(set, kSetTextures);
				if (a_bytesPerPixel != 4 || !textures || count != record.layout.tiles.size()) {
					logger::warn("[ORG] Bink: a {}x{} movie's tiles are not the layout the port expects ({} tiles, {} computed); the engine draws it", a_width, a_height,
						count, record.layout.tiles.size());
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

		/** @brief A tile set is about to be freed: its record goes, and the client leaves the Present anchor after a graph movie. */
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
					logger::info("[ORG] Bink: a {}x{} movie ended: {} frames drawn by the render graph, {} by the engine", record.layout.videoWidth,
						record.layout.videoHeight, record.graphFrames, record.engineFrames);
				records.sets.erase(found);
			}
			// The frames in flight keep the graph's textures until their GPU work retires.
			if (graphMovie && FrameAnchors::OnMainThread())
				Graph::MovieEnded();
		}

		/**
		 * @brief BSMovie::FreeTiles (SE 87905, AE 90276): releases the tile textures and quads and frees the set. SE's play loop
		 * calls it directly; on AE only Close frees tiles, with this inlined.
		 */
		struct FreeTiles
		{
			static void thunk(void* a_set)
			{
				Forget(a_set);
				func(a_set);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSMoviePlayer::Close (SE 87891, AE 90260): BinkClose, then the player's tiles freed (FreeTiles, inlined on AE). */
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

		/** @brief MoviePlayer::PlayMovie (SE 87918, AE 90291) as Console::PlayBink calls it, with its defaults. */
		void Play(const std::string& a_file)
		{
			using PlayMovie = bool(void* a_player, const char* a_file, bool, bool, bool, bool, bool, bool, bool, bool);
			static REL::Relocation<void**> player{ REL::RelocationID(525733, 412214) };
			static REL::Relocation<PlayMovie*> play{ REL::RelocationID(87918, 90291) };
			if (!*player) {
				logger::warn("[ORG] Bink: no movie player to play {}", a_file);
				return;
			}
			logger::info("[ORG] Bink: playing {} on thread {} (main thread: {})", a_file, ::GetCurrentThreadId(), FrameAnchors::OnMainThread());
			const bool played = play(*player, a_file.c_str(), false, false, false, false, true, false, true, true);
			logger::info("[ORG] Bink: {} {}", a_file, played ? "played" : "did not play to its end");
		}

		/** @brief JobListManager's frame-job wait (SE 34554, AE 35409), called only by Main::Update, on the main thread. */
		struct WaitForFrameJobs
		{
			static void thunk()
			{
				func();
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
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void RequestPlay(std::string a_file)
	{
		if (!WaitForFrameJobs::func.address()) {
			logger::warn("[ORG] Bink: the frame-job wait is not hooked; {} is not played", a_file);
			return;
		}
		auto& pending = Pending();
		std::scoped_lock lock(pending.mutex);
		pending.files.push_back(std::move(a_file));
		pending.any.store(true, std::memory_order_release);
	}

	bool Requested()
	{
		static const bool requested = !REL::Module::IsVR() && Switches::On("CS_ORG_BINK");
		return requested;
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR())
			return;
		// RequestPlay's main-thread point (the test harness's dclf:bink); one atomic load a frame when nothing is asked.
		stl::detour_thunk<WaitForFrameJobs>(REL::RelocationID(34554, 35409));
		const bool shots = !ShotFrames().empty();
		if (!Requested() && !shots)
			return;
		if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Present))
			logger::warn("[ORG] Bink: CS_ORG_BINK=1, but the Present anchor could not be reserved; the engine draws Bink");
		logger::info("Hooking the Bink movie player ({})", Requested() ? "CS_ORG_BINK" : "CS_ORG_BINK_SHOT");
		stl::detour_thunk<PlayerTick>(REL::RelocationID(87890, 90259));
		stl::detour_thunk<LockTexture>(REL::RelocationID(75514, 77306));
		stl::detour_thunk<UnlockTexture>(REL::RelocationID(75515, 77307));
		stl::detour_thunk<DrawTiles>(REL::RelocationID(87907, 90279));
		stl::detour_thunk<CreateTileTextures>(REL::RelocationID(87904, 90275));
		stl::detour_thunk<FreeTiles>(REL::RelocationID(87905, 90276));
		stl::detour_thunk<ClosePlayer>(REL::RelocationID(87891, 90260));
	}
}
