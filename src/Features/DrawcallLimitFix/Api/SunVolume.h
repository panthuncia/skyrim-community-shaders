#pragma once

// The sun's cull volumes as DCLF's own views have them this frame: the cascades BuildDraws tests and the full-frustum entry planes.
// Core owns the test, which mirrors BuildDrawsCS. One contribution only.
// Without it no cascade is latched, so passes with the sun's bits draw unshadowed.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/SunVolume.h.
	class SunCascadeSink;
	class SunEntryPlanesSink;

	/**
	 * @brief The sun volume contribution.
	 *
	 * - cascades: render thread, once per colour commit. Empty when the sun did not set up its views this frame.
	 * - entryPlanes: puts the planes only if they belong to scene frame a_frame or the one before. Returns whether it put them.
	 */
	struct SunVolumeContribution
	{
		std::string_view name;
		void (*cascades)(SunCascadeSink& a_sink) = nullptr;
		bool (*entryPlanes)(SunEntryPlanesSink& a_sink, std::uint32_t a_frame) = nullptr;
	};
}
