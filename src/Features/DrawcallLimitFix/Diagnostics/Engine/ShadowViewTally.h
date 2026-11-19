#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief Shadow view parity. DCLF's views against the engine's captures at each view's draw, cascade cull and full-frustum process.
	 * One tally per view kind, reported every 300 frames by its source's observers.
	 */
	struct ViewTally
	{
		std::uint64_t views = 0, viewProj = 0, eye = 0, viewport = 0, casters = 0;  // differing views, by field
		std::uint64_t cascades = 0, cascadePlanes = 0, cascadeCasters = 0;          // the sun bits' cascades
		std::uint64_t fullFrusta = 0, fullFrustumPlanes = 0;                        // the full-frustum processes
		std::uint64_t missing = 0;                                                   // a view or cascade without DCLF's values
		float largest = 0.0f;
		std::string first;
		/** @brief Notes a difference, keeping the largest and the first of up to four kinds. */
		void Note(const char* a_what, float a_size, std::string a_detail);
	};

	enum class ShadowView : std::uint8_t
	{
		Sun,
		Light,
		Focus,
	};

	/** @brief The tally for a_view's kind. Render thread. */
	ViewTally& ShadowViewTally(ShadowView a_view);
}
