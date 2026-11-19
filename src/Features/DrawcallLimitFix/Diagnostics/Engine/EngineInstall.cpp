// The engine area's observers, and the engine hooks only observers use. Each checks its own switch.

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Accumulated.h"
#include "CutObservers.h"
#include "Registrations.h"
#include "RegistrationCensus.h"
#include "ReleaseGuard.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Diagnostics
{
	void InstallEngine()
	{
		const auto& o = Observe();
		ReleaseGuard::Install();
		if (o.accumulated)
			Accumulated::Install();
		CutObservers::Install(o.scene);
		RegistrationCensus::Install();
		// Several engine parities live in their features' installs.
		if (o.claims)
			Registrations::Install();
	}
}
