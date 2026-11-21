#pragma once

// Switch nodes (dclf-architecture.md, "Switch nodes"): DCLF draws the leaves under an NiSwitchNode on frames
// when every switch on their path selects their branch. The feature registers the parent contribution.
// Core keeps the selections themselves, since the primary cull stands in for whole roots and follows every switch below them.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::SwitchNodes
{
	Api::Feature Feature();
}
