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

namespace RE
{
	class BSRenderPass;
	class BSShadowLight;
}

namespace DCLF
{
	/**
	 * @brief Native mode with segments. Runs the build point at Renderer::Begin, before the frame's schedule is chosen.
	 * BeginFrame then skips it for that frame.
	 */
	void BuildPointAtFrameStart(std::uint64_t a_frame);

	/**
	 * @brief DCLF's side of its scene fragment in the frame revision (plan step 10b), called from Frame::Revision::AdoptLatest at
	 * every main-thread render-owner Begin, in both modes. The frame revision takes DCLF's newest snapshot, adopts it alone or with
	 * the revision it was recorded against, and BeginFrame hands its publication over.
	 */
	void SceneTaken(const std::shared_ptr<const void>& a_scene);
	void InstallScene(const std::shared_ptr<const void>& a_scene);
	void DropScene(const std::shared_ptr<const void>& a_scene, RenderGraph::Frame::Revision::SceneDrop a_why, std::string_view a_text,
		std::span<const std::uint32_t> a_epochs);
	/** @brief The frames whose coverage ran a DCLF epoch, in the session. The frame revision waits for a scene only while it moves. */
	std::uint64_t SceneRecordedFrames();
	/** @brief After the scene step: the shader-reload wait (plan step 10a). */
	void AfterSceneStep();

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
		 * @brief Main::Draw's early point, every frame. With a_running false the coordinator's passes still apply events.
		 * True when DCLF runs this frame.
		 */
		bool BeginFrame(bool a_running, bool a_installed);
		/** @brief Before Present, in menus too. Queues the CPU pipeline's frame signal and the frame values' signal before DXVK presents. */
		void BeforePresent();
		/** @brief After Present, in menus too. Ends the frame and writes its report. */
		void EndFrame(bool a_running);
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
		bool OcclusionReady(std::uint32_t a_map, bool a_running);
		/** @brief Whether the map is held, which means it renders without the engine's cull. */
		bool OcclusionHeld(std::uint32_t a_map) const;
		bool OcclusionNeedsEngine(std::uint32_t a_map, bool a_running);
		/** @brief Start of the main pass's opaque batches. Applies the engine's pending state, then captures the main view. */
		void BeforeOpaquePass();
		/** @brief End of the opaque batches. Runs the main view's colour epoch. */
		void AfterOpaquePass();
		/** @brief A native lighting draw in the main pass. */
		void NativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	private:
		/** @brief Runs every view's work at a_point, in slot order. */
		void RunViewWork(Api::ViewPoint a_point);

		/** @brief Adds shadow view time for this frame. It is counted at BeforeShadowMaps. */
		void AddShadowViewsTime(std::chrono::steady_clock::time_point a_start);

		Counters counters;
		double shadowViewsMs = 0.0;
		OcclusionFrames occlusionFrames;  // native and engine only, the rest is read at the take
		bool frameRan = false;  // since the last Present
		std::uint32_t occlusionHeld = 0;
		std::uint32_t occlusionWanted = 0;  // until AfterShadowMaps
		std::uint32_t occlusionDrawn = 0;
		std::uint32_t occlusionEverDrawn = 0;  // an engine draw of one of these is a re-entry
		// Per map, frames in a row the engine would have drawn it whole. A map DCLF never drew counts as a re-entry only
		// for kOcclusionReentryFrames, so a map it cannot draw at all is not held forever.
		std::array<std::uint32_t, 32> occlusionNeedStreak{};
		static constexpr std::uint32_t kOcclusionReentryFrames = 300;
		std::uint32_t captureFrame = ~0u;
	};
}
