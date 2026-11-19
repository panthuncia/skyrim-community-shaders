#pragma once

// Extended Translucency's SetupGeometry hook sets the permutation's ExtraFeatureDescriptor to a blended geometry's material model.
// DCLF reproduces that with the AnisotropicAlphaMaterial geometry extra, captured with the geometry's record,
// and the Lighting key's spare bits, so each model gets its own pipeline. Non-zero only for blended geometry, such as NPC hairlines.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Integrations::ExtendedTranslucency
{
	Api::Feature Integration();
}
