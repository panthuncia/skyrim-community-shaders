// The first-person view's two engine hooks, both on calls to the accumulator render (FUN_1414a90f0).
// RenderDepth's first-person draw takes the culled bits and depth range. Main::RenderFirstPersonView's runs the colour epoch.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// SE calls StartAccumulating instead of inlining it, but both sites are plain CALLs (E8) on both runtimes.
		namespace Address
		{
			inline constexpr Engine::Offset kDepthFirstPersonDraw{ { 107139, 0x94e }, { 100421, 0x427 } };
			inline constexpr Engine::Offset kFirstPersonViewDraw{ { 107129, 0x101 }, { 100411, 0x103 } };
			inline constexpr Engine::Offset kAccumulatorRender{ { 106436 }, { 99789 } };  // FUN_1414a90f0
		}

		Api::TriggerId colourRender;

		// AE 0x1414cd47e, after the root's first-person cull and before kPOST_ZPREPASS_COPY. DCLF's depth epoch already drew
		// into the same depth, and no world depth draws in between.
		struct RenderDepth_FirstPersonDraw
		{
			static void thunk(void* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				if (ShellState::Running()) {
					RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ZPrepass);
					// The hidden bits as the engine's first-person cull left them. Next frame's member chains read these.
					if (Extensions::ViewOn(SceneToggles(), kView))
						if (auto* player = RE::PlayerCharacter::GetSingleton())
							FrameGlobals::NoteFirstPersonCulled(player->Get3D(true));
					TakeDepthRange(ShellState::Passes().depthControlDriven);
				}
				func(a_camera, a_accumulator, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// AE 0x1414cb7d1. DCLF's colour epoch draws first, then the engine draws whatever it still holds.
		struct RenderFirstPersonView_Draw
		{
			static void thunk(void* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				Diagnostics::Notify(Diagnostics::TargetPoint{ "before first person" });
				if (ShellState::Running()) {
					RenderThreadBudget::Hook budget(RenderThreadBudget::Site::AfterOpaque);
					{
						Extensions::TriggerScope trigger(colourRender);
						ExecuteColour();
					}
					globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
				}
				Diagnostics::Notify(Diagnostics::TargetPoint{ "after DCLF first person" });
				func(a_camera, a_accumulator, a_flags);
				Diagnostics::Notify(Diagnostics::TargetPoint{ "after first person" });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void PreflightHooks()
	{
		HookSites::RequireCall("RenderDepth's first-person draw", REL::Offset(Address::kDepthFirstPersonDraw).address());
		HookSites::RequireCall("Main::RenderFirstPersonView's accumulator render", REL::Offset(Address::kFirstPersonViewDraw).address());
	}

	void InstallHooks(Api::InstallContext& a_context)
	{
		colourRender = a_context.FindTrigger("fp.colour-render");
		// Always installed. They chain onto whatever the calls already hold.
		stl::write_thunk_call<RenderDepth_FirstPersonDraw>(REL::Offset(Address::kDepthFirstPersonDraw).address());
		stl::write_thunk_call<RenderFirstPersonView_Draw>(REL::Offset(Address::kFirstPersonViewDraw).address());
	}
}
