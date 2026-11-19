// The late pose contributions.

#include "Features/DrawcallLimitFix/Core/Extensions/LatePoses.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		std::vector<LatePoseEntry> resolvedPoses;
	}

	void ResolveLatePoses(std::span<const Api::LatePoseContribution> a_registered)
	{
		resolvedPoses.clear();
		for (const auto& contribution : a_registered)
			resolvedPoses.push_back({ std::string(contribution.name), contribution.root });
	}

	std::span<const LatePoseEntry> LatePoses() { return resolvedPoses; }
}
