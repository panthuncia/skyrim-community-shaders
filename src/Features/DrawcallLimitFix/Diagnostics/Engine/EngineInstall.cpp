// The engine area's observers. Each checks its own switch.

#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Registrations.h"
#include "ReleaseGuard.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Diagnostics
{
	void InstallEngine()
	{
		ReleaseGuard::Install();
		// Set parity's withheld-pass counts.
		if (Observe().parity)
			Registrations::Install();
	}
}
