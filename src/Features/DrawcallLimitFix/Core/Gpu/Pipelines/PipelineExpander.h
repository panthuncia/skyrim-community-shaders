#pragma once

// The combination expander: a pipeline key source that adds likely neighbours of the keys the manifests list, so a key a session
// happened not to see is still built at startup.
//
// Every rule is a small function in PipelineExpander.cpp. A rule only derives keys from listed keys, never from another rule's
// keys, and takes every input a new key needs (engine state words, target formats, shadow rasterizer state) from listed keys.
// A key it cannot fill that way is not made. Expanded keys are marked (PipelineKeySet::AddExpanded): a failed build is listed as
// unbuildable and not offered again, and a built one joins the local manifest only when play asks for it.

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PipelineKeySource.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DCLF
{
	/** @brief Adds likely neighbours of the keys already in the set. Run it after the manifest sources. */
	class PipelineCombinationExpander final : public PipelineKeySource
	{
	public:
		/** @brief The engine state bits a main line's build reads (DrawPipelines::Impl::EngineStateBitsOf), or none when malformed. */
		using StateBitsOf = std::function<std::optional<std::uint32_t>(const PipelineWords& a_identity)>;

		explicit PipelineCombinationExpander(StateBitsOf a_stateBitsOf) : stateBitsOf(std::move(a_stateBitsOf)) {}

		std::string_view Name() const override { return "expanded"; }
		void Collect(PipelineKeySet& a_set) override;

		/** @brief Each rule's name and the keys it added, in rule order. */
		const std::vector<std::pair<std::string, std::size_t>>& RuleCounts() const { return ruleCounts; }
		/** @brief Keys a rule made that the unbuildable list holds, so they were left out. */
		std::size_t SkippedUnbuildable() const { return skippedUnbuildable; }

	private:
		StateBitsOf stateBitsOf;
		std::vector<std::pair<std::string, std::size_t>> ruleCounts;
		std::size_t skippedUnbuildable = 0;
	};
}
