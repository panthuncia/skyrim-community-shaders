#pragma once

// The ORG FX runtime's parser: reads an .fx effect into OrgFx::EffectDesc. std only. Design:
// docs/development/org-fx-runtime.md, section 3.

#include "FxModel.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace OrgFx
{
	/**
	 * @brief Parses an effect. a_source is the preprocessed text (Effects11's text steps: patches, extender syntax,
	 * D3DPreprocess, stringify expansion, #line stripping). Leftover directive lines are skipped and comments are handled, so
	 * raw source without macros parses too. Never throws: problems are diagnostics, and an unknown global construct is skipped.
	 *
	 * After parsing it resolves each pass's entry functions and argument bindings, builds the technique chains by Effects11's
	 * numbering rule, and lays out $Globals and each cbuffer with FXC's packing.
	 */
	EffectDesc Parse(std::string_view a_source);

	/**
	 * @brief The initial bytes of a constant buffer ($Globals when a_cbuffer is empty), as FXC would fill them: each laid-out
	 * variable's evaluated initializer at its offset (floats, ints, uints, bools as ~0u/0 like FXC, doubles; matrices in their declared
	 * majorness, column-major by default), zero elsewhere. A single value fills every component, as HLSL's scalar promotion
	 * does. Variables whose initializer was not evaluated stay zero. This is the value image FX11 starts from.
	 */
	std::vector<std::uint8_t> BuildDefaultValues(const EffectDesc& a_fx, std::string_view a_cbuffer = {});
}
