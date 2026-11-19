#pragma once

// The geometry only the engine draws in a view DCLF otherwise draws. It forms the one subset every such view's engine cull is limited to.
//
// Caster: geometry with no record that casts a shadow the engine would draw, such as a refused skin or a buffer DCLF cannot import.
// Colour: geometry the engine would draw in a colour view DCLF takes over by root, under an object class's category root.
//
// A view never culls the whole scene for it. Each view hands its engine cull just these roots.

#include <array>
#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Scene
{
	/** @brief One publication's engine-only geometry, by the views it matters to. Immutable once published. */
	struct EngineOnlySubset
	{
		enum View : std::uint8_t
		{
			kCaster = 0,
			kColour = 1,
			kViewCount = 2
		};
		static constexpr std::uint8_t ViewBit(View a_view) { return static_cast<std::uint8_t>(1u << a_view); }

		// A root is culled by the engine with all below it. An ancestor of a root is descended so its portal, room and bound tests stay on the path.
		enum class Admit : std::uint8_t
		{
			Skip,
			Descend,
			Cull
		};

		struct Roots
		{
			// A light or sun entry while the coordinator owns it, else the geometry itself. Either lives while the publication does.
			std::vector<const RE::NiAVObject*> roots;
			// Per root, its geometry's category node as a key, or null.
			std::vector<const void*> category;
			// Keys only, never dereferenced.
			ankerl::unordered_dense::set<const void*> rootSet, ancestors;
		};

		std::array<Roots, kViewCount> views;
		std::uint32_t geometries = 0;     // the geometries under the roots
		std::uint32_t geometryRoots = 0;  // of the roots, geometries culled alone (no owned root above them)

		const Roots& Of(View a_view) const { return views[a_view]; }

		Admit AdmitObject(View a_view, const void* a_object) const
		{
			const auto& view = views[a_view];
			if (view.rootSet.contains(a_object))
				return Admit::Cull;
			return view.ancestors.contains(a_object) ? Admit::Descend : Admit::Skip;
		}
	};
}
