#pragma once

// A per-object value another CS feature keys by an engine node above the object, such as Light Limit Fix's room index.
// Core walks to the key node and looks the value up in the feature's map. Nothing runs per object per frame. One contribution only.

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/AncestorValues.h.
	class AncestorMapSink;

	/**
	 * @brief The ancestor value contribution.
	 *
	 * - none: the value when there is no keyed ancestor, no map entry, or whileOn is off.
	 * - ancestorRtti: the node classes whose nearest instance above the object is its key. One may be null.
	 * - generation: render thread, once per frame while whileOn. Changes whenever fill would hand a different map.
	 * - fill: render thread, when generation moved. Core posts a copy, so the feature's map stays render-thread only.
	 */
	struct AncestorValueContribution
	{
		std::string_view name;
		std::uint32_t recordWord = 0;
		std::int32_t none = -1;
		std::array<const void* (*)(), 2> ancestorRtti{};
		ClassifyInputId whileOn;
		std::uint64_t (*generation)() = nullptr;
		void (*fill)(AncestorMapSink& a_map) = nullptr;
	};
}
