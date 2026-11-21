// Hooks on the engine's shadow-light selection, on the render thread. They begin the frame's shadow views and decide their coverage
// before the engine keeps a light, hand each light's views over before its Accumulate, and list them once the selection is done.

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/ShadowEngineAddresses.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"

#include <cstdint>

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		// Inside the selection, so the three points agree.
		bool castersRunning = false;

		/** @brief Main::Draw's call of CalculateAndDrawShadowCasterLights (AE 0x140644674). The views are begun before and listed after. */
		struct CalculateAndDrawShadowCasterLights
		{
			static void thunk(void* a_camera)
			{
				castersRunning = true;
				FrameDriver::Get().BeforeShadowCasters();
				func(a_camera);
				FrameDriver::Get().ShadowCastersSelected();
				castersRunning = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The focus accumulation. The host's focus views are listed and covered before it registers into them.
		 * For a spot light host that is before the light is kept.
		 */
		struct FocusAccumulation
		{
			static void thunk(RE::BSShadowLight* a_light)
			{
				const bool running = castersRunning;
				if (running)
					FrameDriver::Get().ShadowFocusAccumulating(a_light, true);
				func(a_light);
				if (running)
					FrameDriver::Get().ShadowFocusAccumulating(a_light, false);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FUN_1414a3fb0, which writes a kept light into its shadowLightsAccum slots right before its Accumulate.
		 * Site 0 is the sun's (AE 0x1414cbc66), site 1 each kept light's (AE 0x1414cc7e1).
		 */
		template <std::uint32_t Site>
		struct KeepLight
		{
			static void thunk(void* a_node, RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
			{
				func(a_node, a_light, a_firstSlot, a_slots);
				if (castersRunning)
					FrameDriver::Get().ShadowCasterKept(a_light, a_firstSlot, a_slots);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallCasterHooks()
	{
		// Chained onto whatever the calls hold. PreflightCasterHooks required CALLs.
		const auto base = REL::Module::get().base();
		const auto keep = base + Address::kKeepLight;
		const auto chained = [](std::uintptr_t a_site, std::uintptr_t a_engine, const char* a_what) {
			if (!DCLF::Engine::CallsTo(a_site, a_engine))
				logger::info("[DCLF] shadow casters: {} is hooked by {}; DCLF's thunk chains onto it", a_what, HookSites::BranchTargetModule(a_site));
		};
		chained(base + Address::kCastersCallSite, base + Address::kCalculateAndDrawShadowCasterLights, "Main::Draw's shadow caster lights call");
		chained(base + Address::kKeepSunCallSite, keep, "the sun's keep call");
		chained(base + Address::kKeepLightCallSite, keep, "the lights' keep call");
		stl::write_thunk_call<CalculateAndDrawShadowCasterLights>(base + Address::kCastersCallSite);
		stl::write_thunk_call<KeepLight<0>>(base + Address::kKeepSunCallSite);
		stl::write_thunk_call<KeepLight<1>>(base + Address::kKeepLightCallSite);
		if (Address::kFocusAccumulation.Known()) {
			stl::detour_thunk<FocusAccumulation>(REL::Offset(Address::kFocusAccumulation.Get()).address());
			HookSites::NoteDetour("the focus accumulation detour", FocusAccumulation::func.address());
		} else {
			logger::warn("[DCLF] shadow casters: the focus accumulation is not known for this runtime; a spot light's focus views reach the engine");
		}
		logger::info("[DCLF] shadow casters: the shadow lights' selection hooked (its call, each light kept, the focus accumulation)");
	}

	void PreflightCasterHooks()
	{
		const auto base = REL::Module::get().base();
		HookSites::RequireCall("Main::Draw's shadow caster lights call", base + Address::kCastersCallSite);
		HookSites::RequireCall("the shadow lights' selection's sun keep call", base + Address::kKeepSunCallSite);
		HookSites::RequireCall("the shadow lights' selection's light keep call", base + Address::kKeepLightCallSite);
	}
}
