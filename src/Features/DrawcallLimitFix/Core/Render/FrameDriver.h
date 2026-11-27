#pragma once

// FrameDriver runs DCLF's render-thread work at each engine point of the frame, from the scene frame's start to Present.
// The feature's hooks call it. It reads the scene through Scene::FrameView and drives IndirectDraws.

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "RenderGraph/Frame/Revision.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace RE
{
	class BSRenderPass;
	class BSShadowLight;
}

namespace DCLF
{
	struct FrameTicket;

	/**
	 * @brief Registers DCLF's scene as a fragment source of the frame revision (cleanup plan A5, step 4), like any other client.
	 * Main thread, once, before the render graph initializes. The frame revision takes DCLF's newest snapshot at every main-thread
	 * render-owner Begin, adopts it alone or with the revision it was recorded against, and BeginFrame hands its publication over.
	 */
	void RegisterSceneSource();

	/** @brief Render thread. Whether the engine's utility shader existed at the frame's ticket. False before the first ticket. */
	bool TicketUtilityShader();

	/** @brief DCLF's scene source, from RegisterSceneSource. */
	RenderGraph::Frame::Revision::FragmentSourceId SceneSource();

	class FrameDriver
	{
	public:
		static FrameDriver& Get();

		/** @brief Render-thread time in DCLF's frame points since the last report. */
		struct Counters
		{
			struct Timing
			{
				double eventsMs = 0.0;  // Present, plus frame-start requests and captures
				double buildMs = 0.0;
				double buildMaxMs = 0.0;
				double sceneMs = 0.0;  // shadow view points only. Scene passes are timed by the coordinator's pump.
				double sceneMaxMs = 0.0;
				std::uint32_t frames = 0;
			} timing;
		};
		Counters& ReportCounters() { return counters; }

		/** @brief Occlusion map frame counts since the last take. */
		struct OcclusionFrames
		{
			// Per occlusion view: maps left to the engine, frames the engine culled the map's scene,
			// and frames DCLF drew it while some occluder was not yet a member.
			std::array<std::uint32_t, 2> native{};
			std::array<std::uint32_t, 2> engine{};
			std::array<std::uint32_t, 2> joining{};
			// Per occlusion view, now: members of the map's phase the frame's set lacks.
			std::array<std::uint32_t, 2> lacking{};
			std::uint32_t withheld = 0;  // last frame's
		};
		/** @brief Render thread. Returns and resets the occlusion map counts. */
		OcclusionFrames TakeOcclusionFrames();

		/**
		 * @brief Main::Draw's early point (SetupSunAndSky), every world render. Makes the game update's ticket (GameFrameStart), or keeps
		 * the snapshot's on a repeated render, then runs the render half (BeginFrame) from it.
		 */
		void SceneFrameStart();
		/** @brief Native, main thread: a game update the cadence renders no frame for. Its ticket retires unused when the next is made. */
		void UnrenderedUpdate(std::uint64_t a_snapshot);
		/** @brief Render owner, at Begin. Whether this render repeats the snapshot whose ticket is made, so it keeps the ticket's scene. */
		bool HoldsScene() const;
		/** @brief Inside the adoption point: a revision left DCLF's scene, so no coverage counts until a render decides it again. */
		void NoteStoppedDrawing();
		/** @brief Render thread. The ticket the frame renders, null before the first. */
		const FrameTicket* Ticket() const { return ticket.get(); }
		/** @brief The tickets made, those no render took, and those a repeated render made again, since the last take. */
		struct TicketCounts
		{
			std::uint64_t made = 0, unrendered = 0, remade = 0;
		};
		TicketCounts TakeTicketCounts() { return { std::exchange(ticketsMade, 0), std::exchange(ticketsUnrendered, 0), std::exchange(ticketsRemade, 0) }; }
		/** @brief Before Present, in menus too. Queues the CPU pipeline's frame signal and the frame values' signal before DXVK presents. */
		void BeforePresent();
		/** @brief After Present, in menus too. Ends the frame and writes its report. */
		void EndFrame();
		/**
		 * @brief Called around the engine's shadow-light selection while DCLF runs. Coverage is decided before the first light is kept,
		 * so every shadow view's registration follows this frame's coverage.
		 */
		void BeforeShadowCasters();
		void ShadowCasterKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots);
		/** @brief Render thread, before and after the engine's focus accumulation of a_light. */
		void ShadowFocusAccumulating(const RE::BSShadowLight* a_light, bool a_begin);
		void ShadowCastersSelected();
		void BeforeShadowMaps();
		void AfterShadowMaps();
		/** @brief Main::RenderDepth's world-drawn point. Runs the depth commit with the hosted view work inside it. */
		void DepthCommitted();
		void EarlyPrepass();
		void Prepass();
		/**
		 * @brief At the shadow maps, whether DCLF draws the map in this frame's shadow epoch.
		 * At the engine's occlusion point, whether the engine must render it whole.
		 */
		bool OcclusionReady(std::uint32_t a_map);
		/** @brief Whether the map is held, which means it renders without the engine's cull. */
		bool OcclusionHeld(std::uint32_t a_map) const;
		bool OcclusionNeedsEngine(std::uint32_t a_map);
		/** @brief Start of the main pass's opaque batches. Applies the engine's pending state, then captures the main view. */
		void BeforeOpaquePass();
		/** @brief End of the opaque batches. Runs the main view's colour epoch. */
		void AfterOpaquePass();
		/** @brief A native lighting draw in the main pass. */
		void NativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	private:
		/** @brief Makes the next ticket. The last one retires, unused when no render took it. */
		void MakeTicket(std::uint64_t a_snapshot);
		/** @brief The game half: everything the frame start reads or writes of the engine, once per game update. */
		std::shared_ptr<const FrameTicket> GameFrameStart(std::uint64_t a_snapshot);
		/** @brief The render half, once per render: reads only the ticket, the snapshot and CS's frame. */
		void BeginFrame();
		/** @brief Runs every view's work at a_point, in slot order. */
		void RunViewWork(Api::ViewPoint a_point);

		/** @brief Adds shadow view time for this frame. It is counted at BeforeShadowMaps. */
		void AddShadowViewsTime(std::chrono::steady_clock::time_point a_start);

		Counters counters;
		double shadowViewsMs = 0.0;
		OcclusionFrames occlusionFrames;  // native and engine only, the rest is read at the take
		std::uint32_t occlusionHeld = 0;
		std::uint32_t occlusionWanted = 0;  // until AfterShadowMaps
		std::uint32_t occlusionTaken = 0;   // the maps AfterShadowMaps asked the shadow epoch for, this frame
		std::uint32_t occlusionDrawn = 0;
		std::uint32_t occlusionEverDrawn = 0;  // an engine draw of one of these is a re-entry
		// Per map, frames in a row the engine would have drawn it whole. A map DCLF never drew counts as a re-entry only
		// for kOcclusionReentryFrames, so a map it cannot draw at all is not held forever.
		std::array<std::uint32_t, 32> occlusionNeedStreak{};
		static constexpr std::uint32_t kOcclusionReentryFrames = 300;
		std::uint32_t captureFrame = ~0u;
		std::shared_ptr<const FrameTicket> ticket;
		bool ticketRendered = false;  // a render took the ticket
		bool coveredLast = false;     // the last render's coverage of DCLF's claims epochs
		std::uint64_t ticketsMade = 0, ticketsUnrendered = 0, ticketsRemade = 0;
	};
}
