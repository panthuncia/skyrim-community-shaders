// The engine's shadow-light selection, hooked (render thread): NiCamera::CalculateAndDrawShadowCasterLights' call from Main::Draw, and its
// light-keeping calls (FUN_1414a3fb0, the sun's and each kept light's), which reach Core's FrameDriver: the frame's shadow views begun and
// their coverage decided (the epochs' beforeShadowCasters) before the engine keeps a light, each light's views before its Accumulate
// registers into them, and the list once the selection is done (skyrim-engine-notes.md, "The shadow lights' selection"). Installed from
// the feature's install.

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
		// Render thread: DCLF runs this frame's selection (the shell's Running, sampled at its start, so that the three points agree).
		bool castersRunning = false;

		/**
		 * @brief Main::Draw's call of NiCamera::CalculateAndDrawShadowCasterLights (AE 0x140644674): the whole selection, every shadow light's
		 * UpdateCamera and Accumulate (the registrations into its views) inside it. The views begun before, listed after.
		 */
		struct CalculateAndDrawShadowCasterLights
		{
			static void thunk(void* a_camera)
			{
				castersRunning = ShellState::Running();
				if (castersRunning)
					FrameDriver::Get().BeforeShadowCasters();
				func(a_camera);
				if (castersRunning)
					FrameDriver::Get().ShadowCastersSelected();
				castersRunning = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FUN_1414a3fb0(shadow scene node, light, first slot, slots): the light written into its slots of shadowLightsAccum, once the
		 * engine kept it - its UpdateCamera run and its focus flag set - and right before its Accumulate. Site 0 is the sun's (AE 0x1414cbc66,
		 * its cascades' slots), site 1 each kept light's (AE 0x1414cc7e1, one slot).
		 */
		/**
		 * @brief The focus accumulation (Address::kFocusAccumulation): the host's focus views listed and covered before it registers into
		 * them, which for a spot light host is before the light is kept (KeepLight).
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
		// Chained onto whatever the calls hold (PreflightCasterHooks required CALLs).
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
