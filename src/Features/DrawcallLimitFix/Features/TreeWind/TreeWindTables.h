#pragma once

// Tree wind's table family: the tree slots its members draw under, published with the scene's tables. Written by the coordinator
// component on the scene lane. Read from the tables they are handed.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Features::TreeWind
{
	/** @brief The trees' columns: a slot per tree node, refcounted by members. Each slot's generation is its row tag. */
	struct TreeTables
	{
		std::vector<TreeStatic> trees;
		std::vector<std::uint32_t> treeRefs, treeFree;
		std::vector<const void*> treeNode;
		ankerl::unordered_dense::map<const void*, std::uint32_t> treeIndex;
		std::vector<TreeObject> treeObjects;
		std::uint64_t treesContentEpoch = 0, treeObjectsVersion = 0;
		std::uint32_t treeGenerations = 0;
	};

	/** @brief The trees' family's registration. */
	Api::TableFamilyId TreeFamilyId();
	/** @brief The trees' family in a_tables, or null before any tree was listed. */
	const Extensions::TableFamilyColumns* TreeFamilyOf(const SceneTables& a_tables);
	/** @brief The trees' columns in a_tables (empty ones before the coordinator first wrote them). */
	const TreeTables& TreesOf(const SceneTables& a_tables);
	/** @brief a_object's tree slot in a_tables (kNoTree, kNodelessTree, or a slot). */
	std::uint32_t TreeOf(const SceneTables& a_tables, std::uint32_t a_object);
}
