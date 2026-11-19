#pragma once

// Core's fixed slot per draw family, reserved whichever feature takes it.
// A new family needs its enum value, its slot here, its pipeline lane slot (DrawFamilyLaneSlots.h) and the feature's
// registration. The family's GPU layout belongs to its feature. Self-contained so features can include it.

// volk must come before every Vulkan header, so SceneBuffers.h goes first.
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawFamilyLaneSlots.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace DCLF::Draws
{
	/** @brief A family's slot: the forward key flags of a trees view's pipeline for it. */
	struct FamilySlot
	{
		Api::DrawFamily family;
		std::uint32_t forwardFlags;
	};
	inline constexpr std::array<FamilySlot, Extensions::kDrawFamilies> kFamilySlots = { {
		{ Api::DrawFamily::TreeInstances, DrawFamilyLaneSlotOf(Api::DrawFamily::TreeInstances).forwardFlags },
		{ Api::DrawFamily::Grass, DrawFamilyLaneSlotOf(Api::DrawFamily::Grass).forwardFlags },
	} };
	static_assert(kFamilySlots[static_cast<std::size_t>(Api::DrawFamily::TreeInstances)].family == Api::DrawFamily::TreeInstances &&
	                  kFamilySlots[static_cast<std::size_t>(Api::DrawFamily::Grass)].family == Api::DrawFamily::Grass,
		"kFamilySlots is by family");

	inline const FamilySlot& FamilySlotOf(Api::DrawFamily a_family) { return kFamilySlots[static_cast<std::size_t>(a_family)]; }

	/** @brief Whether a_family is taken in a_scene and its scene resources exist. Without them it has no decision, draws or lists. */
	inline bool FamilySetUp(const SceneBuffers& a_scene, Api::DrawFamily a_family)
	{
		const auto& entry = Extensions::DrawFamilyOf(a_family);
		return entry.taken && Extensions::SceneResourcesStateOf(a_scene, entry.contribution.resources) != nullptr;
	}

	/** @brief Any thread: a_family's pipelines as the lane published them, null before the first. */
	inline std::shared_ptr<const FamilyPipelines> FamilyPipelinesOf(const SceneBuffers& a_scene, Api::DrawFamily a_family)
	{
		return a_scene.familyPipelines[static_cast<std::size_t>(a_family)].load(std::memory_order_acquire);
	}
}
