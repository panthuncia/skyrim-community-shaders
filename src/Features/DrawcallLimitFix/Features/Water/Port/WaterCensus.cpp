// The water census: where the drawn water hangs in the scene graph. Phase 1 needs every root that holds a drawn water shape,
// because DCLF tracks only the roots its category captures name (dclf-water-indirect.md 3.1).
// Per latched draw it logs once per shape and technique: the parent chain to the root (names and RTTI), the property's flags,
// its stencil bit (unk8C), the pass, the material, and whether it is a Unified Water tile, a water system object or under the
// water system's root. Every few hundred frames it logs the distinct roots that held drawn water. Render thread.

#include "Features/DrawcallLimitFix/Features/Water/Port/WaterParity.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"

#include "Utils/Logging.h"

#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace DCLF::Features::Water::Port::Census
{
	namespace
	{
		constexpr std::uint32_t kSummaryFrames = 600;
		// Shapes logged in full. Past it they are only counted.
		constexpr std::size_t kMaxLoggedShapes = 2048;
		constexpr std::uint32_t kMaxDepth = 64;

		struct RootEntry
		{
			std::string name;  // "<root name> (<RTTI>) / <its child on the path> (<RTTI>)"
			std::set<const void*> shapes;
			std::uint64_t draws = 0;
		};

		struct CensusState
		{
			std::set<std::pair<const void*, std::uint32_t>> logged;  // (shape, technique)
			std::set<const void*> shapes;
			std::map<const void*, RootEntry> roots;  // this period's
			std::uint32_t lastSummary = 0;
			std::uint64_t unlogged = 0;
		};

		CensusState& Get()
		{
			static CensusState state;
			return state;
		}

		std::string Describe(const RE::NiAVObject* a_node)
		{
			const auto* rtti = a_node->GetRTTI();
			const char* name = a_node->name.c_str();
			return std::format("'{}' ({})", name ? name : "", rtti && rtti->GetName() ? rtti->GetName() : "?");
		}

		/** @brief Whether a_shape is a TESWaterSystem water object's shape, and whether it hangs under the water system's root. */
		std::pair<bool, bool> WaterSystemMembership(const RE::BSGeometry* a_shape)
		{
			auto* system = RE::TESWaterSystem::GetSingleton();
			if (!system)
				return { false, false };
			bool object = false;
			{
				RE::BSSpinLockGuard guard(system->lock);
				for (const auto& water : system->waterObjects)
					if (water && water->shape.get() == a_shape) {
						object = true;
						break;
					}
			}
			bool underRoot = false;
			const RE::NiAVObject* root = system->waterRoot.get();
			for (const RE::NiAVObject* node = a_shape; node && root; node = node->parent)
				if (node == root) {
					underRoot = true;
					break;
				}
			return { object, underRoot };
		}

		void Summarise(CensusState& a_state, std::uint32_t a_frame)
		{
			if (a_frame - a_state.lastSummary < kSummaryFrames)
				return;
			a_state.lastSummary = a_frame;
			if (a_state.roots.empty())
				return;
			std::string list;
			for (const auto& [root, entry] : a_state.roots)
				list += std::format("{}{}: {} shapes, {} draws", list.empty() ? "" : "; ", entry.name, entry.shapes.size(), entry.draws);
			logger::info("[DCLF] water census: {} roots held drawn water in the last {} frames ({} shapes seen in all{}): {}", a_state.roots.size(), kSummaryFrames,
				a_state.shapes.size(), a_state.unlogged ? std::format(", {} not logged", a_state.unlogged) : std::string(), list);
			a_state.roots.clear();
		}
	}

	void Note(const RE::BSRenderPass& a_pass, std::uint32_t a_frame, bool a_unifiedWaterTile)
	{
		auto& state = Get();
		Summarise(state, a_frame);
		const auto* shape = a_pass.geometry;
		if (!shape)
			return;

		// The root and the root's child on the path name the category the shape belongs to.
		const RE::NiAVObject* root = shape;
		const RE::NiAVObject* belowRoot = nullptr;
		std::string chain = Describe(shape);
		std::uint32_t depth = 0;
		for (const RE::NiAVObject* node = shape->parent; node && depth < kMaxDepth; node = node->parent, ++depth) {
			belowRoot = root;
			root = node;
			chain += " <- " + Describe(node);
		}
		auto& entry = state.roots[root];
		if (entry.name.empty())
			entry.name = belowRoot && belowRoot != shape ? std::format("{} / {}", Describe(root), Describe(belowRoot)) : Describe(root);
		entry.shapes.insert(shape);
		++entry.draws;
		state.shapes.insert(shape);

		const std::uint32_t technique = TechniqueOf(RawDescriptor(a_pass.passEnum));
		if (!state.logged.insert({ shape, technique }).second)
			return;
		if (state.logged.size() > kMaxLoggedShapes) {
			++state.unlogged;
			return;
		}
		const auto* property = static_cast<const RE::BSWaterShaderProperty*>(a_pass.shaderProperty);
		const auto [systemObject, underSystemRoot] = WaterSystemMembership(shape);
		logger::info("[DCLF] water census: {} technique {} pass {:#x} hint {}: flags {:#x}, unk8C {:#x}, material {}, Unified Water tile {}, water system object {}, under its root {}, root {}; chain {}",
			Describe(shape), technique, a_pass.passEnum, a_pass.accumulationHint, property->waterFlags.underlying(), property->unk8C,
			static_cast<const void*>(property->material), a_unifiedWaterTile ? "yes" : "no", systemObject ? "yes" : "no", underSystemRoot ? "yes" : "no",
			static_cast<const void*>(root), chain);
	}
}
