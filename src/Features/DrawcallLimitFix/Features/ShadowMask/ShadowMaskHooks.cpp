// Engine-side hooks for the shadow mask family. Render thread.
// At the world's and a menu scene's shadow mask calls, the family computes the masks' draws from the shadow lights
// (ShadowMaskNative.cpp), does the call's own bookkeeping, and the engine's call does not run. A batch the native build cannot
// compute reaches the engine, logged once per reason.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"
#include "Snapshot/EngineReads.h"

#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/RenderDepth.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/FrameAnchors.h"
#include "State.h"
#include "Utils/Logging.h"
#include "Utils/VersionedRelocation.h"

#include <format>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Features::ShadowMask
{
	namespace
	{
		namespace Calls
		{
			// What the masks' calls do besides their draws (AE, SE).
			inline constexpr Engine::Offset kSetDepthTarget{ { 77454 }, { 75647 } };       // RenderTargetManager::SetCurrentDepthStencilTarget
			inline constexpr Engine::Offset kSetRenderTarget{ { 77453 }, { 75646 } };      // RenderTargetManager::SetCurrentRenderTarget
			inline constexpr Engine::Offset kRestoreClearColours{ { 77250 }, { 75464 } };  // Renderer::RestoreClearColors, a menu scene's last call
			inline constexpr Engine::Offset kRenderTargets{ { 411451 }, { 524970 } };      // BSGraphics::RenderTargetManager (the object)
		}

		/** @brief The batches the engine draws, each logged once per reason. */
		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] shadow mask: the engine draws");
			return log;
		}

		/** @brief What both calls do first: no depth target, and kSHADOW_MASK as RT0, cleared and with its viewport. */
		void SetTargets()
		{
			using SetDepth = void (*)(void*, std::int32_t, std::uint32_t, std::uint32_t, bool);
			using SetTarget = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, bool);
			static REL::Relocation<SetDepth> setDepth{ REL::Offset(Calls::kSetDepthTarget) };
			static REL::Relocation<SetTarget> setTarget{ REL::Offset(Calls::kSetRenderTarget) };
			auto* targets = reinterpret_cast<void*>(REL::Offset(Calls::kRenderTargets).address());
			setDepth(targets, -1, 3, 0, false);
			setTarget(targets, 0, kShadowMaskTarget, 0, true);
		}

		/**
		 * @brief What the calls do after their lights: the write mode back to 1; the world's call returns each light's maps (after
		 * its draw, which the native build has read), a menu scene's restores the clear colours.
		 */
		void Finish(View a_view, std::span<const Native::Light> a_lights)
		{
			if (a_view == View::World)
				for (const auto& light : a_lights)
					const_cast<RE::BSShadowLight*>(light.light)->ReturnShadowmaps();
			auto& rss = CS_ENGINE_READ(ShadowState, CapturePoint, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
			if (rss.alphaBlendWriteMode != 1) {
				rss.alphaBlendWriteMode = 1;
				rss.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
			}
			if (a_view == View::Menu) {
				static REL::Relocation<void (*)(void*)> restore{ REL::Offset(Calls::kRestoreClearColours) };
				restore(CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer));
			}
		}

		/** @brief Hands a built run to the anchor, or keeps its owners when it has no draw. */
		void SubmitMaskRun(std::shared_ptr<Run> a_run)
		{
			if (a_run->draws.empty()) {
				ShadowMaskDraws::Keep(std::move(a_run->owners));
			} else {
				DCLF_ZONE("CS.DCLF.ShadowMask.Submit");
				ShadowMaskDraws::Submit(std::move(a_run));
			}
		}

		/**
		 * @brief SubmitMaskRun after the sun's cascades go to Volumetric Shadows, Exponential Height Fog and Skylighting, which take them at
		 * the mask draw.
		 */
		void SubmitMaskRunWithSun(View a_view, std::span<const Native::Light> a_lights, std::shared_ptr<Run> a_run)
		{
			if (auto* sun = Native::SunMap(a_view, a_lights))
				globals::state->OnShadowMaskDraw(sun);
			SubmitMaskRun(std::move(a_run));
		}

		/** @brief A shadow mask call hook. The draws are computed and run at the anchor in the engine's place. */
		template <View kView>
		struct MaskBatch
		{
			static void thunk(void* a_arg1, void* a_arg2, void* a_arg3, void* a_arg4)
			{
				if (!FrameAnchors::IsRenderOwner() || !ShadowMaskDraws::Available() || !ShadowMaskDraws::CanTake(kView)) {
					func(a_arg1, a_arg2, a_arg3, a_arg4);
					return;
				}
				DCLF_ZONE("CS.DCLF.ShadowMask.Native");
				Family::BeginBatch();
				Native::EndTechnique();
				SetTargets();
				auto run = std::make_shared<Run>();
				run->view = kView;
				// The engine renders this scene: its lights are read here, where they are drawn.
				const auto* node = Native::SceneNodeOf(kView);
				const auto lights = Shadows::Native::ReadLights(node);
				std::string why = node ? "" : "no shadow scene node";
				if (!node || !Native::Build(kView, *run, lights, Shadows::Native::ReadMaskSettings(), Native::ReadSun(), why)) {
					Refusals().Report(std::format("a {} batch: {}", NameOf(kView), why));
					ShadowMaskDraws::Keep(std::move(run->owners));
					func(a_arg1, a_arg2, a_arg3, a_arg4);
					return;
				}
				// The clear lands before the anchor's run, as at the engine's first draw. The engine applies the vertex layout from the bound
				// vertex shader; when none was ever bound (every render before this one was CS's, as at the first loading screen after a
				// native main menu) there is no layout to apply, and applying it would read the null shader.
				{
					DCLF_ZONE("CS.DCLF.ShadowMask.Clear");
					auto& rss = CS_ENGINE_READ(ShadowState, CapturePoint, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
					if (!rss.currentVertexShader && rss.stateUpdateFlags.any(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC)) {
						static std::once_flag logged;
						std::call_once(logged, [] { logger::info("[DCLF] shadow mask: a {} batch with no vertex shader bound; its vertex layout is not applied", NameOf(kView)); });
						rss.stateUpdateFlags.reset(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
					}
					Engine::ApplyPendingState();
				}
				SubmitMaskRunWithSun(kView, lights, std::move(run));
				Finish(kView, lights);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Main::RenderPlayerView's call of Main::RenderShadowMasks (SE 100422, AE 107140). */
		struct RenderShadowMasks
		{
			static void thunk(bool a_arg)
			{
				// Native drives Main::RenderShadowMasks' work itself; the engine's function is not entered.
				if (::RenderGraph::Engine::Native::Active())
					::RenderGraph::Engine::Native::RenderShadowMasks();
				else
					func(a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		std::uintptr_t RenderShadowMasksSite() { return REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x39C, 0x39C, 0x3BA); }
	}

	void DrawWorldMasks(const float a_clear[4])
	{
		DCLF_ZONE("CS.DCLF.ShadowMask.Native");
		static Util::ReasonLog skipped("[DCLF] shadow mask (native): the world masks are only cleared");
		Family::BeginBatch();
		// SetCurrentRenderTarget(0, kSHADOW_MASK, SRTM_CLEAR) with the clear colour RenderShadowMasks set: the clear lands before the
		// anchor's run.
		if (!::RenderGraph::Engine::Skeleton::ClearColour(RE::RENDER_TARGET::kSHADOW_MASK, a_clear))
			skipped.Report("the skeleton did not take kSHADOW_MASK's clear");
		// Until a revision registers the client, the masks are not drawn.
		if (!ShadowMaskDraws::Available() || !ShadowMaskDraws::CanTake(View::World)) {
			skipped.Report("the family cannot take them yet");
			return;
		}
		auto run = std::make_shared<Run>();
		run->view = View::World;
		// The light set the render half adopted. Its lights gave their maps back on the game side once it was made.
		static const Shadows::Native::LightSet kNoSet;
		const auto* set = Shadows::Native::RenderSet();
		const auto& lights = set ? *set : kNoSet;
		std::string why;
		if (!Native::Build(View::World, *run, lights.lights, lights.mask, lights.sun, why))
			::RenderGraph::Fatal("shadow masks", std::format("native's world masks cannot be built: {}", why));
		WatchSun(set, *run);
		SubmitMaskRunWithSun(View::World, lights.lights, std::move(run));
	}

	void DrawMenuMasks(const RE::NiPoint3& a_eye)
	{
		DCLF_ZONE("CS.DCLF.ShadowMask.Native");
		Family::BeginBatch();
		// SetCurrentRenderTarget(0, kSHADOW_MASK, SRTM_CLEAR): each light then writes its own channel, the only one its passes read.
		constexpr float kClear[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		if (!::RenderGraph::Engine::Skeleton::ClearColour(RE::RENDER_TARGET::kSHADOW_MASK, kClear))
			::RenderGraph::Fatal("shadow masks", "kSHADOW_MASK's clear at a native menu scene render was not taken");
		if (!ShadowMaskDraws::Available() || !ShadowMaskDraws::CanTake(View::Menu))
			return;
		auto run = std::make_shared<Run>();
		run->view = View::Menu;
		// A menu scene renders where its lights are kept: they are read here.
		const auto* node = Native::SceneNodeOf(View::Menu);
		const auto lights = Shadows::Native::ReadLights(node);
		std::string why = node ? "" : "no shadow scene node";
		if (!node || !Native::Build(View::Menu, *run, lights, Shadows::Native::ReadMaskSettings(), Native::ReadSun(), why, &a_eye))
			::RenderGraph::Fatal("shadow masks", std::format("a native menu scene's masks cannot be built: {}", why));
		SubmitMaskRun(std::move(run));
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kWorldMasks.Known() && Address::kMenuMasks.Known();
		HookSites::RequireKnown("the shadow mask calls", known);
		if (!known)
			return;
		HookSites::RequireCall("the world's shadow masks (Main::RenderShadowMasks' call)", REL::Offset(Address::kWorldMasks).address());
		HookSites::RequireCall("a menu scene's shadow masks (UI3DSceneManager::Render's call)", REL::Offset(Address::kMenuMasks).address());
		HookSites::RequireCall("Main::RenderPlayerView's shadow masks call", RenderShadowMasksSite());
	}

	void InstallHooks()
	{
		if (!Address::kWorldMasks.Known() || !Address::kMenuMasks.Known()) {
			logger::warn("[DCLF] shadow mask: this runtime has no address for the shadow mask calls; the shadow masks stay the engine's");
			return;
		}
		stl::write_thunk_call<MaskBatch<View::World>>(REL::Offset(Address::kWorldMasks).address());
		stl::write_thunk_call<MaskBatch<View::Menu>>(REL::Offset(Address::kMenuMasks).address());
		stl::write_thunk_call<RenderShadowMasks>(RenderShadowMasksSite());
		Family::AddSetup("shadow mask", [] { return ShadowMaskDraws::Setup(); });
		logger::info("[DCLF] shadow mask: hooks installed (the world's and the menu scenes' shadow masks)");
	}
}
