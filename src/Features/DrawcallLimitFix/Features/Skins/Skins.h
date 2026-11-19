#pragma once

// Skins (CS_DCLF_SKINNED, CS_DCLF_SKIN_PARTITIONS): which skinned shapes are DCLF's. Registers the switches, toggles and object class
// contributions, and the report line. Core keeps the path a skinned shape takes once its class is on, shared with every feature that
// reads skins. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Skins
{
	Api::Feature Feature();
	void RegisterReport(Api::InstallContext& a_context);
}
