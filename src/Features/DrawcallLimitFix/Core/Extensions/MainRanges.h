#pragma once

// Core's side of Api/MainRanges.h: the second pass's ranges in draw order, resolved once at install, plus held groups and the near twin.
// A slot nobody registers is never drawn. Each slot's arguments, count word and capacity are Core's, in GpuLayouts.h.

#include "Features/DrawcallLimitFix/Api/MainRanges.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Extensions
{
	inline constexpr std::size_t kMainRangeSlots = static_cast<std::size_t>(Api::MainRangeSlot::Count);

	/** @brief A range's entry: its slot, its full nvperf range name, and whether it draws a depth prepass. */
	struct MainRangeEntry
	{
		std::uint32_t slot = 0;
		std::string colourRange;
		bool depthPrepass = false;
		Api::ObjectClass objectClass = Api::ObjectClass::Count;
	};

	struct MainRangeTable
	{
		std::vector<MainRangeEntry> drawn;
		bool anyDepthPrepass = false;
	};

	/** @brief Main thread, at the end of InstallFeatures. */
	void ResolveMainRanges(const std::array<Api::MainRangeContribution, kMainRangeSlots>& a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const MainRangeTable& MainRanges();

	/** @brief Most held groups: one per appended slot. */
	inline constexpr std::size_t kHeldGroupSlots = kMainRangeSlots - Api::kFixedMainRanges;
	/** @brief A registered held group. group is its slot's terrain group bits: 1 for Appended0, 2 for Appended1. */
	struct HeldGroupEntry
	{
		Api::ClassifyInputId heldWhile;
		std::uint32_t group = 0;
		std::uint64_t propertyFlags = 0;
		Api::ToggleId toggle;
	};
	/** @brief Any thread, after InstallFeatures. In registration order, where the first whose flags match takes the object. */
	std::span<const HeldGroupEntry> HeldGroups();

	/** @brief The registered near twin, with its strings owned and its NotReady texts built. A null target means none. */
	struct NearTwinEntry
	{
		std::string name, debugName, resource;
		std::string cannotDescribe, cannotImport;
		decltype(Api::NearTwinContribution::target) target = nullptr;
		decltype(Api::NearTwinContribution::nearTest) nearTest = nullptr;
		decltype(Api::NearTwinContribution::graph) graph = nullptr;
	};
	/** @brief Any thread, after InstallFeatures. */
	const NearTwinEntry& NearTwin();
}
