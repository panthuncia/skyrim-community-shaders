// Portals: the walk's thunk, the main cull input and its programs buffer, and RoomVisibility's detours.

#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Features/Portals/RoomVisibility.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"

namespace DCLF::Features::Portals
{
	namespace
	{
		constexpr Engine::Offset kPortalWalkCallSite{ { 36564, 0x9bf }, { 35571, 0xdb }, { 36564, 0x9d1 } };  // Main::Update -> FUN_1401a43c0, the main camera's portal walk
		constexpr Engine::Offset kPortalWalk{ { 13408 }, { 13257 } };

		/** @brief The game half's main portal context, for its ticket. */
		std::shared_ptr<const void> TakeTicketPart()
		{
			return PortalViews::Get().MainContext();
		}

		/** @brief The frame's programs, for the CPU pipeline's culls and set parity (PortalViews::FramePrograms). */
		std::span<const std::uint32_t> ProgramWords()
		{
			return PortalViews::Get().FramePrograms();
		}

		/** @brief Main::Update's portal walk (FUN_1401a43c0). DCLF's walk follows it with the same camera, before the list build. */
		struct PortalWalk
		{
			static void thunk(void* a_tes, void* a_process)
			{
				func(a_tes, a_process);
				// Only with the primary cull installed (written once at PostPostLoad), as when its install wrote this thunk.
				if (Extensions::StandInInstalled())
					PortalViews::Get().Build();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Writes the walk's thunk, which the primary cull's install requires. Chained, since Preflight required a CALL. */
		void EarlyHooks()
		{
			const auto base = REL::Module::get().base();
			if (!Engine::CallsTo(base + kPortalWalkCallSite, base + kPortalWalk))
				logger::info("[DCLF] portals: Main::Update's portal walk call is hooked by {}; DCLF's thunk chains onto it", HookSites::BranchTargetModule(base + kPortalWalkCallSite));
			stl::write_thunk_call<PortalWalk>(base + kPortalWalkCallSite);
			Extensions::NoteMainWalkInstalled();
		}

		void Preflight()
		{
			HookSites::RequireCall("Main::Update's portal walk call", REL::Module::get().base() + kPortalWalkCallSite);
			RoomVisibility::Preflight();
		}

		bool UnboundVisible()
		{
			return PortalViews::Get().VisibleUnbound();
		}

		void Structure(const void* a_parent)
		{
			PortalViews::Get().NoteStructure(static_cast<const RE::NiNode*>(a_parent));
		}

		std::uint64_t ProgramsVersion()
		{
			return PortalViews::Get().ProgramsVersion();
		}

		/** @brief Each asked root's program: its room's (ProgramOf), in Core's order. */
		void Judge(Api::RootProgramsJudge& a_roots)
		{
			const auto& portals = PortalViews::Get();
			for (std::size_t i = 0, count = a_roots.Count(); i < count; ++i)
				a_roots.Put(i, portals.ProgramOf(static_cast<const RE::NiAVObject*>(a_roots.Root(i))));
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterSwitch({ .name = "CS_DCLF_PORTAL_UNIFORM_SCAN", .kind = Api::SwitchKind::Diagnostic, .description = "Evaluate portal batches with the diagnostic uniform instruction scan" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_PORTAL_TRACE", .kind = Api::SwitchKind::Diagnostic,
				.description = "Portal batches write each query's decision trace" });
			a_context.RegisterMainCullInput({ .name = "portals",
				.noProgram = kPortalNoProgram,
				.unboundVisible = &UnboundVisible,
				.structure = &Structure,
				.programsVersion = &ProgramsVersion,
				.judge = &Judge,
				.test = &PortalTest,
				.programWords = &ProgramWords });
			RegisterTicketPart(TicketPart::Portals, &TakeTicketPart);
			RegisterPrograms(a_context);
			a_context.RegisterStandInObserver({ .updateEntry = &RoomVisibility::SetLive });
			RoomVisibility::Install();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "portals", .install = &Install, .earlyHooks = &EarlyHooks, .preflight = &Preflight };
	}
}
