#pragma once

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
*   capture for its block.
* - Published: latest wins. A published snapshot the render side never adopted is replaced by the next one, and its events and
*   counters are merged into that newer one first, so no event is lost.
* - Adopted: at the render frame's start (Renderer::Begin, before the frame driver's OnBegin), on the main thread. Recording code
*   reads only Snapshot::Current(). A render frame that finds nothing new keeps the snapshot it had: a repeat.
*
* Game identity (id, gameTime, gameDelta) is the snapshot's. Render identity (render frame index, render delta) is the render
* side's (Snapshot::Render()). Anything that means "a new game frame" keys by the snapshot id, never by a render counter.
*/
namespace Snapshot
{
	/** @brief What kind of frame the game asked for. Decided from the UI and the player on the game thread. */
	enum class FrameKind : std::uint8_t
	{
		World,          // no menu freezes the background and the player has 3D
		MenuOverWorld,  // a menu that freezes the background is open over a loaded world
		MainMenu,       // the main menu, or any update before the player has 3D
		Loading,        // the loading menu is open
		Movie,          // a Bink movie frame (published by the movie's producer; phase 0 never captures one)
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
	};

	constexpr Event operator|(Event a, Event b) { return static_cast<Event>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)); }

	/**
	* @brief The events since the previous snapshot. A snapshot replaced before adoption hands its events on to the next one
	* (MergeOlder): flags OR, counters add. So a render frame sees every event of the game updates it skipped.
	*/
	struct Events
	{
		std::uint32_t flags = 0;
		std::uint32_t gameUpdates = 1;  // the game updates this snapshot stands for: 1, plus every replaced snapshot folded in
		std::uint32_t loadsStarted = 0;
		std::uint32_t menusOpened = 0;
		std::uint32_t menusClosed = 0;
		std::uint32_t cameraCuts = 0;
		std::uint32_t teleports = 0;
		float gameDelta = 0.0f;  // the game time these updates advanced, summed over folded snapshots

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
		Count
	};

	inline constexpr std::size_t kBlocks = static_cast<std::size_t>(BlockId::Count);

	template <class T>
	concept FeatureBlock = std::is_same_v<std::remove_cv_t<decltype(T::kId)>, BlockId> && std::is_copy_constructible_v<T> &&
	                       requires(const T& a, const T& b) {
							   { a == b } -> std::convertible_to<bool>;
						   };

	/** @brief The UI command stream (phase 4's native UI renderer). Empty in phase 0. */
	struct UiCommandStream;

	/** @brief One game update's frame data. Immutable once published. */
	struct FrameSnapshot
	{
		std::uint64_t id = 0;         // 1 for the first snapshot, then one more per game update
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
		std::shared_ptr<const DCLF::ScenePublication> scene;  // the scene publication it pairs with (empty in phase 0)
		std::shared_ptr<const UiCommandStream> ui;            // the UI command stream (empty in phase 0)
		Events events{};

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
