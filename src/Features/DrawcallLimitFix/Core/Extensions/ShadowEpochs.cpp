// The shadow epochs' entry.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowEpochs.h"
#include "RenderGraph/Fatal.h"

#include <utility>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		Api::ShadowEpochsContribution shadowEpochs;
		bool shadowEpochsRegistered = false;
	}

	void ResolveShadowEpochs(const Api::ShadowEpochsContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the shadow epochs were resolved twice");
		if (!a_registered.afterShadowMaps)
			return;
		shadowEpochs = a_registered;
		shadowEpochsRegistered = true;
	}

	const Api::ShadowEpochsContribution* ShadowEpochs() { return shadowEpochsRegistered ? &shadowEpochs : nullptr; }
}
