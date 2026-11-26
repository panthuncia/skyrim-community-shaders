#pragma once

// Linear Lighting: while it is on, its upload hooks rewrite colours the engine's Setup functions wrote, from sRGB to its working
// space. DCLF's rows never pass through those hooks, so the conversions are ported here: an After material contributor for
// Lighting's SpecularColor (LightingMaterialUpload), and the working space the Effect port and the shading rows convert with.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Extensions
{
	struct MaterialContributorSamples;
}

namespace DCLF::Integrations::LinearLighting
{
	/** @brief The working space colours convert to while Linear Lighting is on. */
	struct WorkingSpace
	{
		bool acescg = false;
	};

	/** @brief Any thread. The working space now, or null while Linear Lighting is off. */
	const WorkingSpace* LiveWorkingSpace();
	/** @brief Any thread. The working space a frame's material samples hold, or null while Linear Lighting was off. */
	const WorkingSpace* FrameWorkingSpace(const Extensions::MaterialContributorSamples& a_samples);
	/** @brief Pure. Converts an sRGB colour to a_space in place. */
	void ToWorking(const WorkingSpace& a_space, float* a_rgb);

	Api::Feature Integration();
}
