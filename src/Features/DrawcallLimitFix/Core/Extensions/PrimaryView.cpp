// The primary view's entry.

#include "Features/DrawcallLimitFix/Core/Extensions/PrimaryView.h"
#include "RenderGraph/Fatal.h"

#include <utility>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		Api::PrimaryViewContribution resolved;
		bool registered = false;
	}

	void ResolvePrimaryView(const Api::PrimaryViewContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the primary view was resolved twice");
		if (!a_registered.beforeOpaque)
			return;
		resolved = a_registered;
		registered = true;
	}

	const Api::PrimaryViewContribution* PrimaryView() { return registered ? &resolved : nullptr; }
}
