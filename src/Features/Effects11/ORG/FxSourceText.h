#pragma once

// Text passes over .fx source that both Effects11 paths use: FX11's (ENBExtender) and the ORG FX runtime's. No D3D11 or
// CS dependency, so the offline parser check (tools/fx-parse-check) runs them too.

#include <string>
#include <string_view>
#include <vector>

namespace OrgFx::SourceText
{
	/**
	 * @brief Removes stringify macro definitions (`#define NAME(x) #x`) and returns their names. Each removed line becomes a
	 * blank line, so line numbers stay the same. D3DPreprocess has no `#` operator: ExpandStringifyMacros replaces the calls.
	 */
	void StripStringifyDefines(std::string& a_source, std::vector<std::string>& a_macroNames);

	/**
	 * @brief Replaces each call of the named macros with its argument as a string literal. The spaces D3DPreprocess puts
	 * between tokens are collapsed the way ENB's own `#x` would write them ("| -" to "|-", " . " to ".").
	 */
	void ExpandStringifyMacros(std::string& a_source, const std::vector<std::string>& a_macroNames);

	/** @brief Replaces `#line` directives with blank lines. */
	void StripLineDirectives(std::string& a_source);

	/** @brief True if the source is a KIEFX-encrypted ENB Extender preset (no readable source). */
	bool IsEncrypted(std::string_view a_source);
}
