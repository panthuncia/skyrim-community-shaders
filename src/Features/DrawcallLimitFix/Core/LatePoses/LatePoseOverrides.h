#pragma once

// The late poses: scene graph parts a plugin hides after the shadow maps by shrinking nodes. This module finds those nodes and
// turns them into rows for a second copy of the frame's placements and palettes, which the epochs from the depth commit on read.
// Nothing here waits, and nothing reads the engine off the render thread. It stands in for a view-mask API and goes when that comes.
//
// At the depth commit the roots are searched for nodes far smaller than their parent, which confirms this frame's plan and is kept.
// At the next frame's start that search becomes a plain-data plan, which the frame's producer applies to its sampled rows.
// So a hide that starts shows one frame late, while one that stops does not, because the frame's check refuses the plan.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::LatePoses
{
	/** @brief A frame's late pose as plain data, with no engine reference. */
	struct FramePlan
	{
		/** @brief A hidden node's shrink in world space: x' = center + factor * (x - center). */
		struct Scale
		{
			float center[3]{};
			float factor = 1.0f;
		};
		/** @brief A skin with bones under a hidden node. Per bone, its scale's index + 1, or 0 when not hidden. */
		struct Skin
		{
			const void* geometry = nullptr;  // a key, never dereferenced
			std::uint32_t boneRows = 0;      // three per bone, as in the palette block
			std::vector<std::uint16_t> scaleOf;
		};
		/** @brief A geometry without a skin under a hidden node, whose placement row shrinks whole. */
		struct Rigid
		{
			const void* geometry = nullptr;
			std::uint16_t scale = 0;
		};
		std::vector<Scale> scales;
		std::vector<Skin> skins;
		std::vector<Rigid> rigid;
	};

	/** @brief Where the producer keeps a geometry's rows. */
	struct Location
	{
		std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
	};

	/** @brief The late copies' rows that differ from the frame's: whole placement rows and whole palette blocks. */
	struct Rows
	{
		std::vector<BindlessPlacement> placements;
		std::vector<std::uint32_t> placementSlots;  // one per placement row
		std::vector<FrameValues::PaletteRow> palettes;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> paletteRuns;  // first row and row count, in palette order
		void Clear()
		{
			placements.clear();
			placementSlots.clear();
			palettes.clear();
			paletteRuns.clear();
		}
	};

	/** @brief Render thread, at the frame's start. The frame's plan from last frame's search, or null. */
	std::shared_ptr<const FramePlan> PrepareFrame();

	/** @brief Render thread, at the depth commit. Searches the roots for next frame, and returns whether this frame's plan holds now. */
	bool ConfirmFrame();

	/**
	 * @brief Render thread, before Present, for diagnostics. When the depth commit found nothing under a named root, searches again
	 * and counts frames where the plugin hid nodes after the depth commit, which the plan cannot follow.
	 */
	void CheckAtPresent();

	/**
	 * @brief The producer. Applies a_plan to the frame's rows into a_out, cleared first. a_find gives a geometry's rows.
	 * A skin whose palette block is not the plan's size is left out.
	 */
	void BuildRows(const FramePlan& a_plan, const std::function<bool(const void*, Location&)>& a_find, const std::vector<BindlessPlacement>& a_rows,
		const std::vector<FrameValues::PaletteRow>& a_palettes, Rows& a_out);

	/** @brief The report's text since the last call, or empty when no root was named. */
	std::string TakeReport();
}
