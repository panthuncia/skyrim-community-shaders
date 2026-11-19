// CS_DCLF_PERSISTENT_PARITY check of the grass mirror against the capture's live set.
// An event not yet drained makes a shape differ, so a difference only counts if the next drain does not resolve it.

#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsView.h"

#include <DirectXPackedVector.h>

#include <cmath>
#include <limits>
#include <mutex>

namespace DCLF::Features::Grass
{
	namespace
	{
		struct LiveGroup
		{
			std::uint32_t count = 0;
			std::uint64_t seq = 0;
		};

		struct Live
		{
			ankerl::unordered_dense::map<std::uint32_t, LiveGroup> groups;
			std::uint32_t count = 0;
		};

		// Written by the sink threads under the capture's lock, so in seq order.
		std::mutex liveLock;
		ankerl::unordered_dense::map<const void*, Live> live;

		void OnCaptured(const GrassCaptured& a_event)
		{
			std::scoped_lock lock(liveLock);
			const auto it = live.find(a_event.shape);
			if (a_event.kind == GrassCaptured::Kind::Remove) {
				if (it != live.end())
					live.erase(it);
				return;
			}
			if (it == live.end() && a_event.kind != GrassCaptured::Kind::Add)
				return;
			auto& entry = it != live.end() ? it->second : live[a_event.shape];
			if (const auto group = entry.groups.find(a_event.group); group != entry.groups.end()) {
				entry.count -= group->second.count;
				entry.groups.erase(group);
			}
			if (a_event.kind == GrassCaptured::Kind::Add) {
				entry.groups[a_event.group] = { a_event.count, a_event.seq };
				entry.count += a_event.count;
			}
			if (entry.groups.empty())
				live.erase(a_event.shape);
		}

		/** @brief Places records as the cull does. If grass culled as far is near here, the cull is wrong, not the placement. */
		void CheckPlacement(const Mirror& a_mirror, GrassParityState& a_stats)
		{
			a_stats.withinDistance = 0;
			a_stats.nearest = -1.0f;
			const auto* draws = Diagnostics::DrawsView::Find();
			const auto* tables = draws && draws->scene ? TablesOf(*draws->scene) : nullptr;
			if (!tables || tables->row.cull.maxDistSq <= 0.0f)
				return;
			using DirectX::PackedVector::XMConvertHalfToFloat;
			const auto& eye = tables->row.eye;
			const float maxDistSq = tables->row.cull.maxDistSq;
			float nearestSq = std::numeric_limits<float>::max();
			for (std::uint32_t slot = 0; slot < a_mirror.ShapeSlots(); ++slot) {
				const auto* records = a_mirror.SlotInstances(slot);
				if (!records)
					continue;
				const auto& origin = a_mirror.SlotRow(slot).origin;
				for (const auto& record : *records) {
					const float dx = origin[0] + XMConvertHalfToFloat(record.shorts[0]) - eye[0];
					const float dy = origin[1] + XMConvertHalfToFloat(record.shorts[1]) - eye[1];
					const float dz = origin[2] + XMConvertHalfToFloat(record.shorts[2]) - eye[2];
					const float distSq = dx * dx + dy * dy + dz * dz;
					nearestSq = std::min(nearestSq, distSq);
					a_stats.withinDistance += distSq <= maxDistSq ? 1u : 0u;
				}
			}
			a_stats.maxDistance = std::sqrt(maxDistSq);
			if (nearestSq != std::numeric_limits<float>::max())
				a_stats.nearest = std::sqrt(nearestSq);
		}

		void CheckParity(const Mirror& a_mirror)
		{
			auto& stats = ParityState();
			auto& suspects = stats.suspects;
			const auto& shapes = a_mirror.MirroredShapes();
			++stats.checks;
			std::uint64_t liveInstances = 0;
			std::scoped_lock lock(liveLock);
			for (const auto& [shape, entry] : live) {
				++stats.shapes;
				liveInstances += entry.count;
				const auto it = shapes.find(shape);
				if (it == shapes.end())
					suspects[shape] = fmt::format("a captured shape ({} records) not mirrored", entry.count);
				else if (it->second.records != entry.count)
					suspects[shape] = fmt::format("a shape mirrored with {} records, captured with {}", it->second.records, entry.count);
			}
			// Mirrored records the capture no longer holds mean a missed removal.
			for (const auto& [shape, mirrored] : shapes)
				if (mirrored.records && !live.contains(shape))
					suspects[shape] = fmt::format("a mirrored shape ({} records) no longer captured", mirrored.records);
			stats.liveInstances = liveInstances;
			stats.mirroredInstances = a_mirror.InstanceCount();
			CheckPlacement(a_mirror, stats);
		}

		void OnEventDrained(const GrassEventDrained& a_event)
		{
			auto& stats = ParityState();
			if (!stats.suspects.empty() && stats.suspects.erase(a_event.shape))
				++stats.late;
		}

		void OnDrained(const GrassDrained& a_event)
		{
			// No event since the check means the mirror missed or misapplied one.
			auto& stats = ParityState();
			for (auto& [shape, text] : stats.suspects) {
				if (text.starts_with("a captured"))
					++stats.missing;
				else if (text.starts_with("a mirrored"))
					++stats.stale;
				else
					++stats.differ;
				if (stats.first.empty())
					stats.first = std::move(text);
			}
			stats.suspects.clear();
			if (Diagnostics::ParityDue(a_event.frame) && std::exchange(stats.frame, a_event.frame) != a_event.frame)
				CheckParity(a_event.mirror);
		}
	}

	GrassParityState& ParityState()
	{
		static GrassParityState state;
		return state;
	}

	void SubscribeParity()
	{
		Diagnostics::Subscribe<GrassCaptured>(OnCaptured);
		Diagnostics::Subscribe<GrassEventDrained>(OnEventDrained);
		Diagnostics::Subscribe<GrassDrained>(OnDrained);
	}
}
