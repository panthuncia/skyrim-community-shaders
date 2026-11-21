// The draws area's observers.

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Events.h"
#include "CullCounters.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void InstallDraws()
	{
		const auto& o = Observe();
		// Readback order after a main epoch is culling counters, set parity, then set class parity. Features add theirs after.
		Subscribe<CullLatchWritten>(&NoteCullLatch);
		Subscribe<MainEpochDone>(&ReadCullCounters);
		Subscribe<MainGraphPasses>(&AppendCullCounterPass);
		Subscribe<FadeInputsVaried>(&LogFadeInputsVaried);
		Subscribe<MainCullingSetup>(&LogCullingSetup);
		if (!o.parity)
			return;
		Subscribe<MainEpochDone>(&CheckSetParity);
		Subscribe<MainEpochDone>(&CheckSetClassParity);
		// Read without waiting for the producer, so a lease the producer refused still shows as an unsampled row.
		Subscribe<MainEpochDone>([](const MainEpochDone& a_epoch) {
			static std::uint32_t epochs = 0;
			if (!a_epoch.depthOnly && ++epochs % 60 == 0)
				CheckShadingSampled(Scene::FrameView::Get().GetTables());
		});
		// The kept stores against a rebuild.
		Subscribe<ExtrasSending>(&MirrorExtras);
		Subscribe<ExtrasSent>(&CheckExtras);
		Subscribe(&CheckObjectRecords);
		Subscribe(&CheckGeometryDraws);
		Subscribe(&CheckDrawBound);
		Subscribe<ShadowBuilt>(&CheckKeptShadow);
	}
}
