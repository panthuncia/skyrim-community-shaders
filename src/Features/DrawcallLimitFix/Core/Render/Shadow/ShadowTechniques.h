#pragma once

#include <cstdint>
#include <vector>

namespace DCLF::Draws
{
	/** @brief The Utility techniques DCLF's shadow, occlusion and refraction pipelines can draw with, and how many came from each. */
	struct DerivedUtilityTechniques
	{
		std::vector<std::uint32_t> techniques;  // sorted, each once
		std::uint32_t casterBases = 0;          // caster techniques before the mode bits
		std::uint32_t shadow = 0;               // shadow view techniques
		std::uint32_t occlusion = 0;            // occlusion map techniques
	};

	/**
	 * @brief Any thread, after the features installed. Every technique a view pipeline can ask for, in closed form: each caster technique
	 * with each shadow mode's bits, through ViewPipelineTechnique, each registered occlusion map's techniques, and the refraction technique.
	 * The engine never loads most of them, so no Community Shaders compile task asks for them.
	 */
	DerivedUtilityTechniques EnumerateDerivedUtilityTechniques();
}
