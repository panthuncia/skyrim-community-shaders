#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief VRAM attribution's view of BasicRHI and ORG (org-full-port.md P4.7): the device's memory through BasicRHI's memory budget
 * (VK_EXT_memory_budget, P0.7), and ORG's allocator by owner label (P0.13), in place of DXVK's heap reports.
 *
 * CS_ORG_VRAM_SOURCE=org selects it for the heaps of Vram::Current and for texture streaming's budget. DXVK's snapshot still lists
 * D3D11's resources while DXVK is loaded, and DXVK's own totals still go into the reconciliation. The default stays DXVK's reports.
 *
 * Without the render graph every call returns nothing.
 */
namespace Vram::Org
{
	/** @brief Whether CS_ORG_VRAM_SOURCE=org: the heaps come from BasicRHI. Read once. */
	bool Selected();

	/** @brief One segment group of the device: video memory, or system memory. */
	struct Segment
	{
		bool valid = false;
		std::uint64_t capacity = 0;         // the size of the group's heaps
		std::uint64_t usage = 0;            // the process's use, as the driver reports it
		std::uint64_t budget = 0;           // the driver's budget for this process
		bool usageAtBudget = false;         // AMD reports the use as the budget at most: past it the real use cannot be read
		std::uint64_t allocatorBlocks = 0;  // ORG's allocator: the memory in its blocks
		std::uint64_t allocatorUsed = 0;    // of that, what its allocations use
	};

	/** @brief The device's video memory (local) and system memory (system). */
	struct Heaps
	{
		Segment local;
		Segment system;
	};

	/** @brief The device's two segment groups. Any thread. Both invalid without the render graph. */
	Heaps ReadHeaps();

	/** @brief One owner label's live allocations in ORG's allocator. */
	struct Owner
	{
		std::string label;  // empty for allocations nobody labelled: ORG's own
		std::uint32_t count = 0;
		std::uint64_t videoBytes = 0;
		std::uint64_t systemBytes = 0;
		std::uint64_t spilledBytes = 0;  // of systemBytes, what asked for video memory and was placed in system memory
	};

	/** @brief Every owner label with live allocations. Any thread. Empty without the render graph. */
	std::vector<Owner> ReadOwners();

	/** @brief The bytes of every allocation that asked for video memory and was placed in system memory. Any thread. */
	std::uint64_t SpilledBytes();

	/**
	 * @brief The owner path VRAM attribution files a label under. Labels are owner paths already ("Game/Textures/landscape", the
	 * same as DXVK's owner scopes give), so they map to the same categories; an empty label is "ORG/Allocator".
	 */
	std::string OwnerPath(std::string_view a_label);
}
