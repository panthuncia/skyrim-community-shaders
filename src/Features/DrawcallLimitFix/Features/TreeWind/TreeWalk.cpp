// TreeWalk: a move event after each walked tree's update pass. Main thread. TreeWalk.h describes it.
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWalk.h"

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"

#include <array>
#include <cstring>

namespace DCLF::Features::TreeWind::TreeWalk
{
	namespace
	{
		// SE's call is in FUN_1403df7d0. AE's is in FUN_1404381e0's near list loop, right after the bones' wind.
		constexpr Engine::Offset kWalkUpdateCall{ { 27095, 0x315 }, { 26517, 0x10e } };
		constexpr std::array<std::uint8_t, 6> kWalkUpdateBytes{ 0xFF, 0x90, 0x68, 0x01, 0x00, 0x00 };
		constexpr std::size_t kUpdateSelectedSlot = 0x2D;  // NiAVObject::UpdateSelectedDownwardPass (vtable +0x168)

		bool installed = false;
		Api::MoveWriterId moveWriter;

		// Make the site's virtual call here, then push the event.
		void Walked(RE::NiAVObject* a_tree, RE::NiUpdateData* a_data, std::uint32_t a_flags)
		{
			using Pass = void (*)(RE::NiAVObject*, RE::NiUpdateData*, std::uint32_t);
			reinterpret_cast<Pass>((*reinterpret_cast<void***>(a_tree))[kUpdateSelectedSlot])(a_tree, a_data, a_flags);
			Extensions::PushReservedMove(moveWriter, a_tree->GetUserData(), Api::MoveReach::WithPlacements);
		}
	}

	void Install(Api::InstallContext& a_context)
	{
		if (installed || !kWalkUpdateCall.Known())
			return;
		// Preflight checked the call's bytes before any hook was written.
		const auto site = REL::Module::get().base() + kWalkUpdateCall;
		moveWriter = a_context.ReserveMoveWriter("tree animation");
		SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&Walked));
		REL::safe_write(site + 5, std::uint8_t{ 0x90 });
		Extensions::MoveWriterInstalled(moveWriter);
		installed = true;
		logger::info("[DCLF] tree manager walk: its trees' update pass followed by a move event");
	}

	bool Installed() { return installed; }

	void Preflight()
	{
		if (kWalkUpdateCall.Known())
			HookSites::RequireBytes("the tree manager walk's update pass call", REL::Module::get().base() + kWalkUpdateCall, kWalkUpdateBytes);
	}
}
