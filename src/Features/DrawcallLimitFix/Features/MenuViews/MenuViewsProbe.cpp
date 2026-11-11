// The menu views' probe: thunks on the menu scenes' SubmitAccumulator calls (UI3DSceneManager::Render's, LocalMapRenderer::Render's two)
// that read what a DCLF view of the scene would need and log it once per change, then let the engine draw the scene. Main thread only:
// the loading screen's model renders on the ServingThread (MistMenu, scheme kLoading), which stays the engine's (the p5-loading lane).

#include "Features/DrawcallLimitFix/Features/MenuViews/MenuViews.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include "RenderGraph/FrameAnchors.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <atomic>
#include <format>
#include <vector>

namespace DCLF::Features::MenuViews
{
	namespace
	{
		const char* NameOf(MenuScene a_scene)
		{
			switch (a_scene) {
			case MenuScene::Menu3D:
				return "menu 3D";
			case MenuScene::LocalMap:
				return "local map";
			case MenuScene::LocalMapWater:
				return "local map water";
			default:
				return "?";
			}
		}

		const char* SchemeName(std::uint32_t a_scheme)
		{
			// RE::INTERFACE_LIGHT_SCHEME, the index of UI3DSceneManager's menuObjects (+0x38) and current scheme (+0x90).
			constexpr std::array<const char*, 8> kNames{ "journal", "inventory", "magic", "book", "loading", "unk05", "stats", "lockpicking" };
			return a_scheme < kNames.size() ? kNames[a_scheme] : "?";
		}

		/** @brief What one render of a menu scene needs from a view: the inputs of the probe's verdict. */
		struct Probe
		{
			MenuScene scene = MenuScene::Count;
			std::uint32_t scheme = ~0u;  // Menu3D's light scheme
			std::array<std::int32_t, 3> targets{ -1, -1, -1 };
			std::int32_t depth = -1;
			std::uint32_t stencilMode = 0, stencilRef = 0;
			bool ortho = false;
			bool worldFrame = false;   // DCLF's frame advanced since the scene's last render (the world was drawn this frame)
			bool freezeFrame = false;  // a menu with kFreezeFrameBackground is open: the engine draws no world frame
			bool installed = false;    // DCLF's scene has an installed publication
			std::uint32_t geometry = 0, lighting = 0, skinned = 0, tracked = 0, claimed = 0;
			std::uint64_t signature = 0;  // the root's geometry

			bool SameShape(const Probe& a_other) const
			{
				return scene == a_other.scene && scheme == a_other.scheme && targets == a_other.targets && depth == a_other.depth && stencilMode == a_other.stencilMode &&
				       stencilRef == a_other.stencilRef && ortho == a_other.ortho && worldFrame == a_other.worldFrame && freezeFrame == a_other.freezeFrame &&
				       installed == a_other.installed && signature == a_other.signature && tracked == a_other.tracked;
			}
		};

		/** @brief The probe's state (main thread) and its report counters (taken by the report). */
		struct State
		{
			std::array<Probe, static_cast<std::size_t>(MenuScene::Count)> last{};
			std::array<std::uint32_t, static_cast<std::size_t>(MenuScene::Count)> lastFrame{};
			std::array<bool, static_cast<std::size_t>(MenuScene::Count)> seen{};  // rendered before (lastFrame holds a frame)
			std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(MenuScene::Count)> renders{};
			std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(MenuScene::Count)> withoutWorldFrame{};
			std::atomic<std::uint64_t> offThread{ 0 };
		};

		State& GetState()
		{
			static State state;
			return state;
		}

		bool FreezeFrameMenuOpen()
		{
			// MenuManager::sub (SE 0x140ebe220): Main's frame (SE 0x1405b1020) draws the world only when no open menu has
			// kFreezeFrameBackground (IMenu flag 0x20); otherwise it renders it once (FUN_1405b1650, Main::Draw mode 0xE) and composites
			// the frozen frame. The UI's lock is held: this runs inside UI::Render (UI__DrawInterface), which took it.
			auto* ui = RE::UI::GetSingleton();
			if (!ui)
				return false;
			for (const auto& menu : ui->menuStack)
				if (menu && menu->FreezeFrameBackground())
					return true;
			return false;
		}

