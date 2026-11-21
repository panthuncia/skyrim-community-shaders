#pragma once

// Core's side of Api/DeferredRanges.h: the registered ranges, resolved once at install, and which ranges a key draws in.
// A slot nobody registers draws nothing, and a key outside a DeferredRanges family is in no range.

#include "Features/DrawcallLimitFix/Api/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DCLF::Extensions
{
	inline constexpr std::size_t kDeferredRanges = Api::kDeferredRanges;

	/** @brief A range's entry, with its strings owned. */
	struct DeferredRangeEntry
	{
		bool taken = false;
		Api::DeferredRangeContribution contribution;
		std::string name;
		std::string colourRange;  // "cs.dclf.deferred.<name>"
	};
	using DeferredRangeSet = std::array<DeferredRangeEntry, kDeferredRanges>;

	/** @brief Main thread, at the end of InstallFeatures, after the program families. Fails if a range's family is not a DeferredRanges build. */
	void ResolveDeferredRanges(const std::array<Api::DeferredRangeContribution, kDeferredRanges>& a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const DeferredRangeSet& DeferredRanges();

	/** @brief The ranges a key draws in, a bit per slot, and its stencil reference in each. */
	struct DeferredDraws
	{
		std::uint32_t ranges = 0;
		std::array<std::uint8_t, kDeferredRanges> stencilRef{};
	};
	/** @brief Snapshot builder or any thread after InstallFeatures. Empty for a key outside a DeferredRanges family. */
	DeferredDraws DeferredDrawsOf(const PipelineKey& a_key);
}
