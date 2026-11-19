// The scene area's observers, each subscribed only when the run's switches ask. Observers of one event run in subscription order.

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "CaptureParity.h"
#include "MirrorWatch.h"
#include "Events.h"
#include "SceneObservers.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Diagnostics
{
	void InstallScene()
	{
		const auto& o = Observe();
		// The change log runs first in the scene phase, before anything of the pass writes.
		if (o.changeLog)
			InstallChangeLogParity();
		if (o.walk)
			InstallWalkParity();
		if (o.mirror) {
			InstallMirrorReads();
			InstallMirrorParity();
		} else
			InstallMirrorParity(true);  // the compatibility monitor's, while the player has it on
		// Prepass on the render thread.
		if (o.scene) {
			InstallFrameParity();
			InstallSlotDrainCheck();
			InstallCandidateParity();
			InstallShadowSetParity();
			InstallReplayParity();
		}
		InstallRegistrations(o.scene);
		if (o.residents)
			InstallResidentParity();
		if (o.claims)
			InstallCommitParity();
		// CS_DCLF_CAPTURE_PARITY: the materials written each frame, for its follow-ups.
		if (o.capture)
			Subscribe<ScenePoint::AccumulatePrepared>([](const On<ScenePoint::AccumulatePrepared>&) { CaptureParity::Get().DrainWrittenMaterials(); });
		if (MirrorWatch::Enabled())
			InstallMirrorWatchPlacement();
		if (SwitchEnabled(Switch::Census))
			InstallCensus();
		if (SwitchEnabled(Switch::PrecisionCensus))
			InstallPrecisionCensus();
	}
}