		/** @brief The root's geometry, against the installed tables: tracked by DCLF's scene, and claimed by a set phase. */
		void ReadGeometry(RE::NiAVObject* a_root, Probe& a_probe)
		{
			std::vector<RE::BSGeometry*> geometry;
			if (a_root)
				SceneTracker::CollectGeometry(a_root, geometry);
			a_probe.geometry = static_cast<std::uint32_t>(geometry.size());
			std::uint64_t signature = 0xcbf29ce484222325ull;
			for (auto* g : geometry) {
				signature = (signature ^ reinterpret_cast<std::uintptr_t>(g)) * 0x100000001b3ull;
				const auto& data = g->GetGeometryRuntimeData();
				if (data.shaderProperty && netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get()))
					++a_probe.lighting;
				if (data.skinInstance)
					++a_probe.skinned;
			}
			a_probe.signature = signature;
			auto& view = Scene::FrameView::Get();
			a_probe.installed = view.HasInstalled();
			if (geometry.empty() || !a_probe.installed)
				return;
			const ankerl::unordered_dense::set<const RE::BSGeometry*> wanted(geometry.begin(), geometry.end());
			const auto& tables = view.GetTables();
			const auto claims = view.FrameClaims();
			for (std::size_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				const auto* g = tables.objectGeometry[o];
				if (!g || (tables.objects[o].flags & kObjectFree) || !wanted.contains(g))
					continue;
				++a_probe.tracked;
				if (claims && claims->PhasesOf(g))
					++a_probe.claimed;
			}
		}

		/**
		 * @brief The verdict for a menu scene's view: what stops DCLF from drawing it (the first missing input), or that every input a view
		 * needs is there. The view is not built in this stage, so the engine draws the scene either way.
		 */
		const char* Verdict(const Probe& a_probe)
		{
			if (!a_probe.installed)
				return "DCLF's scene has no publication";
			if (!a_probe.worldFrame)
				return a_probe.freezeFrame ? "no DCLF frame (a freeze-frame menu is open: the engine draws no world frame)" : "no DCLF frame this frame";
			if (a_probe.scene == MenuScene::Menu3D) {
				if (!a_probe.geometry)
					return "the scheme's root has no geometry";
				if (a_probe.tracked < a_probe.geometry)
					return "DCLF's scene does not track the menu root's geometry";
				if (a_probe.lighting < a_probe.geometry)
					return "geometry other than BSLightingShader (effects) needs its own family";
			}
			return "every input a view needs is there (the view is not built yet)";
		}

		void Log(const Probe& a_probe)
		{
			const auto stencil = a_probe.stencilMode ? std::format("stencil mode {} ref {:#x}", a_probe.stencilMode, a_probe.stencilRef) : std::string("no stencil");
			std::string scheme;
			if (a_probe.scene == MenuScene::Menu3D)
				scheme = std::format(" scheme {} ({}): {} geometries ({} Lighting, {} skinned), {} tracked by DCLF's scene, {} claimed;", a_probe.scheme,
					SchemeName(a_probe.scheme), a_probe.geometry, a_probe.lighting, a_probe.skinned, a_probe.tracked, a_probe.claimed);
			logger::info("[DCLF] menu views: {}{} targets {} {} {}, depth {}, {}, {} camera; world frame {}{}; {}", NameOf(a_probe.scene), scheme, a_probe.targets[0],
				a_probe.targets[1], a_probe.targets[2], a_probe.depth, stencil, a_probe.ortho ? "orthographic" : "perspective", a_probe.worldFrame ? "yes" : "no",
				a_probe.freezeFrame ? " (freeze-frame menu open)" : "", Verdict(a_probe));
		}

		/**
		 * @brief One render of a menu scene: its probe read from the shadow state as SubmitAccumulator will bind it (the engine set the
		 * targets and the stencil mode just before the call) and from DCLF's scene, logged when anything changed since the scene's last.
		 */
		void ProbeRender(MenuScene a_scene, std::uint32_t a_scheme, RE::NiAVObject* a_root, const RE::NiCamera* a_camera)
		{
			auto& state = GetState();
			const auto index = static_cast<std::size_t>(a_scene);
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			Probe probe;
			probe.scene = a_scene;
			probe.scheme = a_scheme;
			for (std::size_t t = 0; t < probe.targets.size(); ++t)
				probe.targets[t] = static_cast<std::int32_t>(rss.renderTargets[t]);
			probe.depth = static_cast<std::int32_t>(rss.depthStencil);
			probe.stencilMode = rss.depthStencilStencilMode;
			probe.stencilRef = rss.stencilRef;
			probe.ortho = a_camera && a_camera->GetRuntimeData2().viewFrustum.bOrtho;
			// DCLF's frame number moves at its frame start (Main::Draw): the same number at two renders of the scene means no world frame
			// ran between them. The first render has nothing to compare with and counts as one without.
			const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
			probe.worldFrame = state.seen[index] && frame != state.lastFrame[index];
			state.lastFrame[index] = frame;
			state.seen[index] = true;
			probe.freezeFrame = FreezeFrameMenuOpen();
			if (a_scene == MenuScene::Menu3D)
				ReadGeometry(a_root, probe);
			else
				probe.installed = Scene::FrameView::Get().HasInstalled();
			state.renders[index].fetch_add(1, std::memory_order_relaxed);
			if (!probe.worldFrame)
				state.withoutWorldFrame[index].fetch_add(1, std::memory_order_relaxed);
			if (!probe.SameShape(state.last[index])) {
				Log(probe);
				state.last[index] = probe;
			}
		}

