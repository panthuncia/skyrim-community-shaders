// The Fades feature's install. Fades steps the fade roots' OnVisible state and is the only producer of root states.
// The install order below matters because several pieces must register after other features' equivalents.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Utils/Hash.h"
#include "Features/DrawcallLimitFix/Features/Fades/Fades.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <bit>

namespace DCLF::Scene
{
	namespace
	{
		Api::EngineChannelId fadeSnaps, fadeAmounts;  // set at install, read-only after
	}

	Api::EngineChannelId FadeSnapsChannelId()
	{
		return fadeSnaps;
	}

	Api::EngineChannelId FadeAmountsChannelId()
	{
		return fadeAmounts;
	}
}

namespace DCLF::Features::Fades
{
	namespace
	{
		// The fade-out distance is measured from the node's centre. Trees also get the height test.
		void RootTest(const Api::RootTestInput& a_input, Api::RootTestOutput& a_out)
		{
			a_out.distance = FadeDistanceOf(*a_input.node);
			a_out.objectFlags = (a_input.node->kind & SceneCapture::kKindTree) ? kObjectHeightTest : 0u;
		}

		// Mixes the globals FadeDistanceOf reads. Keep the order MembershipWitness used, or the witness changes.
		void RootGlobalsKey(const Api::RootTestInput& a_input, std::uint32_t& a_witness)
		{
			const auto& g = a_input.globals;
			const auto mix = [&](std::uint32_t a_value) { a_witness = (a_witness ^ a_value) * Util::Fnv1a32::kPrime; };
			mix(g.fadesOn | (g.fadeLodUpdates << 8) | (g.fadeSpecialA == g.fadeSpecialB ? 0x10000u : 0u));
			mix(std::bit_cast<std::uint32_t>(g.fadeDistanceMult));
			mix(std::bit_cast<std::uint32_t>(g.fadeOutThreshold));
			mix(std::bit_cast<std::uint32_t>(g.fadeDefaultScale));
			for (std::uint32_t type = 0; type < 13; ++type)
				mix(std::bit_cast<std::uint32_t>(g.fadeTypeDivisors[type]));
		}

		// The write-back task is kicked at the frame start and never joined.
		void RegisterRootStates(Api::InstallContext& a_context)
		{
			a_context.RegisterRootStates({ .name = "fade roots",
				.family = Scene::FadeRootFamilyId(),
				.view = &Scene::ViewFadeRoots,
				.objectFlags = kObjectFadeTest | kObjectHeightTest,
				.test = &RootTest,
				.globalsKey = &RootGlobalsKey,
				.kick = &KickFadeWriteBack,
				.latchGlobals = &FadeState::LatchGlobals });
		}

		void Install(Api::InstallContext& a_context)
		{
			// The family must come after TreeWind's trees, and the component last of all components.
			RegisterScene(a_context);
			RegisterEngine(a_context, Scene::fadeSnaps, Scene::fadeAmounts);
			RegisterRender(a_context);
			RegisterRootStates(a_context);
			// The fade update's engine inputs, taken in the game half for the render half.
			RegisterTicketPart(TicketPart::Fades, &FadeState::TakeTicketPart);
			RegisterDiagnostics(a_context);
			// Last, so the writer stores run after the other observers at Main::Update's entry.
			RegisterWriters(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "fades", .install = &Install, .earlyHooks = &InstallEarlyHooks, .preflight = &PreflightWriters };
	}
}
