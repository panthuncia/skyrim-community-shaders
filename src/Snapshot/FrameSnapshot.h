#pragma once

#include "Snapshot/Retire.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace DCLF
{
	struct ScenePublication;
}

/**
* @brief The frame snapshot contract (render-thread roadmap, phase 0). Notes: docs/development/plans/snap0-contract.md.
*
* A FrameSnapshot is everything a produced frame reads of the game, copied out of the game on the game thread. It is
* immutable once published and handed around as std::shared_ptr<const FrameSnapshot>. It holds no pointer into live engine
* state the game may change.
*
* Life of a snapshot (Snapshot/Publication.h):
* - Built: Snapshot::Capture runs the builder once per game update, at Main::Update's call of Main::DrawFrame (the boundary
*   between the game update and its render half). It fills the core fields from game data and asks every registered feature
*   capture for its block. A render frame no game update stands for (a loading screen's on the serving thread, a Bink movie's,
*   a main-thread frame outside Main::Update) gets a frame-only snapshot from Snapshot::CaptureFrame at its Begin.
* - Published: latest wins. A published snapshot the render side never adopted is replaced by the next one, and its events and
*   counters are merged into that newer one first, so no event is lost.
* - Adopted: at the render frame's start (Renderer::Begin, before the frame driver's OnBegin), on the thread that renders it.
*   Recording code reads only Snapshot::Current(). A render frame that finds nothing new keeps the snapshot it had: a repeat.
*   The render side's frame kind is the adopted snapshot's kind.
*
* - Retired: the render side never lets a snapshot go itself. It hands the one it rendered last to the retire queue
*   (Snapshot/Retire.h), which lets it go on the main thread once the GPU finished that frame. Producers let go only on the main
*   thread too. So engine references a block holds (Snapshot::EngineRef) are always released where the engine allows it.
*
* Game identity (id, gameTime, gameDelta) is the snapshot's. Render identity (render frame index, render delta) is the render
* side's (Snapshot::Render()). Anything that means "a new game frame" keys by the snapshot id, never by a render counter.
*/
namespace Snapshot
{
	/**
	* @brief What kind of frame the game asked for, decided by the snapshot's producer. The render side's frame kind
	* (RenderGraph::Frame::Kind) is the adopted snapshot's.
	*/
	enum class FrameKind : std::uint8_t
	{
		World,          // main thread: no menu freezes the background and the player has 3D (also under a loading menu held open)
		MenuOverWorld,  // main thread: a menu freezes the background (MenuManager's kFreezeFrameBackground test)
		MainMenu,       // main thread: no menu freezes the background and the player has no 3D (the main menu, startup)
		Loading,        // a loading screen's frame, on the serving thread
		Movie,          // a Bink movie frame (BSMoviePlayer::Tick), on any thread
		Count
	};

	/** @brief "world", "menu over world", ... */
	const char* FrameKindName(FrameKind a_kind);

	/** @brief One bit per event that happened since the previous snapshot. */
	enum class Event : std::uint32_t
	{
		None = 0,
		CameraCut = 1u << 0,        // the main camera jumped (position or direction): histories reset
		LoadStarted = 1u << 1,      // a loading screen began
		FirstAfterLoad = 1u << 2,   // the first snapshot captured after a loading screen ended
		Teleport = 1u << 3,         // the player moved further than a step, or changed cell, without a loading screen
		MenuOpened = 1u << 4,       // a menu opened
		MenuClosed = 1u << 5,       // a menu closed
		KindChanged = 1u << 6,      // the frame kind differs from the previous snapshot's
		Paused = 1u << 7,           // the game paused
		Resumed = 1u << 8,          // the game resumed
		SettingsChanged = 1u << 9,  // the settings revision moved
		WindowResized = 1u << 10,   // the window proc saw a new client size (Events::windowSize)
	};

