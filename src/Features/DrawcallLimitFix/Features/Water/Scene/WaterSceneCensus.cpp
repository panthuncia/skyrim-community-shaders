// The water census: every water shape DCLF tracks, how the port classified it, and Core's verdict now.
// The port's derive queues each classification. The census component takes the queue at the end of each walk, files each tracked
// shape by where it hangs (WaterLOD, the water system's root, or a cell's category nodes) and reads Core's verdict back. A report
// line, at the report schedule, counts them. Each shape is also logged once.

#include "Features/DrawcallLimitFix/Features/Water/Scene/WaterScene.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DCLF::Features::Water::WaterScene
{
	namespace
	{
		enum class Placement : std::uint8_t
		{
			Tile,          // under WaterLOD without the LOD bit: Unified Water's tiles
			Lod,           // under WaterLOD with the LOD bit: vanilla LOD water
			Procedural,    // under the water system's root
			CellOrStatic,  // under a cell's category nodes: cell water and water inside statics
			Count
		};
		constexpr std::array<const char*, static_cast<std::size_t>(Placement::Count)> kPlacementNames{ "tile", "LOD", "procedural", "cell or static" };
		constexpr std::array<const char*, static_cast<std::size_t>(Classification::Refusal::Count)> kRefusalNames{ "plain", "wading", "stencil bit",
			"no Light Limit Fix", "skinned", "not captured" };

		// Classifications queued past this are dropped and counted. A full evaluation of a large scene stays well below it.
		constexpr std::size_t kMaxPending = 1u << 16;
		// Shapes logged one by one in a session.
		constexpr std::uint32_t kMaxLogged = 1024;

		struct Entry
		{
			Classification classification;
			Placement placement = Placement::CellOrStatic;
			std::uint8_t reason = 0;  // Core's verdict now (Ineligible)
		};

		// The derive queues from the scene lane's pool, the component drains on the scene lane, the report reads on the render thread.
		struct CensusState
		{
			std::mutex mutex;
			std::vector<std::pair<const void*, Classification>> pending;
			std::unordered_map<const void*, Entry> entries;
			std::uint64_t dropped = 0;
			std::uint32_t logged = 0;
		};

		CensusState& Census()
		{
			static CensusState state;
			return state;
		}

		/** @brief Where a shape hangs, from the mirror's parents. a_reference gets the nearest reference's node name. */
		Placement PlacementOf(const SceneMirror& a_mirror, const void* a_geometry, const Classification& a_classification, const Roots& a_roots,
			const char*& a_reference)
		{
			constexpr std::uint32_t kMaxDepth = 64;
			const void* at = a_mirror.Parent(a_geometry);
			for (std::uint32_t depth = 0; at && depth < kMaxDepth; ++depth) {
				if (at == a_roots.waterLod)
					return (a_classification.attributes & Classification::kLodBit) ? Placement::Lod : Placement::Tile;
				if (at == a_roots.waterSystem)
					return Placement::Procedural;
				const auto* node = a_mirror.Node(at);
				if (!node)
					break;
				if (node->userData && !a_reference)
					a_reference = node->name;
				at = node->parent;
			}
			return Placement::CellOrStatic;
		}

		void LogShape(const SceneMirror& a_mirror, const void* a_geometry, const Entry& a_entry, const char* a_reference)
		{
			const auto* node = a_mirror.Node(a_geometry);
			const auto* parent = node ? a_mirror.Node(node->parent) : nullptr;
			const auto& c = a_entry.classification;
			logger::info("[DCLF] water scene: '{}' under '{}' (reference '{}', geometry {}): {}, flags {:#x}, stencil bit {:#x}, surface {:#x} hint {}, "
						 "stencil pass {:#x}, {}",
				node && node->name ? node->name : "", parent && parent->name ? parent->name : "", a_reference ? a_reference : "", fmt::ptr(a_geometry),
				kPlacementNames[static_cast<std::size_t>(a_entry.placement)], c.waterFlags, c.stencilBit, c.surfaceDescriptor, c.surfaceHint, c.stencilPass,
				kRefusalNames[static_cast<std::size_t>(c.refusal)]);
		}

		std::shared_ptr<void> MakeCensusComponent() { return std::make_shared<int>(0); }

		void WalkEnded(const Api::CoordinatorComponentContext& a_context)
		{
			auto& census = Census();
			const Roots roots = ListedRoots();
			const auto& mirror = a_context.Mirror();
			std::lock_guard lock(census.mutex);
			for (const auto& [geometry, classification] : census.pending) {
				if (!a_context.TrackedStateOf(geometry).tracked)
					continue;
				const char* reference = nullptr;
				Entry entry{ classification, PlacementOf(mirror, geometry, classification, roots, reference) };
				const bool inserted = census.entries.insert_or_assign(geometry, entry).second;
				if (inserted && census.logged < kMaxLogged) {
					++census.logged;
					LogShape(mirror, geometry, entry, reference);
				}
			}
			census.pending.clear();
			// Core's verdicts now, and the shapes it let go without telling the census.
			for (auto it = census.entries.begin(); it != census.entries.end();) {
				const auto state = a_context.TrackedStateOf(it->first);
				if (!state.tracked) {
					it = census.entries.erase(it);
					continue;
				}
				it->second.reason = state.reason;
				++it;
			}
		}

		void Forgot(const Api::CoordinatorComponentContext&, const void* a_entry)
		{
			auto& census = Census();
			std::lock_guard lock(census.mutex);
			census.entries.erase(a_entry);
		}

		void Lines(std::uint32_t a_frame)
		{
			if (!Reports::Due(a_frame))
				return;
			std::array<std::uint32_t, static_cast<std::size_t>(Placement::Count)> placements{};
			std::array<std::uint32_t, static_cast<std::size_t>(Classification::Refusal::Count)> refusals{};
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> reasons{};
			std::uint32_t wading = 0, hint13 = 0, vertexColors = 0, flowmap = 0;
			std::size_t shapes = 0;
			std::uint64_t dropped = 0;
			{
				auto& census = Census();
				std::lock_guard lock(census.mutex);
				shapes = census.entries.size();
				dropped = census.dropped;
				for (const auto& [geometry, entry] : census.entries) {
					const auto& c = entry.classification;
					++placements[static_cast<std::size_t>(entry.placement)];
					++refusals[static_cast<std::size_t>(c.refusal)];
					if (entry.reason < reasons.size())
						++reasons[entry.reason];
					wading += (c.attributes & Classification::kWading) ? 1u : 0u;
					hint13 += (c.attributes & Classification::kHint13) ? 1u : 0u;
					vertexColors += (c.attributes & Classification::kVertexColors) ? 1u : 0u;
					flowmap += (c.attributes & Classification::kFlowmap) ? 1u : 0u;
				}
			}
			std::string verdicts;
			for (std::size_t r = 0; r < reasons.size(); ++r)
				if (reasons[r])
					verdicts += fmt::format("{}{} {}", verdicts.empty() ? "" : ", ", kIneligibleNames[r], reasons[r]);
			const auto pools = CurrentPoolSizes();
			logger::info("[DCLF] water scene: {} shapes tracked: {} tiles, {} LOD, {} procedural, {} cell or static; wading {}, hint 13 {}, vertex colours {}, "
						 "flowmap {}; plain {} (claimed from phase 2), refused: wading {}, stencil bit {}, no Light Limit Fix {}, skinned {}, not captured {}; "
						 "verdicts now: {}; camera under water {}; material pool {} rows for {} members{}",
				shapes, placements[0], placements[1], placements[2], placements[3], wading, hint13, vertexColors, flowmap, refusals[0], refusals[1], refusals[2],
				refusals[3], refusals[4], refusals[5], verdicts.empty() ? "none" : verdicts, pools.cameraUnderwater ? "yes" : "no", pools.materialRows,
				pools.materialMembers, dropped ? fmt::format("; {} classifications dropped <- WATER CENSUS", dropped) : std::string{});
		}
	}

	void NoteClassified(const void* a_geometry, const Classification& a_classification)
	{
		auto& census = Census();
		std::lock_guard lock(census.mutex);
		if (census.pending.size() >= kMaxPending) {
			++census.dropped;
			return;
		}
		census.pending.emplace_back(a_geometry, a_classification);
	}

	void RegisterCensus(Api::InstallContext& a_context)
	{
		a_context.RegisterCoordinatorComponent({ .name = "water-census", .make = &MakeCensusComponent, .walk = { .ended = &WalkEnded }, .retirement = { .forgot = &Forgot } });
		a_context.RegisterReportLines({ .name = "water scene", .point = Api::ReportPoint::RenderThread, .lines = &Lines });
	}
}
