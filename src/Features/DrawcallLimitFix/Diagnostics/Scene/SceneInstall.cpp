// The scene area's observers, each subscribed only when the run's switches ask. Observers of one event run in subscription order.

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "CaptureParity.h"
#include "Events.h"
#include "SceneObservers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Diagnostics
{
	void InstallScene()
	{
		const auto& o = Observe();
		// CS_DCLF_PARITY runs the mirror's probe every frame. Otherwise it is the compatibility monitor's, while the player has it on.
		InstallMirrorParity(!o.parity);
		// CS_DCLF_CAPTURE_PARITY: the materials written each frame, for its follow-ups.
		if (o.capture)
			Subscribe<ScenePoint::AccumulatePrepared>([](const On<ScenePoint::AccumulatePrepared>&) { CaptureParity::Get().DrainWrittenMaterials(); });
	}
}
