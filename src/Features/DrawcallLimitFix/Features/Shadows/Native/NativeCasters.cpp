// Native mode's shadow light selection and shadow maps without the engine's shadow functions (see NativeCasters.h). Ports of
// Main::CalculateAndDrawShadowCasterLights (AE 0x1414cbb90, SE 0x1412e2660 with its sun part 0x1412e2e10), CalculateActiveShadowCasterLights
// (AE 0x1414cc570, SE 0x1412e2f60; its loop is LightSelection's) and Main::RenderShadowMaps (AE 0x1414cbff0, SE 0x1412e2a60).
// The sun's full-frustum culls and every light's cull and registration are not run: DCLF draws every shadow view from its own tables.
// The main camera's list accumulation and registration jobs are the engine's scene work and still run here, through the calls the
// engine's functions make (so DCLF's stand-in hooks and the engine-render detector see them as before).

#include "Features/DrawcallLimitFix/Features/Shadows/Native/NativeCasters.h"

#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LocalCameras.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/SunCamera.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "RenderGraph/Engine/Native/Native.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace DCLF::Features::Shadows::Native
{
	namespace
	{
		std::uint32_t keptThisFrame = 0;  // render thread, inside SelectCasters

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
		std::uint32_t& FocusTargets() { return Global<std::uint32_t>(527703, 414625); }             // 0x14332a498: the focus targets' count
		RE::ShadowSceneNode*& SceneNode() { return Global<RE::ShadowSceneNode*>(513211, 390951); }  // 0x142033060
		std::byte*& WorldGraph() { return Global<std::byte*>(528087, 415032); }                     // 0x14338c8e8: the world scene graph (camera at +0x128)

		// The main camera's culling: its processes, their lists and the portal records the list jobs use.
		std::uint32_t& ProcessCount() { return Global<std::uint32_t>(528071, 415016); }                            // 0x14338c868
		void**& Processes() { return Global<void**>(528077, 415022); }                                             // 0x14338c8a0
		std::byte*& Lists() { return Global<std::byte*>(528072, 415017); }                                         // 0x14338c870, 0x18 bytes a list
		std::uint32_t& RecordCount() { return Global<std::uint32_t>(528079, 415024); }                             // 0x14338c8b0
		void*& CullingJobs() { return Global<void*>(527999, 414944); }                                             // 0x14338c630: gJobList_SceneListAccumCulling
		void*& RegisterJobs() { return Global<void*>(528000, 414945); }                                            // 0x14338c638: gJobList_SceneListAccumRegister
		RE::BSShaderAccumulator*& DepthAccumulator() { return Global<RE::BSShaderAccumulator*>(528063, 415008); }  // 0x14338c828: the Z-prepass accumulator
		RE::BSShaderAccumulator*& MainAccumulator() { return Global<RE::BSShaderAccumulator*>(528064, 415009); }   // 0x14338c830
		std::uint8_t& DepthGroupsFlag() { return Global<std::uint8_t>(513437, 391262); }                           // 0x142033a70

		constexpr std::size_t kPortalEntry = 0x30190;    // a culling process's portal graph entry
		constexpr std::size_t kProcessRecord = 0x301A0;  // the process's portal record for the list jobs
		constexpr std::size_t kContextSize = 0x68;       // a list job's culling context
		constexpr std::size_t kRecordSize = 200;         // a portal record
		constexpr std::size_t kLensFlareData = 0x138;    // BSLight::lensFlareData
		constexpr std::uint32_t kMemoryContext = 0x1D;   // the TLS memory context the shadow lights' Render runs under
		constexpr std::size_t kTlsMemoryContext = 0x768;

		template <class F>
		F Function(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return reinterpret_cast<F>(REL::RelocationID(a_se, a_ae).address());
		}

		/** @brief The target of the rel32 CALL at a_site: the engine's function, or whatever hook sits on that call. */
		template <class F>
		F CallTarget(std::uint64_t a_se, std::uint64_t a_ae, std::ptrdiff_t a_seOffset, std::ptrdiff_t a_aeOffset)
		{
			const auto site = REL::RelocationID(a_se, a_ae).address() + (REL::Module::IsAE() ? a_aeOffset : a_seOffset);
			const auto rel = *reinterpret_cast<const std::int32_t*>(site + 1);
			return reinterpret_cast<F>(site + 5 + rel);
		}

		// Job plumbing (engine).
		using LockGuard = void (*)(void*, void*);
		using UnlockGuard = void (*)(void*);
		using QueueJob = void (*)(void*, void*, void*, std::uint64_t);
		using BeginJobs = void (*)(void*, std::uint64_t, std::uint64_t);
		using FinishJobs = void (*)(void*);
		using ObjectFn = void (*)(void*);
		using AttachRecord = void (*)(void*, void*);
		using AfterFullFrustumFn = void (*)(RE::ShadowSceneNode*, void*);
		using GroupsFn = void (*)(RE::BSShaderAccumulator*, std::uint32_t);

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

		void QueueLensFlareIfAny(RE::BSShadowLight& a_light)
		{
			if (DCLF::Engine::At<void*>(&a_light, kLensFlareData))
				QueueLensFlare(a_light);
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
			QueueLensFlareIfAny(*sun);
		}

		/** @brief One list job's culling context and the portal records, as 107132 makes and frees them. */
		struct ListAccumulation
		{
			std::vector<std::byte> records;
			std::vector<std::byte> contexts;
			std::uint32_t recordCount = 0;
			std::uint32_t processCount = 0;
		};

		void BeginListAccumulation(ListAccumulation& a_jobs, RE::NiCamera* a_camera)
		{
			// The engine calls these with the node and the first process; DCLF's stand-in hooks the first one's call.
			auto* first = Processes()[0];
			CallTarget<AfterFullFrustumFn>(100414, 107132, 0x4B, 0x15A)(SceneNode(), first);
			auto* entry = DCLF::Engine::At<std::byte*>(first, kPortalEntry);
			auto* portalList = entry ? entry + 0x68 : nullptr;
			if (portalList)
				Function<ObjectFn>(75078, 76846)(portalList);
			a_jobs.recordCount = RecordCount();
			a_jobs.records.assign(std::size_t(a_jobs.recordCount) * kRecordSize + 16, std::byte{ 0 });
			auto* records = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(a_jobs.records.data()) + 15) & ~std::uintptr_t(15));
			for (std::uint32_t i = 0; i < a_jobs.recordCount; ++i)
				Function<ObjectFn>(75051, 76815)(records + i * kRecordSize);
			a_jobs.processCount = ProcessCount();
			for (std::uint32_t i = 0; i < a_jobs.processCount; ++i) {
				if (portalList)
					Function<AttachRecord>(75055, 76819)(portalList, records + i * kRecordSize);
				DCLF::Engine::At<void*>(Processes()[i], kProcessRecord) = records + i * kRecordSize;
			}
			a_jobs.contexts.assign(std::size_t(a_jobs.processCount) * kContextSize + 16, std::byte{ 0 });
			auto* contexts = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(a_jobs.contexts.data()) + 15) & ~std::uintptr_t(15));
			for (std::uint32_t i = 0; i < a_jobs.processCount; ++i)
				Function<ObjectFn>(100211, 106919)(contexts + i * kContextSize);
			std::array<std::uint64_t, 3> guard{};
			Function<LockGuard>(67997, 69360)(guard.data(), CullingJobs());
			for (std::uint32_t i = 0; i < a_jobs.processCount; ++i) {
				auto* context = contexts + i * kContextSize;
				DCLF::Engine::At<RE::NiPointer<RE::NiCamera>>(context, 0x10) = RE::NiPointer<RE::NiCamera>(a_camera);
				DCLF::Engine::At<void*>(context, 0x30) = Processes()[i];
				DCLF::Engine::At<void*>(context, 0x48) = Lists() + std::size_t(i) * 0x18;
				DCLF::Engine::At<std::uint32_t>(context, 0x58) = i;
				// FirstListAccumulationJob for the first list, ListAccumulationJob for the rest.
				void* job = i == 0 ? reinterpret_cast<void*>(REL::RelocationID(100416, 107134).address()) : reinterpret_cast<void*>(REL::RelocationID(100417, 107135).address());
				Function<QueueJob>(67988, 69349)(CullingJobs(), job, context, 0);
			}
			Function<UnlockGuard>(67998, 69361)(guard.data());
			Function<BeginJobs>(67991, 69352)(CullingJobs(), 0, 0);
		}

		void FinishListAccumulation(ListAccumulation& a_jobs)
		{
			// The engine's Finish call, through DCLF's stand-in hook on it.
			CallTarget<FinishJobs>(100414, 107132, 0x2DD, 0x3BD)(CullingJobs());
			auto* contexts = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(a_jobs.contexts.data()) + 15) & ~std::uintptr_t(15));
			for (std::uint32_t i = 0; i < a_jobs.processCount; ++i)
				Function<ObjectFn>(16099, 16341)(contexts + i * kContextSize);
			auto* records = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(a_jobs.records.data()) + 15) & ~std::uintptr_t(15));
			for (std::uint32_t i = 0; i < a_jobs.recordCount; ++i)
				Function<ObjectFn>(75052, 76816)(records + i * kRecordSize);
		}

		/** @brief BSAccumProcess::DoSceneListAccumRegisterJob's argument (FUN_1414bf300 makes it). */
		struct RegisterJob
		{
			RE::NiPointer<RE::BSShaderAccumulator> accumulator;
			void*** processes = nullptr;
			std::uint32_t count = 0;
			std::uint32_t main = 0;
		};
		static_assert(sizeof(RegisterJob) == 0x18);

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
		QueueLensFlareIfAny(a_light);
	}

	void SelectCasters(RE::NiCamera* a_camera)
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
		ListAccumulation jobs;
		BeginListAccumulation(jobs, a_camera);
		LightSelection::Get().SelectNative();
		node->GetRuntimeData().firstPersonShadowMask = InsideMask();
		FinishListAccumulation(jobs);
		KeptLights() = keptThisFrame;
	}

	void RenderShadowMaps()
	{
		auto* world = WorldCamera();
		auto* main = MainAccumulator();
		if (!world || !main)
			return;
		auto* mainBytes = reinterpret_cast<std::byte*>(main);
		DCLF::Engine::At<std::uint8_t>(mainBytes, 0x5C) = 1;
		DCLF::Engine::At<RE::NiCamera*>(mainBytes, 0x10) = world;
		// The main camera's registration jobs (engine scene work), as RenderShadowMaps queues them.
		RegisterJob depthJob{ RE::NiPointer<RE::BSShaderAccumulator>(DepthAccumulator()), &Processes(), ProcessCount(), 0 };
		RegisterJob mainJob{ RE::NiPointer<RE::BSShaderAccumulator>(main), &Processes(), ProcessCount(), 1 };
		auto* job = reinterpret_cast<void*>(REL::RelocationID(100219, 106927).address());
		std::array<std::uint64_t, 3> guard{};
		Function<LockGuard>(67997, 69360)(guard.data(), RegisterJobs());
		Function<QueueJob>(67988, 69349)(RegisterJobs(), job, &depthJob, 0);
		Function<QueueJob>(67988, 69349)(RegisterJobs(), job, &mainJob, 0);
		Function<UnlockGuard>(67998, 69361)(guard.data());
		Function<BeginJobs>(67991, 69352)(RegisterJobs(), 0, 0);
		{
			const MemoryContext context;
			auto& accumulated = SceneNode()->GetRuntimeData().shadowLightsAccum;
			std::uint32_t index = 0;
			while (index < accumulated.size() && accumulated[index]) {
				auto* light = accumulated[index];
				const std::uint32_t before = index;
				// A light DCLF cannot draw casts no shadow this frame. ShadowMapsNative's line counts it.
				(void)Engine::RenderLightNatively(*light, index);
				if (index == before)
					++index;
			}
		}
		CallTarget<FinishJobs>(100415, 107133, 0x152, 0x1D0)(RegisterJobs());
		if (DepthGroupsFlag()) {
			auto* depth = DepthAccumulator();
			DCLF::Engine::At<RE::NiCamera*>(depth, 0x10) = world;
			Function<GroupsFn>(99966, 106613)(depth, 0x11);
			Function<GroupsFn>(99966, 106613)(depth, 9);
		}
	}
}
