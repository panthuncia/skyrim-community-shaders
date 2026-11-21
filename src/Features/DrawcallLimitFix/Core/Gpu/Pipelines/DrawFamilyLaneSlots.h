#pragma once

// The pipeline lane's part of each draw family's fixed slot. The rest is in Core/Render/DrawFamilies/FamilySlots.h.
// It lives here, below the render layer, so ShaderPrograms and DrawPipelines can read it.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace DCLF
{
	inline constexpr std::size_t kDrawFamilyLanes = static_cast<std::size_t>(Api::DrawFamily::Count);

	/** @brief A draw family's lane slot. */
	struct DrawFamilyLaneSlot
	{
		Api::DrawFamily family;
		std::uint32_t forwardFlags;  // 0 for no faces
		std::uint64_t stageKeyBit;  // 36 up; 37 and 39 are the program families'
		bool pulled;
		std::uint64_t forwardProgramId;  // ~0 - family
	};
	inline constexpr std::array<DrawFamilyLaneSlot, kDrawFamilyLanes> kDrawFamilyLaneSlots = { {
		{ Api::DrawFamily::TreeInstances, 2u, 1ull << 36, true, ~0ull - 0 },
		{ Api::DrawFamily::Grass, 0u, 1ull << 38, true, ~0ull - 1 },
	} };
	static_assert(kDrawFamilyLaneSlots[static_cast<std::size_t>(Api::DrawFamily::TreeInstances)].family == Api::DrawFamily::TreeInstances &&
	                  kDrawFamilyLaneSlots[static_cast<std::size_t>(Api::DrawFamily::Grass)].family == Api::DrawFamily::Grass,
		"kDrawFamilyLaneSlots is by family");

	constexpr const DrawFamilyLaneSlot& DrawFamilyLaneSlotOf(Api::DrawFamily a_family) { return kDrawFamilyLaneSlots[static_cast<std::size_t>(a_family)]; }
}