		bool Probing()
		{
			if (!ShellState::Running())
				return false;
			if (!FrameAnchors::OnRenderThread()) {
				GetState().offThread.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			return true;
		}

		/** @brief UI3DSceneManager::Render's SubmitAccumulator (SE 51855 +0x658, AE 52727 +0x659): the scheme's model. */
		struct Menu3DSubmit
		{
			static void thunk(RE::NiCamera* a_camera, RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
			{
				if (Probing())
					if (auto* manager = RE::UI3DSceneManager::GetSingleton()) {
						const auto scheme = static_cast<std::uint32_t>(manager->currentlightScheme);
						RE::NiAVObject* root = scheme < std::size(manager->menuObjects) ? manager->menuObjects[scheme].get() : nullptr;
						ProbeRender(MenuScene::Menu3D, scheme, root, a_camera);
					}
				func(a_camera, a_accumulator, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief LocalMapRenderer::Render's first SubmitAccumulator (SE 16094 +0x340, AE 16335 +0x342): the land and objects. */
		struct LocalMapSubmit
		{
			static void thunk(RE::NiCamera* a_camera, RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
			{
				if (Probing())
					ProbeRender(MenuScene::LocalMap, ~0u, nullptr, a_camera);
				func(a_camera, a_accumulator, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Its second (SE +0x430, AE +0x433): the water pass, render mode 0x13. */
		struct LocalMapWaterSubmit
		{
			static void thunk(RE::NiCamera* a_camera, RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
			{
				if (Probing())
					ProbeRender(MenuScene::LocalMapWater, ~0u, nullptr, a_camera);
				func(a_camera, a_accumulator, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void PreflightHooks()
	{
		if (Menu3DEnabled()) {
			HookSites::RequireKnown("the menu 3D submit", Address::kMenu3DSubmit.Known());
			if (Address::kMenu3DSubmit.Known())
				HookSites::RequireCall("the menu 3D submit (UI3DSceneManager::Render's SubmitAccumulator)", REL::Offset(Address::kMenu3DSubmit).address());
		}
		if (MapEnabled()) {
			HookSites::RequireKnown("the local map submits", Address::kLocalMapSubmit.Known() && Address::kLocalMapWaterSubmit.Known());
			if (Address::kLocalMapSubmit.Known() && Address::kLocalMapWaterSubmit.Known()) {
				HookSites::RequireCall("the local map submit (LocalMapRenderer::Render's first SubmitAccumulator)", REL::Offset(Address::kLocalMapSubmit).address());
				HookSites::RequireCall("the local map water submit (LocalMapRenderer::Render's second SubmitAccumulator)", REL::Offset(Address::kLocalMapWaterSubmit).address());
			}
		}
	}

	void InstallHooks()
	{
		if (Menu3DEnabled()) {
			if (Address::kMenu3DSubmit.Known())
				stl::write_thunk_call<Menu3DSubmit>(REL::Offset(Address::kMenu3DSubmit).address());
			else
				logger::warn("[DCLF] menu views: this runtime has no address for the menu 3D submit; menu 3D stays the engine's");
		}
		if (MapEnabled()) {
			if (Address::kLocalMapSubmit.Known() && Address::kLocalMapWaterSubmit.Known()) {
				stl::write_thunk_call<LocalMapSubmit>(REL::Offset(Address::kLocalMapSubmit).address());
				stl::write_thunk_call<LocalMapWaterSubmit>(REL::Offset(Address::kLocalMapWaterSubmit).address());
			} else
				logger::warn("[DCLF] menu views: this runtime has no address for the local map submits; the local map stays the engine's");
		}
		logger::info("[DCLF] menu views: hooks installed (menu 3D {}, local map {})", Menu3DEnabled() ? "on" : "off", MapEnabled() ? "on" : "off");
	}

	std::string Report()
	{
		auto& state = GetState();
		std::string text;
		for (std::size_t s = 0; s < static_cast<std::size_t>(MenuScene::Count); ++s) {
			const auto renders = state.renders[s].exchange(0, std::memory_order_relaxed);
			const auto without = state.withoutWorldFrame[s].exchange(0, std::memory_order_relaxed);
			if (renders)
				text += std::format("menu views: {} rendered {} times ({} without a DCLF frame); the engine drew them\n", NameOf(static_cast<MenuScene>(s)), renders, without);
		}
		if (const auto off = state.offThread.exchange(0, std::memory_order_relaxed))
			text += std::format("menu views: {} menu scene renders off the main thread (the loading screen's), the engine's\n", off);
		return text;
	}
}