	constexpr Event operator|(Event a, Event b) { return static_cast<Event>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)); }

	/**
	* @brief The events since the previous snapshot. A snapshot replaced before adoption hands its events on to the next one
	* (MergeOlder): flags OR, counters add. So a render frame sees every event of the game updates it skipped.
	*/
	/**
	* @brief The newest client size the window proc asked the renderer for (WM_SIZE, WM_WINDOWPOSCHANGED). A state, not a delta:
	* every snapshot carries the newest request, and serial moves with each one, so a render frame acts once on a new serial.
	*/
	struct WindowSize
	{
		std::uint64_t serial = 0;  // 0 before the first request
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	struct Events
	{
		std::uint32_t flags = 0;
		std::uint32_t gameUpdates = 1;  // the game updates this snapshot stands for: 1 (0 for a frame-only one), plus every replaced snapshot folded in
		std::uint32_t loadsStarted = 0;
		std::uint32_t menusOpened = 0;
		std::uint32_t menusClosed = 0;
		std::uint32_t cameraCuts = 0;
		std::uint32_t teleports = 0;
		float gameDelta = 0.0f;  // the game time these updates advanced, summed over folded snapshots
		WindowSize windowSize{};  // the newest resize request at the capture; the newer serial wins when folded

		bool Has(Event a_event) const { return (flags & static_cast<std::uint32_t>(a_event)) != 0; }
		void Add(Event a_event) { flags |= static_cast<std::uint32_t>(a_event); }
		void MergeOlder(const Events& a_older)
		{
			flags |= a_older.flags;
			gameUpdates += a_older.gameUpdates;
			loadsStarted += a_older.loadsStarted;
			menusOpened += a_older.menusOpened;
			menusClosed += a_older.menusClosed;
			cameraCuts += a_older.cameraCuts;
			teleports += a_older.teleports;
			gameDelta += a_older.gameDelta;
			if (a_older.windowSize.serial > windowSize.serial)
				windowSize = a_older.windowSize;
		}
	};

	/** @brief The menu states the frame reads. Captured from RE::UI once per game update. */
	struct MenuStates
	{
		bool main = false;
		bool loading = false;
		bool map = false;
		bool stats = false;
		bool console = false;
		bool freezesBackground = false;  // MenuManager's own test (kFreezeFrameBackground), as Main::DrawFrame asks it
		bool paused = false;             // UI::GameIsPaused
		std::uint32_t open = 0;          // menus on the stack

		bool operator==(const MenuStates&) const = default;
	};

	/** @brief The views a snapshot carries cameras for. Phase 0 fills Main. Cube faces and shadow views get rows when ported. */
	enum class ViewId : std::uint8_t
	{
		Main,
		Count
	};

	/** @brief One view's camera as the game set it: world-to-camera, placement, frustum and viewport. */
	struct CameraView
	{
		bool valid = false;
		float worldToCam[4][4]{};
		RE::NiPoint3 position{};
		RE::NiMatrix3 rotation{};
		RE::NiFrustum frustum{};
		RE::NiRect<float> port{};
		// BSGraphics::State::SetCameraData's matrices from this camera, row-major and unjittered (Snapshot/FrameCamera.h).
		std::array<float, 16> view{}, proj{}, viewProj{};
	};

	/** @brief The cameras by view. previous is the previous published snapshot's current (not the last rendered one). */
	struct Cameras
	{
		std::array<CameraView, static_cast<std::size_t>(ViewId::Count)> current{};
		std::array<CameraView, static_cast<std::size_t>(ViewId::Count)> previous{};

		const CameraView& Current(ViewId a_view) const { return current[static_cast<std::size_t>(a_view)]; }
		const CameraView& Previous(ViewId a_view) const { return previous[static_cast<std::size_t>(a_view)]; }
	};

	/**
	* @brief The per-feature block ids, one line per block. A block is a struct with `static constexpr BlockId kId` and an
	* operator==, copied from game data by its feature's capture (Snapshot::RegisterCapture). Blocks are values: no engine
	* pointers, nothing the game may change.
	*/
	enum class BlockId : std::uint16_t
	{
		TreeWind,   // Features/TreeWind.h, TreeWind::Block
		CoreFrame,  // Snapshot/CoreFrame.h, the shared frame state's game data
		// Sky, atmosphere and lighting (snap1-sky)
		SkySync,               // Features/SkySync.h, SkySync::Block
		PhysicalSky,           // Features/PhysicalSky.h, PhysicalSky::Block
		ProceduralSun,         // Features/ProceduralSun.h, ProceduralSun::Block
		ProceduralAurora,      // Features/ProceduralAurora.h, ProceduralAurora::Block
		CloudShadows,          // Features/CloudShadows.h, CloudShadows::Block
		Skylighting,           // Features/Skylighting.h, Skylighting::Block
		TerrainShadows,        // Features/TerrainShadows.h, TerrainShadows::Block
		InteriorSun,           // Features/InteriorSun.h, InteriorSun::Block
		VolumetricLighting,    // Features/VolumetricLighting.h, VolumetricLighting::Block
		WetnessEffects,        // Features/WetnessEffects.h, WetnessEffects::Block
		ExponentialHeightFog,  // Features/ExponentialHeightFog.h, ExponentialHeightFog::Block
		// The post group: post-processing, upscaling and the other non-DCLF features.
		GrassCollision,      // Features/GrassCollision.h, GrassCollision::Block
		UnifiedWater,        // Features/UnifiedWater.h, UnifiedWater::Block
		DynamicCubemaps,     // Features/DynamicCubemaps.h, DynamicCubemaps::Block
		ScreenSpaceShadows,  // Features/ScreenSpaceShadows.h, ScreenSpaceShadows::Block
		PostProcessing,      // Features/PostProcessing.h, PostProcessing::Block
		Effects11,           // Features/Effects11.h, Effects11::Block
		ImageSpace,          // RenderGraph/Engine/ImageSpace/ImageSpaceBlock.h, RenderGraph::Engine::ImageSpace::Block
		// The scene (snap3-world)
		ScreenBlood,  // Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h, DCLF::Features::BloodSplatter::Block
		SunQuery,     // Features/DrawcallLimitFix/Features/Sky/Sky.h, DCLF::Features::Sky::SunQueryBlock
		RenderWorld,  // RenderGraph/Engine/Native/WorldPass.h, RenderGraph::Engine::Native::WorldPass::Block
		FaceTint,     // Snapshot/FaceTint.h, Snapshot::FaceTint::Block
		// The post (snap3-post)
		VolumetricLightingRender,  // Features/VolumetricLighting/ORGVolumetricLighting.h, ORGVolumetricLighting::Block
		Count
	};

	inline constexpr std::size_t kBlocks = static_cast<std::size_t>(BlockId::Count);

	template <class T>
	concept FeatureBlock = std::is_same_v<std::remove_cv_t<decltype(T::kId)>, BlockId> && std::is_copy_constructible_v<T> &&
	                       requires(const T& a, const T& b) {
							   { a == b } -> std::convertible_to<bool>;
						   };

	/** @brief The UI command stream (phase 4's native UI renderer, Snapshot/UiCommandStream.h). Native only. */
	struct UiCommandStream;

	/**
	* @brief One render frame's game data. Immutable once published.
	*
	* Ids: a game update's snapshot takes the next id (1, 2, ...) with sub 0. A frame-only snapshot (Snapshot::CaptureFrame) keeps
	* the last game update's id and takes the next sub (1, 2, ...), so (id, sub) orders every snapshot and "a new game frame" is
	* still a new id. It copies the last snapshot's world data (blocks, cameras, scene, game time, settings revision) and has its
	* own kind, menus, load epoch, events (gameUpdates 0, game delta 0) and UI stream.
	*/
	struct FrameSnapshot
	{
		std::uint64_t id = 0;         // 1 for the first game update's snapshot, then one more per game update (0 before the first)
		std::uint32_t sub = 0;        // 0 for a game update's snapshot; 1, 2, ... for the frame-only snapshots published since it
		double gameTime = 0.0;        // game seconds since the session started, not advancing while paused
		float gameDelta = 0.0f;       // this update's game seconds (RE::GetSecondsSinceLastFrame); 0 while paused
		float realDelta = 0.0f;       // this update's wall-clock seconds (BSTimer::realTimeDelta)
		float gameDaysPassed = 0.0f;  // the calendar's days passed (time of day in the fraction), 0 without a calendar
		FrameKind kind = FrameKind::MainMenu;
		std::uint64_t loadEpoch = 0;  // loading screens begun this session
		MenuStates menus{};
		std::uint64_t settingsRevision = 0;  // CS::SettingRegistry::Serial() at the capture: moves with every settings change
		Cameras cameras{};
		bool interior = false;                                // the player's cell is an interior
		bool worldLoaded = false;                             // the player has 3D and a parent cell: a frozen menu can draw the world behind it
		bool firstPersonShown = false;                        // the player's first-person root is not hidden at the capture's start (Main::RenderPlayerView's test)
		std::shared_ptr<const DCLF::ScenePublication> scene;  // the scene publication it pairs with (empty in phase 0)
		std::shared_ptr<const UiCommandStream> ui;            // the UI command stream (native; set at the update's DrawInterface end)
		Events events{};
		Retire::LiveCount live;  // counts snapshots alive for the leak check (Snapshot/Retire.h)

		/** @brief a_T's block, or null when its feature captured none. Shared by pointer with the snapshots it did not change in. */
		template <FeatureBlock T>
		const T* Block() const
		{
			return static_cast<const T*>(blocks[static_cast<std::size_t>(T::kId)].get());
		}

		/** @brief The block by id, untyped, for stats and pointer comparisons. */
		const std::shared_ptr<const void>& BlockAt(BlockId a_id) const { return blocks[static_cast<std::size_t>(a_id)]; }

	private:
		friend class Builder;
		std::array<std::shared_ptr<const void>, kBlocks> blocks{};
	};

	/**
	* @brief Fills one FrameSnapshot on the game thread, inside the capture scope. Features add their blocks with Set; an equal
	* block keeps the previous snapshot's pointer, so an unchanged feature costs one comparison and no allocation.
	*/
	class Builder
	{
	public:
		Builder(FrameSnapshot& a_snapshot, const FrameSnapshot* a_previous) :
			snapshot(a_snapshot), previous(a_previous) {}

		/** @brief The snapshot being built (core fields already filled when feature captures run). */
		const FrameSnapshot& Built() const { return snapshot; }

		/** @brief The previous published snapshot, or null for the first. */
		const FrameSnapshot* Previous() const { return previous; }

		/** @brief Sets a_T's block. Shares the previous snapshot's block when equal. */
		template <FeatureBlock T>
		void Set(const T& a_block)
		{
			constexpr auto index = static_cast<std::size_t>(T::kId);
			if (previous)
				if (const auto* before = previous->Block<T>(); before && *before == a_block) {
					snapshot.blocks[index] = previous->blocks[index];
					++shared;
					return;
				}
			snapshot.blocks[index] = std::make_shared<const T>(a_block);
			++made;
		}

		std::uint32_t shared = 0;  // blocks kept by pointer
		std::uint32_t made = 0;    // blocks allocated

	private:
		FrameSnapshot& snapshot;
		const FrameSnapshot* previous = nullptr;
	};
}
