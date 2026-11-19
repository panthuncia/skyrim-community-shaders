#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	struct GpuShadowVolume;
}

namespace DCLF::Diagnostics
{
	/**
	 * @brief Shadow lights for dclf:actor-probe. The next CPU colour cull logs each local shadow light's verdict for the named objects.
	 * A light missing from an object's mask is one Light Limit Fix ignores for its draw, so the object goes unlit by it.
	 */
	namespace ShadowMaskProbe
	{
		/** @brief The objects to report on the next colour cull that meets them. Scene lane. */
		void Arm(std::vector<std::pair<std::uint32_t, std::string>> a_objects);
		/** @brief Whether an armed probe is waiting. A relaxed read for the cull's hot loop. Any thread. */
		bool Armed();
		/**
		 * @brief Logs a_object's mask decision once if the probe names it. a_rejects per volume is 0 taken, 1 sphere, 2 plane,
		 * 3 strict gate, 4 landscape, 5 bit already taken.
		 */
		void Note(std::uint32_t a_object, const float a_bound[4], float a_margin, std::uint32_t a_mask, const Draws::GpuShadowVolume* a_volumes, std::uint32_t a_count,
			const std::uint8_t* a_rejects);
	}
}
