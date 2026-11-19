#pragma once

// A world fact outside the toggles that classification reads, such as whether Terrain Blending is holding terrain back.
// Core samples every input once per frame, and a scene pass takes them at its start so it classifies by one answer throughout.
// A change drops the classification caches.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/** @brief A classify input. One needed by name reads false until every install has run. */
	struct ClassifyInputId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A classify input.
	 * sample may run on any thread. It must be cheap and read only the feature's own state, never the engine.
	 */
	struct ClassifyInputContribution
	{
		std::string_view name;
		bool (*sample)() = nullptr;
	};
}
