// Installs the LOD gates' engine hooks, then registers their channel, show veto and coordinator component.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"

namespace DCLF::Features::Lod::Gates
{
	void RegisterGates(Api::InstallContext& a_context, Api::SwitchId a_switch)
	{
		LodGates::Install(a_switch, &ShellState::Running);
		if (!LodGates::Enabled())
			return;
		// A stack channel keeps gate events in order with the incoming blocks' attaches.
		channel = a_context.RegisterEngineChannel({ .name = "LOD gates",
			.kind = Api::ChannelKind::SceneStack,
			.apply = Api::ChannelApply::AfterBatch,
			.wake = Api::WakeSource::Pass });
		a_context.RegisterShowVeto({ .open = &LodGates::OpenGates(),
			.vetoes = &LodGates::Vetoes,
			.engineHid = &LodGates::NoteEngineHide,
			.hideSites = LodGates::EngineHideSites() });
		LodGates::NoteRegistered(channel, a_context.ReserveHiddenSite());
		// No adoption guard is needed. A forced release reaches the scene as ordinary hidden and tracker events and wakes a pass.
		component = a_context.RegisterCoordinatorComponent({ .name = "LOD gates",
			.make = &MakeState,
			.retirement = { .forgot = &Forgot },
			.events = { .keys = &ApplyOutcomes, .stacked = &ApplyGateEvent, .detached = &RootDetached, .underMark = &UnderMark },
			.commit = { .settle = &Settle, .take = &Take, .published = &Published },
			.adoption = { .adopt = &Adopt } });
		a_context.RegisterReportLines({ .name = "LOD gates",
			.point = Api::ReportPoint::SceneCoordinatorGates,
			.component = component,
			.componentText = &GateLines });
		a_context.RegisterReportLines({ .name = "LOD gate releases", .point = Api::ReportPoint::GateReleases, .lines = &ReleaseLines });
	}
}
