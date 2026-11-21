#pragma once

// Main ranges: a feature's ordered range in the colour segment's second pass, drawn after every opaque draw.
// The slots and their count words are BuildDrawsCS's compile-time layout in GpuLayouts.h. A registration picks the slot's order and name.
// Held groups fill an appended slot with objects a CS feature replays after the opaque pass. A near twin draws those again into a second depth target.

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class ExternalTextureResource;
}

namespace DCLF::Api
{
	/**
	 * @brief Core's fixed slot per range. A fixed slot keeps the feature's order through per-object ordinals.
	 * An appended slot's draws come in cull order and reuse phase 2's range once the depth segment is done with it.
	 */
	enum class MainRangeSlot : std::uint8_t
	{
		Fixed0,     // decal group 1
		Fixed1,     // decal group 2
		Fixed2,     // decal group 3
		Appended0,  // terrain group 1
		Appended1,  // terrain group 2
		Count,
	};
	inline constexpr std::size_t kFixedMainRanges = static_cast<std::size_t>(MainRangeSlot::Appended0);

	/**
	 * @brief A main range, drawn in the second pass by ascending order under the nvperf range "cs.dclf.colour.<name>".
	 *
	 * - depthPrepass: fixed slots only. Draws depth first, so a host fragment under an opaque texel fails the colour pass's EQUAL test.
	 * - objectClass: Count for a held group's range.
	 */
	struct MainRangeContribution
	{
		MainRangeSlot slot = MainRangeSlot::Count;
		std::string_view name;
		std::uint8_t order = 0;
		bool depthPrepass = false;
		ObjectClass objectClass = ObjectClass::Count;
	};

	/**
	 * @brief Objects a CS feature takes out of the opaque pass and replays after it, as Terrain Blending does. DCLF draws them the same way.
	 *
	 * - heldWhile: outside this input they are ordinary objects.
	 * - slot: Appended0 or Appended1. Registered as a main range too.
	 * - propertyFlags: the first group in registration order whose flags match takes the object.
	 * - Decals and layers in a held group are always Ineligible::Held.
	 */
	struct HeldGroupContribution
	{
		ClassifyInputId heldWhile;
		MainRangeSlot slot = MainRangeSlot::Count;
		std::string_view name;
		std::uint8_t order = 0;
		std::uint64_t propertyFlags = 0;
	};

	/**
	 * @brief Draws Appended0's near objects a second time in the depth segment, into the feature's own depth target. One registration.
	 *
	 * - target: render thread. The engine depth texture to import, or null when the feature is not loaded.
	 * - nearTest: render thread, per depth commit. Writes eye xyz and distance w. False means no test this frame.
	 * - graph: optional. The graph's own resource for the target, so the Z-prepass declares it instead of importing it again.
	 */
	struct NearTwinContribution
	{
		std::string_view name;
		std::string_view debugName;
		std::string_view resource;
		void* (*target)() = nullptr;
		bool (*nearTest)(float (&a_eyeDistance)[4]) = nullptr;
		std::shared_ptr<org::ExternalTextureResource> (*graph)() = nullptr;
	};
}
