#pragma once

// The decal draw order: its key, the coordinator component's state, and the parity comparison.

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Api
{
	class CoordinatorComponentContext;
}

namespace DCLF::Features::Decals
{
	/**
	 * @brief Where the engine's main registration reaches a geometry (skyrim-engine-notes.md, "The main registration's order").
	 * Geometry under a BSOrderedNode registers after the list's plain appends.
	 */
	struct OrderKey
	{
		std::uint32_t list = 0;
		bool ordered = false;
		std::uint32_t position = 0;
		std::vector<std::uint32_t> path;
	};

	inline bool Before(const OrderKey& a_a, const OrderKey& a_b)
	{
		if (a_a.list != a_b.list)
			return a_a.list < a_b.list;
		if (a_a.ordered != a_b.ordered)
			return !a_a.ordered;
		if (a_a.position != a_b.position)
			return a_a.position < a_b.position;
		return std::lexicographical_compare(a_a.path.begin(), a_a.path.end(), a_b.path.begin(), a_b.path.end());
	}

	struct OrderedDecal
	{
		std::uint64_t chain;
		OrderKey key;
		std::uint32_t object;
		// Places under a decal node count from the end of its array. Adding or removing one shifts all of them, so they are re-keyed together.
		const RE::NiAVObject* decalNode = nullptr;
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY sums, reset by each report. */
	struct DecalOrderParity
	{
		std::uint64_t checks = 0, decals = 0, differ = 0, staleKeys = 0, unordered = 0;
		std::string first, staleFirst;
	};

	/** @brief The coordinator component's state. Scene lane. */
	struct DecalOrder
	{
		// By group, which is (chain >> 60) - 1.
		std::array<std::uint32_t, 3> candidates{};
		DecalOrderParity parity;
		// Object -> chain. The chain packs group, technique and sub-pass.
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> memberDecals;
		std::vector<std::uint32_t> decalsChanged;
		bool decalsRebuild = true;
		// Kept in draw order, so a change re-keys only the decals it names.
		std::vector<OrderedDecal> kept;
		std::vector<std::uint32_t> decalOrdered;
	};

	/** @brief The kept order compared with one rebuilt from scratch. */
	struct DecalOrderComparison
	{
		std::uint64_t decals = 0, staleKeys = 0, unordered = 0;
		bool differ = false;
		std::string first, staleFirst;  // The first difference and the first stale key.
	};

	/** @brief A key goes stale when a parent's children are re-indexed under a kept decal. */
	DecalOrderComparison CompareDecalOrder(const DecalOrder& a_order, const Api::CoordinatorComponentContext& a_context);

	/** @brief Observers set `checked` to force the upkeep even with no change. */
	struct OrderDue
	{
		std::uint64_t passSerial;
		bool& checked;
	};

	struct Ordered
	{
		const DecalOrder& order;
		const Api::CoordinatorComponentContext& context;
		DecalOrderParity& parity;
	};
}
