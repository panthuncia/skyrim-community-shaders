#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"

#include <cstdint>
#include <string>

// What only one holder's engine references keep alive, meaning objects whose count is 1, and the geometries under them.
// Read on the holder's own thread. A sole-held subtree is reached by nothing else, so its walk races no engine edit.

namespace DCLF::Diagnostics
{
	struct SoleHolds
	{
		std::uint64_t references = 0;
		std::uint64_t sole = 0;            // references whose object's count is 1
		std::uint64_t soleGeometries = 0;  // geometries in the subtrees of the sole-held scene objects

		void Add(const RE::NiObject* a_object)
		{
			if (!a_object)
				return;
			++references;
			if (a_object->GetRefCount() != 1)
				return;
			++sole;
			if (auto* object = const_cast<RE::NiObject*>(a_object); object->AsNode() || object->AsGeometry())
				Engine::ForEachLive(static_cast<const RE::NiAVObject*>(object), [&](RE::NiAVObject& a_child, std::uint32_t) {
					if (a_child.AsGeometry())
						++soleGeometries;
				});
		}
		void Add(const RE::NiRefObject* a_object) { Add(static_cast<const RE::NiObject*>(a_object)); }

		std::string Text(const char* a_name) const
		{
			return fmt::format("{} {} held ({} sole, {} geometries under them)", a_name, references, sole, soleGeometries);
		}
	};
}
