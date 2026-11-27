// Native mode's shadow light selection and shadow maps without the engine's shadow functions (see NativeCasters.h). Ports of
// Main::CalculateAndDrawShadowCasterLights (AE 0x1414cbb90, SE 0x1412e2660 with its sun part 0x1412e2e10), CalculateActiveShadowCasterLights
// (AE 0x1414cc570, SE 0x1412e2f60; its loop is LightSelection's) and Main::RenderShadowMaps (AE 0x1414cbff0, SE 0x1412e2a60).
// The sun's full-frustum culls and every light's cull and registration are not run: DCLF draws every shadow view from its own tables.
// Neither are the main camera's list accumulation jobs nor its registration jobs: the scene lists are DCLF's, and the game-visible
// effects of the list culls are produced on the game thread (Snapshot/SideEffects.h). The selection and the shadow maps' bookkeeping
// run on the game side and hand the render side the light set (LightSet.h); RenderShadowMaps reads only the set.

#include "Features/DrawcallLimitFix/Features/Shadows/Native/NativeCasters.h"

#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LocalCameras.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/SunCamera.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/RenderScene.h"
#include "Snapshot/EngineReads.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace DCLF::Features::Shadows::Native
{
	namespace
	{
		std::uint32_t keptThisFrame = 0;                  // game side, inside SelectCasters
		std::vector<const RE::BSShadowLight*> drawnMaps;  // render side: the lights RenderShadowMaps drew this frame

		template <class T>
		T& Global(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return *reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address());
		}

		// The selection's globals (AE address).
		std::uint32_t& LightCount() { return Global<std::uint32_t>(528091, 415036); }               // 0x14338c904: the shadow light count (gLastFrameEnabledShadowCasterLightsCount2)
		std::uint32_t& MaskCounter() { return Global<std::uint32_t>(528092, 415037); }              // 0x14338c908: the next mask index
		std::uint32_t& InsideMask() { return Global<std::uint32_t>(528093, 415038); }               // 0x14338c90c: lights whose sphere holds the camera
		std::uint8_t& SunOff() { return Global<std::uint8_t>(528095, 415040); }                     // 0x14338c911
		std::uint8_t& FocusFound() { return Global<std::uint8_t>(528096, 415041); }                 // 0x14338c912
		std::uint32_t& KeptLights() { return Global<std::uint32_t>(528090, 415035); }               // 0x14338c900: gLastFrameEnabledShadowCasterLightCount
		RE::BSShadowLight*& LastFocusHost() { return Global<RE::BSShadowLight*>(528315, 415267); }  // 0x1433dcfb8
		std::uint8_t& FocusSetting() { return Global<std::uint8_t>(513201, 390932); }               // 0x142032fd0: focus shadows on
		std::uint8_t& LensFlaresOn() { return Global<std::uint8_t>(513349, 391128); }               // 0x142033658: lens flares are on
		std::uint32_t& FocusTargets() { return Global<std::uint32_t>(527703, 414625); }             // 0x14332a498: the focus targets' count
		RE::ShadowSceneNode*& SceneNode() { return Global<RE::ShadowSceneNode*>(513211, 390951); }  // 0x142033060
		std::byte*& WorldGraph() { return Global<std::byte*>(528087, 415032); }                     // 0x14338c8e8: the world scene graph (camera at +0x128)

		void**& Processes() { return Global<void**>(528077, 415022); }                                             // 0x14338c8a0: the main camera's list processes
		RE::BSShaderAccumulator*& MainAccumulator() { return Global<RE::BSShaderAccumulator*>(528064, 415009); }   // 0x14338c830

		constexpr std::uint32_t kMemoryContext = 0x1D;   // the TLS memory context the shadow lights' Render runs under
		constexpr std::size_t kTlsMemoryContext = 0x768;

		/** @brief The target of the rel32 CALL at a_site: the engine's function, or whatever hook sits on that call. */
		template <class F>
		F CallTarget(std::uint64_t a_se, std::uint64_t a_ae, std::ptrdiff_t a_seOffset, std::ptrdiff_t a_aeOffset)
		{
			const auto site = REL::RelocationID(a_se, a_ae).address() + (REL::Module::IsAE() ? a_aeOffset : a_seOffset);
			const auto rel = *reinterpret_cast<const std::int32_t*>(site + 1);
			return reinterpret_cast<F>(site + 5 + rel);
		}

		using AfterFullFrustumFn = void (*)(RE::ShadowSceneNode*, void*);

		/** @brief The TLS memory context id, set for a scope as the engine's Render loop does. */
		class MemoryContext
		{
		public:
			MemoryContext()
			{
				const auto index = Global<std::uint32_t>(528600, 415542);  // the executable's TLS index
				auto* tls = reinterpret_cast<std::byte**>(__readgsqword(0x58));
				slot = reinterpret_cast<std::uint32_t*>(tls[index] + kTlsMemoryContext);
				saved = *slot;
				*slot = kMemoryContext;
			}
			~MemoryContext() { *slot = saved; }
			MemoryContext(const MemoryContext&) = delete;
			MemoryContext& operator=(const MemoryContext&) = delete;

		private:
			std::uint32_t* slot = nullptr;
			std::uint32_t saved = 0;
		};

		/** @brief FUN_1414a4030 (AE 106367): every shadowLightsAccum slot emptied, its light's mask index reset. */
		void ResetAccumulated(RE::ShadowSceneNode& a_node)
		{
			auto& accumulated = a_node.GetRuntimeData().shadowLightsAccum;
			for (auto& light : accumulated) {
				if (light) {
					light->GetRuntimeData().maskIndex = 0xFF;
					light = nullptr;
				}
			}
		}

		/** @brief FUN_1414a3fb0 (AE 106365): a_light into a_slots slots from a_first. A light it displaces loses its mask index. */
		void Keep(RE::ShadowSceneNode& a_node, RE::BSShadowLight* a_light, std::uint32_t a_first, std::uint32_t a_slots)
		{
			auto& accumulated = a_node.GetRuntimeData().shadowLightsAccum;
			for (std::uint32_t slot = a_first; slot < a_first + a_slots && slot < accumulated.size(); ++slot) {
				if (auto* old = accumulated[slot]; old && old != a_light)
					old->GetRuntimeData().maskIndex = 0xFF;
				accumulated[slot] = a_light;
			}
			FrameDriver::Get().ShadowCasterKept(a_light, a_first, a_slots);
		}

		RE::NiCamera* WorldCamera()
		{
			auto* graph = WorldGraph();
			return graph ? DCLF::Engine::At<RE::NiCamera*>(graph, 0x128) : nullptr;
		}

		/** @brief The sun's part of the selection (107132's first branch) and of CalculateActive's start. */
		void SelectSun(RE::ShadowSceneNode& a_node, RE::NiCamera& a_world, bool a_focusOn)
		{
			auto* sun = a_node.GetRuntimeData().sunShadowDirLight;
			if (SunOff()) {
				if (sun)
					ReturnShadowmaps(*sun);
				return;
			}
			if (!sun)
				return;
			auto& data = sun->GetRuntimeData();
			data.drawFocusShadows = a_focusOn;
			UpdateSunCamera(*sun, a_world);
			Sun::SunViews::Get().Update(sun);
			if (a_focusOn) {
				UpdateFocusCameras(*sun, a_world);
				Local::FocusViews::Get().Update(sun);
				FocusFound() = 1;
			}
			Keep(a_node, sun, LightCount(), sun->shadowMapCount);
			++keptThisFrame;
			data.maskIndex = MaskCounter()++;
			InsideMask() |= 1u << (LightCount() & 31);
			// CalculateActive's sun: the cascades' bookkeeping, the focus accumulators, the lens flare.
			AccumulateBookkeeping(*sun, LightCount(), 0);
			if (a_focusOn) {
				FrameDriver::Get().ShadowFocusAccumulating(sun, true);
				PrepareFocusAccumulators(*sun);
				FrameDriver::Get().ShadowFocusAccumulating(sun, false);
				data.drawFocusShadows = true;
				FocusFound() = 1;
				LastFocusHost() = nullptr;
			}
		}

		/**
		 * @brief 107132's call right after the full-frustum cull (AE 106323, the culling globals the frame reads), through whatever hooks
		 * that call: DCLF's primary cull prepares its frame there. No list job follows: the main camera's list culls are not run in native.
		 */
		void AfterFullFrustum()
		{
			CallTarget<AfterFullFrustumFn>(100414, 107132, 0x4B, 0x15A)(SceneNode(), Processes()[0]);
		}
	}

	bool Replaces()
	{
		return ::RenderGraph::Engine::Native::Requested();
	}

	bool UpdateLightCamera(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera)
	{
		if (a_light.GetIsDirectionalLight()) {
			const bool visible = UpdateSunCamera(static_cast<RE::BSShadowDirectionalLight&>(a_light), a_worldCamera);
			Sun::SunViews::Get().Update(&a_light);
			return visible;
		}
		if (a_light.GetIsParabolicLight())
			return UpdateParabolicCamera(static_cast<RE::BSShadowParabolicLight&>(a_light), a_worldCamera);
		if (a_light.GetIsFrustumLight())
			return UpdateFrustumCamera(static_cast<RE::BSShadowFrustumLight&>(a_light), a_worldCamera);
		return false;
	}

	void FocusHost(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera)
	{
		UpdateFocusCameras(a_light, a_worldCamera);
		Local::FocusViews::Get().Update(&a_light);
		FrameDriver::Get().ShadowFocusAccumulating(&a_light, true);
		PrepareFocusAccumulators(a_light);
		FrameDriver::Get().ShadowFocusAccumulating(&a_light, false);
		a_light.GetRuntimeData().drawFocusShadows = true;
		FocusFound() = 1;
		LastFocusHost() = &a_light;
	}

	void KeepLocal(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera, std::uint32_t a_kept)
	{
		auto* node = SceneNode();
		const std::uint32_t first = LightCount();
		Keep(*node, &a_light, first, 1);
		++keptThisFrame;
		a_light.GetRuntimeData().maskIndex = MaskCounter()++;
		ProjectLightRect(a_light, a_worldCamera, first);
		AccumulateBookkeeping(a_light, LightCount(), a_kept);
	}

	void SelectCasters()
	{
		auto* node = SceneNode();
		auto* world = WorldCamera();
		if (!node || !world)
			return;
		LightCount() = 0;
		MaskCounter() = 0;
		InsideMask() = 0;
		ResetAccumulated(*node);
		const bool focusOn = FocusSetting() != 0 && FocusTargets() != 0;
		FocusFound() = 0;
		keptThisFrame = 0;
		SelectSun(*node, *world, focusOn);
		// Stage 5 is the game-side producer's (Snapshot/SideEffects.cpp).
		AfterFullFrustum();
		LightSelection::Get().SelectNative();
		node->GetRuntimeData().firstPersonShadowMask = InsideMask();
		PrimaryCull::Get().NoListJobs();
		KeptLights() = keptThisFrame;
	}

	void CaptureSet()
	{
		const Snapshot::EngineReads::CaptureScope scope;
		auto set = std::make_shared<LightSet>();
		auto* node = SceneNode();
		set->mask = ReadMaskSettings();
		set->sun = ReadSun(node ? node->GetRuntimeData().sunShadowDirLight : nullptr);
		if (node) {
			// The light walk RenderShadowMaps and the masks make: each kept light once, from its first slot.
			const MemoryContext context;
			auto& accumulated = node->GetRuntimeData().shadowLightsAccum;
			const bool lensFlares = LensFlaresOn() != 0;
			std::uint32_t index = 0;
			while (index < accumulated.size() && accumulated[index]) {
				auto* light = accumulated[index];
				CS_ENGINE_READ_MARK(ShadowLights, CapturePoint);
				const std::uint32_t before = index;
				ShadowLight maps;
				Engine::PrepareLightMaps(*light, maps, index);
				// Read after the bookkeeping, which gave the descriptors their targets, slices and light transforms.
				auto& captured = set->lights.emplace_back(ReadLight(*light));
				captured.verdict = maps.verdict;
				captured.draws = std::move(maps.draws);
				captured.left = maps.left;
				// The selection queues a kept light with lens flare data for the gather (AE 0x1414cec70).
				if (lensFlares)
					captured.lensFlare = light->lensFlareData;
				if (index == before)
					++index;
			}
			// The masks' loop gave each light's maps back after its draw, drawn or not. The render side draws from the set's values, so the
			// slices go back here, before the next update's game logic can remove a light.
			for (const auto& captured : set->lights)
				ReturnShadowmaps(*const_cast<RE::BSShadowLight*>(captured.light));
			// The sun's descriptors as the bookkeeping left them, for the cascade record, also when the sun casts nothing this update.
			if (const auto* sun = set->sun.light)
				set->sun.descriptors = set->Find(sun) ? set->Find(sun)->descriptors : ReadLight(*sun).descriptors;
		}
		// The main accumulator keeps the camera and flag RenderShadowMaps gave it before RenderWorld's batches. Its registration jobs
		// are not run: native draws nothing from the main camera's registered passes, so the engine's batch calls that still mark CS's
		// stage points find nothing to draw.
		if (auto* main = MainAccumulator(); main && WorldCamera()) {
			auto* mainBytes = reinterpret_cast<std::byte*>(main);
			DCLF::Engine::At<std::uint8_t>(mainBytes, 0x5C) = 1;
			DCLF::Engine::At<RE::NiCamera*>(mainBytes, 0x10) = WorldCamera();
		}
		MadeSet(std::move(set));
	}

	void RenderShadowMaps()
	{
		drawnMaps.clear();
		// The native driver's render half adopted its game half's set. A frame the engine's own skeleton draws (the frozen menu
		// capture) selected just before, on this thread: it draws that set.
		if (!::RenderGraph::Engine::Native::Scene::Current().valid)
			AdoptSet(TakeSet());
		const auto* set = RenderSet();
		if (!set)
			return;
		// A light DCLF cannot draw casts no shadow this frame. ShadowMapsNative's line counts it.
		for (const auto& light : set->lights)
			if (Engine::RenderLightNatively(light))
				drawnMaps.push_back(light.light);
	}

	bool DrewMaps(const RE::BSShadowLight* a_light)
	{
		return std::ranges::find(drawnMaps, a_light) != drawnMaps.end();
	}
}
