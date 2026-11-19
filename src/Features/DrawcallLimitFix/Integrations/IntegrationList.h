#pragma once

// The CS features DCLF supports, in install order.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <span>
#include <string>
#include <vector>

namespace DCLF::Integrations
{
	/** @brief Any thread. Every integration, in order. */
	std::span<const Api::Feature> List();

	/** @brief Any thread. One "name: status" line per integration. */
	std::vector<std::string> StatusLines();
}
