#pragma once

// FrameDriver runs DCLF's render-thread work at each engine point of the frame, from the scene frame's start to Present.
// The feature's hooks call it. It reads the scene through Scene::FrameView and drives IndirectDraws.

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "RenderGraph/Frame/Revision.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
	 * @brief Registers DCLF's scene as a fragment source of the frame revision, like any other client. Main thread, once, before the
	 * render graph initializes. The frame revision takes DCLF's scene at the render owner's Begins, as the source's take allows (it
	 * can skip a Begin), adopts it alone or with the revision it was recorded against, and BeginFrame hands its publication over.
	 */
	void RegisterSceneSource();

	/** @brief Render thread. Whether the engine's utility shader existed at the frame's ticket. False before the first ticket. */
	bool TicketUtilityShader();

	/** @brief A feature's values in the frame ticket: taken of the engine by the game half, read by the render half. */
	enum class TicketPart : std::uint8_t
	{
		WaterScene,  // the tracked water shapes (Features/Water/WaterNative.cpp)
		SkyRoot,     // the sky root's objects (Features/Sky/SkyNative.cpp)
		Fades,       // the fade update's engine inputs (Features/Fades/FadeState.cpp)
		AnimatedFades,  // the animation job's fade updates (Features/Fades/FadeWriters.cpp)
		FirstPerson,  // the first-person latch and skeleton walk (Features/FirstPerson/FirstPersonView.cpp)
		Count
	};
	inline constexpr std::size_t kTicketParts = static_cast<std::size_t>(TicketPart::Count);

	/** @brief Takes a part's values in the game half, inside its capture scope. Null leaves the part empty. */
	using TicketPartCapture = std::shared_ptr<const void> (*)();

	/**
	 * @brief Folds a superseded ticket's part into the fresh ticket's, for values every game update adds to. Game thread, before the
	 * fresh ticket is published. Either may be null.
	 */
	using TicketPartFold = std::shared_ptr<const void> (*)(const std::shared_ptr<const void>& a_older, const std::shared_ptr<const void>& a_fresh);

	/** @brief At install, before the first frame. One capture per part. Without a fold, a superseded ticket's part is dropped. */
	void RegisterTicketPart(TicketPart a_part, TicketPartCapture a_capture, TicketPartFold a_fold = nullptr);

	/** @brief Render thread: the rendered ticket's part, or null (before the first ticket, or when its capture took nothing). */
	const void* TicketPartOf(TicketPart a_part);
	/** @brief Like TicketPartOf, for a reader that keeps the part past the render (a frame-value producer). */
	std::shared_ptr<const void> TicketPartHeld(TicketPart a_part);

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
		/**
		 * @brief Native, game thread: a routed update's game half (Native::Scene::PrepareFrame, at the SetupSunAndSky point). Takes DCLF's
		 * newest posted scene fragment into a new ticket and installs its claims (none new: the publication the render side adopted
		 * last). The render half renders the ticket later (SceneRenderStart).
		 */
		void SceneGameStart();
		/** @brief Game thread: the ticket the last game half made, null before the first. */
		const std::shared_ptr<const FrameTicket>& GameTicket() const { return gameTicket; }
		/**
		 * @brief Render owner, before the render's Renderer::Begin: the ticket this render renders. At its first render the Begin's frame
		 * revision adopts the ticket's scene fragment, and nothing else of DCLF's scene; a repeat takes nothing.
		 */
		void ExpectTicket(std::shared_ptr<const FrameTicket> a_ticket);
		/** @brief Render owner: a routed render's frame start, the render half (BeginFrame) of the ticket ExpectTicket named. */
		void SceneRenderStart();
		/** @brief Native, main thread: a game update the cadence renders no frame for. Its ticket retires unused when the next is made. */
		void UnrenderedUpdate(std::uint64_t a_snapshot);
		/** @brief The scene source's take at a Begin (Revision::FragmentSource::take). Render owner. */
		RenderGraph::Frame::Revision::FragmentTake TakeAtBegin(std::optional<RenderGraph::Frame::Revision::Fragment>& a_given);
		/** @brief Inside the adoption point: the publication the frame revision has adopted after it. */
		void NoteAdopted(std::shared_ptr<const void> a_publication);
		/** @brief Inside the adoption point: a revision left DCLF's scene, so no coverage counts until a render decides it again. */
		void NoteStoppedDrawing();
		/** @brief Where the claims' installation gate stands (drawing, the last coverage, the stops), for the load hold's stall line. */
		std::string DescribeClaimsGate() const;
		/** @brief Render thread. The ticket the frame renders, null before the first. */
		const FrameTicket* Ticket() const { return ticket.get(); }
		/**
		 * @brief The tickets made, those no render took, those a repeated render made again, and renders whose ticket's scene was not the
		 * one the frame revision had adopted (they draw no DCLF work), scenes a superseded ticket handed on, and scenes a ticket that
		 * retired without handing its scene put back in the post (returned) or let go for a newer one (released), since the last take.
		 */
		struct TicketCounts
		{
			std::uint64_t made = 0, unrendered = 0, remade = 0, unpaired = 0, carried = 0, returned = 0, released = 0;
		};
		TicketCounts TakeTicketCounts();
		/**
		 * @brief Native, game thread, at a publish: a_old's snapshot was never rendered (the cadence dropped it, or a newer publish
		 * replaced it before a render took it). Its scene fragment goes to a_fresh when a_fresh took none.
		 */
		void TicketSuperseded(const FrameTicket& a_old, const FrameTicket& a_fresh);
		/** @brief Before Present, in menus too. Queues the CPU pipeline's frame signal and the frame values' signal before DXVK presents. */
		void BeforePresent();
		/** @brief After Present, in menus too. Ends the frame and writes its report. */
		void EndFrame();
		/**
		 * @brief Native, game thread: the start of the skeleton driver's game half, every render. From DCLF's frame start to GameHalfEnd
		 * the rest of the game half reads the ticket's frame globals (FrameGlobals::BeginGameHalf).
		 */
		void GameHalfStart();
		/**
		 * @brief Native, main thread: the end of the skeleton driver's game half, every render. The late poses are confirmed here: the
		 * render half changes no scene graph, so the depth commit would see the same nodes.
		 */
		void GameHalfEnd();
		/** @brief Main thread, game side: the end of an update's call of Main::DrawFrame, after its renders (Snapshot::RegisterUpdateEnd). */
		void UpdateEnd();
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
		void MakeTicket(std::uint64_t a_snapshot, bool a_takeScene);
		/**
		 * @brief The game half: everything the frame start reads or writes of the engine, once per game update. a_takeScene: the game half
		 * picks the scene (routed updates, before the render's Begin); else it installs the one the frame revision adopted at this
		 * frame's Begin.
		 */
		std::shared_ptr<const FrameTicket> GameFrameStart(std::uint64_t a_snapshot, bool a_takeScene);
		/** @brief Render owner. Whether this render repeats the snapshot of the ticket it already renders. */
		bool RepeatsTicket() const;
		/** @brief Makes a_ticket the one the render side renders. */
		void RenderTicket(std::shared_ptr<const FrameTicket> a_ticket);
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
		std::shared_ptr<const FrameTicket> gameTicket;  // game side: the newest ticket made
		std::shared_ptr<const FrameTicket> ticket;      // render side: the ticket the frame renders
		std::shared_ptr<const FrameTicket> expected;    // render side: the routed render's ticket, from ExpectTicket to its frame start
		// Render side: the publication the frame revision adopted last. The game half installs it when no newer fragment was posted.
		std::atomic<std::shared_ptr<const void>> renderAdopted;
		// The last render's coverage of DCLF's claims epochs: the render half publishes it, the game half reads the latest (never waits).
		// 1 + the stops the render side had asked (FrameView::StopsAsked) when its last render was covered; 0 when it was not. A render
		// covered before a stop never lets the game half install the claims again after it.
		std::atomic<std::uint64_t> coveredLast{ 0 };
		std::atomic<std::uint64_t> ticketsMade{ 0 }, ticketsUnrendered{ 0 }, ticketsRemade{ 0 }, ticketsUnpaired{ 0 }, ticketScenesCarried{ 0 };
		std::uint32_t latePosesStep = ~0u;  // native, render side: the ticket whose late poses a depth pass used
	};
}
