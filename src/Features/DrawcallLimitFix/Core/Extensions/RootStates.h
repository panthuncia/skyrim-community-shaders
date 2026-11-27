#pragma once

// Core's side of Api/RootStates.h: the root list view, what the root test gets, and the producer's entry with the calls Core makes into it.

#include "Features/DrawcallLimitFix/Api/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <span>
#include <string>

namespace DCLF
{
	struct SceneTables;
	struct FrameGlobals;
	namespace SceneCapture
	{
		struct NodeRecord;
	}
	namespace Extensions
	{
		class TableFamilies;
	}
}

namespace DCLF::Api
{
	/**
	 * @brief The listed roots of one tables, valid while those tables are held. The producer fills rows, nodes and index.
	 * Core fills each object's root and the journal's content epoch. Empty without a producer or before its family's first write.
	 */
	class RootListView
	{
	public:
		std::span<const FadeRootStatic> rows;
		std::span<const void* const> nodes;
		const ankerl::unordered_dense::map<const void*, std::uint32_t>* index = nullptr;
		std::span<const std::uint32_t> objectRoot;
		std::uint64_t contentEpoch = 0;

		/** @brief Root slots in the tables, free ones included. */
		std::size_t Size() const { return rows.size(); }
		/** @brief a_node's root slot, or kNoFadeRoot. */
		std::uint32_t Find(const void* a_node) const
		{
			if (!index)
				return kNoFadeRoot;
			const auto it = index->find(a_node);
			return it != index->end() ? it->second : kNoFadeRoot;
		}
		bool Contains(const void* a_node) const { return index && index->contains(a_node); }
		/** @brief a_object's root slot, or kNoFadeRoot. */
		std::uint32_t RootOf(std::uint32_t a_object) const { return a_object < objectRoot.size() ? objectRoot[a_object] : kNoFadeRoot; }
		/** @brief a_root's node, or null for a free slot. */
		const void* NodeOf(std::uint32_t a_root) const { return a_root < nodes.size() ? nodes[a_root] : nullptr; }
	};

	/** @brief What the root test gets: the fade node's mirror record, never engine memory, and the frame's captured globals. Node is null for globalsKey. */
	class RootTestInput
	{
	public:
		const SceneCapture::NodeRecord* node = nullptr;
		const FrameGlobals& globals;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry, with its name owned. */
	struct RootStatesEntry
	{
		std::string name;
		Api::RootStatesContribution contribution;
	};

	/** @brief Main thread, at the end of InstallFeatures. */
	void ResolveRootStates(const Api::RootStatesContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. Null without a producer. */
	const RootStatesEntry* RootStates();

	/** @brief Main thread, from the producer's early hooks. Reports that its early engine hooks are in. */
	void NoteRootStatesInstalled();
	/** @brief Main thread, before InstallFeatures. Whether the producer's early hooks are in. The primary cull requires it. */
	bool RootStatesInstalled();

	/** @brief Any thread holding a_tables, never per object. The listed roots, or empty without a producer. */
	Api::RootListView RootListOf(const SceneTables& a_tables);
	Api::RootListView RootListOf(const TableFamilies& a_families);

	/** @brief Per join, under the accumulate shard. Runs the root test for an object under a_node. Zero without a producer. */
	void RootStatesTest(const SceneCapture::NodeRecord& a_node, const FrameGlobals& a_globals, Api::RootTestOutput& a_out);
	/** @brief Per membership pass. Mixes the producer's globals into the FNV-1a witness. */
	void RootStatesGlobalsKey(const FrameGlobals& a_globals, std::uint32_t& a_witness);
	/** @brief The root test's record bits, or 0 without a producer. */
	std::uint32_t RootStatesObjectFlags();
	Api::TableFamilyId RootStatesFamily();
	/** @brief Render thread, once per frame. The producer's kick. */
	void RootStatesKick();
	/** @brief Render thread, at the depth commit. The fade test's latch words, or a zero eye and a {0, +infinity} height without a producer. */
	void RootStatesLatchGlobals(float (&a_eye)[4], float (&a_treeHeight)[2]);
}
