#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Features/DrawcallLimitFix/Api/EntryRanges.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"

namespace DCLF::LodSegments
{
	// Object LOD segments (AE 1.6.1170). A LOD BSSubIndexTriShape has one segment per cell of its terrain node.
	// The terrain manager disables the segments of attached cells so loaded cells and LOD do not overlap.
	// FUN_140e31160 disables a segment and FUN_140e31130 enables it. Both set the shape's dirty byte.
	// When dirty, the draw (FUN_1414f2ad0) first merges consecutive enabled segments into runs (FUN_140e31180),
	// then issues one DrawIndexed per run. A non-segmented shape draws whole.
	struct Segment
	{
		std::uint32_t firstIndex;    // 00
		std::uint32_t triangles;     // 04
		std::uint8_t enabled;        // 08
		std::uint8_t pad09[3];       // 09
		std::uint32_t runTriangles;  // 0C: the whole run's triangles, set on its first segment
		std::uint8_t runStart;       // 10
		std::uint8_t pad11[3];       // 11
	};
	static_assert(sizeof(Segment) == 0x14);

	inline constexpr std::size_t kTriangleCount = 0x158;  // uint16
	inline constexpr std::size_t kSegments = 0x160;       // Segment*
	inline constexpr std::size_t kSegmentCount = 0x168;   // uint32
	inline constexpr std::size_t kDirty = 0x170;          // the runs need rebuilding
	inline constexpr std::size_t kNonSegmented = 0x171;   // drawn whole

	template <class T>
	const T& At(const void* a_object, std::size_t a_offset)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(a_object) + a_offset);
	}

	/**
	 * @brief The index ranges the engine would draw for a_shape now. When dirty we read the enabled segments,
	 * which cover the same triangles the rebuilt runs will.
	 */
	inline void DrawnRanges(const void* a_shape, std::vector<Api::EntryRange>& a_out)
	{
		EngineReadWindow::Touch("LodSegments::DrawnRanges");
		a_out.clear();
		auto add = [&](std::uint32_t a_first, std::uint32_t a_triangles, const void* a_key) {
			if (!a_triangles)
				return;
			const std::uint32_t count = a_triangles * 3;
			if (!a_out.empty() && a_out.back().firstIndex + a_out.back().indexCount == a_first)
				a_out.back().indexCount += count;
			else
				a_out.push_back({ a_first, count, a_key });
		};
		if (At<std::uint8_t>(a_shape, kNonSegmented)) {
			add(0, At<std::uint16_t>(a_shape, kTriangleCount), nullptr);
			return;
		}
		const auto* segments = At<const Segment*>(a_shape, kSegments);
		if (!segments)
			return;
		const std::uint32_t count = At<std::uint32_t>(a_shape, kSegmentCount);
		const bool dirty = At<std::uint8_t>(a_shape, kDirty) != 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto& segment = segments[i];
			if (dirty) {
				if (segment.enabled)
					add(segment.firstIndex, segment.triangles, &segment);
			} else if (segment.runStart) {
				add(segment.firstIndex, segment.runTriangles, &segment);
			}
		}
	}

	/** @brief True when a_ranges are the whole index list, so the shape is drawn as its own geometry slot. */
	inline bool Whole(const void* a_shape, const std::vector<Api::EntryRange>& a_ranges)
	{
		return Api::EntryRangesWhole(At<std::uint16_t>(a_shape, kTriangleCount), a_ranges);
	}
}
