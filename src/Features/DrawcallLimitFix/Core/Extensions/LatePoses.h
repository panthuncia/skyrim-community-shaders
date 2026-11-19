#pragma once

// Core's side of Api/LatePoses.h: the contributions, resolved once at install.

#include "Features/DrawcallLimitFix/Api/LatePoses.h"

#include <span>
#include <string>
#include <vector>

namespace DCLF::Extensions
{
	/** @brief A registered late pose, with its name owned. */
	struct LatePoseEntry
	{
		std::string name;
		decltype(Api::LatePoseContribution::root) root = nullptr;
	};

	/** @brief Main thread, once, at the end of InstallFeatures. */
	void ResolveLatePoses(std::span<const Api::LatePoseContribution> a_registered);
	/** @brief Any thread, after InstallFeatures. In registration order. */
	std::span<const LatePoseEntry> LatePoses();
}
