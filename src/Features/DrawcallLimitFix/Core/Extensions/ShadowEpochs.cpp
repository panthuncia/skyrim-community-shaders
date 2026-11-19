// The shadow epochs' entry.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowEpochs.h"

#include <utility>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		Api::ShadowEpochsContribution resolved;
		bool registered = false;
	}

	void ResolveShadowEpochs(const Api::ShadowEpochsContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the shadow epochs were resolved twice");
		if (!a_registered.afterShadowMaps)
			return;
		resolved = a_registered;
		registered = true;
	}

	const Api::ShadowEpochsContribution* ShadowEpochs() { return registered ? &resolved : nullptr; }
}
