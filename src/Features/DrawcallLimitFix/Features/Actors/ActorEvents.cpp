// Actors' engine events. SetAlpha fade writes go to the fade amounts channel. The actors' move writers fill reserved slots
// next to Core's own move writers.

#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DCLF::Features::Actors
{
	namespace
	{
		Api::MoveWriterId actor3DPosition, skyCellSkin, ragdoll;
		Api::EngineChannelId fadeAmounts;

		// Actor::SetAlpha (vfunc 0xE1, 0x1406c1a50) writes fadeAmount (+0x100) on the 3D root and the first-person root.
		// PRE saves both roots in scratch[0..1] and their fadeAmount in scratch[2..3].
		void OnSetAlpha(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			if (SceneWriter::IsPre(a_event)) {
				auto* actor = SceneWriter::Arg<RE::Actor*>(a_event, 0);
				RE::NiAVObject* roots[2]{ actor->Get3D1(false), actor->Get3D1(true) };
				for (std::uint32_t i = 0; i < 2; ++i) {
					const float before = roots[i] ? Engine::At<float>(roots[i], 0x100) : 0.0f;
					a_scratch[i] = reinterpret_cast<std::uintptr_t>(roots[i]);
					std::memcpy(&a_scratch[2 + i], &before, sizeof(before));
				}
				return;
			}
			for (std::uint32_t i = 0; i < 2; ++i) {
				auto* root = reinterpret_cast<RE::NiAVObject*>(a_scratch[i]);
				float before;
				std::memcpy(&before, &a_scratch[2 + i], sizeof(before));
				if (root && Engine::At<float>(root, 0x100) != before && (i == 0 || a_scratch[1] != a_scratch[0]))
					Extensions::PushChannelKey(fadeAmounts, root);
			}
		}

		void OnUpdate3DPosition(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Extensions::PushReservedMove(actor3DPosition, SceneWriter::Arg<RE::Actor*>(a_event, 0), Api::MoveReach::WithPlacements);
		}

		void OnSkyCellSkin(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Extensions::PushReservedMove(skyCellSkin, SceneWriter::Arg<RE::Actor*>(a_event, 0), Api::MoveReach::WithPlacements);
		}

		// FUN_140770dc0's list: the actors' handles at +0x30, their count at +0x40.
		void OnRagdollJob(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto* list = SceneWriter::Arg<const std::byte*>(a_event, 0);
			const auto* handles = *reinterpret_cast<const RE::RefHandle* const*>(list + 0x30);
			const auto count = *reinterpret_cast<const std::uint32_t*>(list + 0x40);
			for (std::uint32_t i = 0; handles && i < count; ++i) {
				// A handle that no longer resolves pushes nothing.
				RE::NiPointer<RE::TESObjectREFR> reference;
				const bool found = RE::LookupReferenceByHandle(handles[i], reference);
				Extensions::PushReservedMove(ragdoll, found ? reference.get() : nullptr, Api::MoveReach::WithPlacements);
			}
		}
	}

	void NeedFadeAmounts(Api::InstallContext& a_context)
	{
		fadeAmounts = a_context.NeedEngineChannel("fade amounts");
	}

	void InstallEngineEvents()
	{
		// PostPostLoad already required these calls, so a refusal here is fatal.
		if (!SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_SET_ALPHA, &OnSetAlpha))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused Actor::SetAlpha after reporting it installed");
		// All move writers or none, so only when Core's are installed.
		if (!Extensions::CoreMoveWritersLive())
			return;
		actor3DPosition = Extensions::ReserveMoveWriter("actor 3D position");
		skyCellSkin = Extensions::ReserveMoveWriter("sky cell skin");
		ragdoll = Extensions::ReserveMoveWriter("ragdoll");
		const bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_UPDATE_3D_POSITION, &OnUpdate3DPosition, SWF_PHASE_POST) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_SKY_CELL_SKIN, &OnSkyCellSkin, SWF_PHASE_POST) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_ACTOR_RAGDOLL_JOB, &OnRagdollJob, SWF_PHASE_POST);
		if (!subscribed)
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the actors' move writers after reporting them installed");
		for (const auto writer : { actor3DPosition, skyCellSkin, ragdoll })
			Extensions::MoveWriterInstalled(writer);
	}
}
