#pragma once

// Core's side of Api/AncestorValues.h: the contribution's entry, resolved once at install, and the sink its fill hands the map through.

#include "Features/DrawcallLimitFix/Api/AncestorValues.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DCLF::Extensions
{
	/** @brief Key node to value. The render thread posts a copy to the scene lane. */
	using AncestorValueMap = ankerl::unordered_dense::map<const void*, std::int32_t>;
}

namespace DCLF::Api
{
	/** @brief Where a fill puts the map. Core posts its copy once filled. */
	class AncestorMapSink
	{
	public:
		explicit AncestorMapSink(Extensions::AncestorValueMap& a_out) :
			out(a_out) {}

		/** @brief Reserves room for a_count entries. */
		void Reserve(std::size_t a_count) { out.reserve(a_count); }
		/** @brief Sets a_node's value. A node put twice keeps its first value. */
		void Put(const void* a_node, std::int32_t a_value) { out.emplace(a_node, a_value); }

	private:
		Extensions::AncestorValueMap& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry. Every field keeps its default when none is registered. */
	struct AncestorValueEntry
	{
		bool registered = false;
		std::string name;
		std::uint32_t recordWord = 0;
		std::int32_t none = -1;
		// RTTI of the key classes, resolved once. A null entry is unused.
		std::array<const void*, 2> ancestorRtti{};
		Api::ClassifyInputId whileOn;
		decltype(Api::AncestorValueContribution::generation) generation = nullptr;
		decltype(Api::AncestorValueContribution::fill) fill = nullptr;

		bool IsKey(const void* a_rtti) const { return a_rtti && (a_rtti == ancestorRtti[0] || a_rtti == ancestorRtti[1]); }
	};

	/** @brief Main thread, once, at the end of InstallFeatures. Calls the RTTI functions. */
	void ResolveAncestorValue(const Api::AncestorValueContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. Null without a contribution. */
	const AncestorValueEntry* AncestorValue();
	/** @brief Any thread, after InstallFeatures. Whether a contribution is registered and its whileOn input is on. */
	bool AncestorValueOn();
}
