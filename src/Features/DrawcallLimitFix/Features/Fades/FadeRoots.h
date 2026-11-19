#pragma once

// The fade-root table, a journaled table family, and the coordinator component that maintains its rows.

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Api
{
	class RootListView;
}

namespace DCLF::Scene
{
	/**
	 * @brief One row per fade node, refcounted by the member slots that draw under it.
	 * A row's object is one of its members and is rechosen when membership changes.
	 */
	struct FadeRootTables
	{
		std::vector<FadeRootStatic> fadeRoots;
		std::vector<std::uint32_t> fadeRootRefs, fadeRootFree;
		std::vector<const void*> fadeRootNode;
		ankerl::unordered_dense::map<const void*, std::uint32_t> fadeRootIndex;
		ankerl::unordered_dense::map<const void*, std::uint32_t> fadeRootSwitch;  // tree LOD switch to root slot
		std::uint32_t fadeRootGenerations = 0;
	};

	// Invalid before InstallFeatures.
	Api::TableFamilyId FadeRootFamilyId();
	Api::CoordinatorComponentId FadeRootComponentId();
	Api::EngineChannelId FadeSnapsChannelId();
	Api::EngineChannelId FadeAmountsChannelId();

	// Api::TableFamily callbacks.
	std::shared_ptr<void> MakeFadeRoots();
	void AssignFadeRoots(void* a_to, const void* a_from);
	void AssignFadeRootRow(void* a_to, const void* a_from, std::uint32_t a_row);
	bool SameFadeRoots(const void* a_left, const void* a_right);
	void ClearFadeRoots(void* a_columns);
	void RecycleFadeRoot(void* a_columns, std::uint32_t a_row);
	std::uint32_t FadeRootRows(const void* a_columns);
	void ViewFadeRoots(const void* a_columns, Api::RootListView& a_out);

	/** @brief Must be registered last of the coordinator components. */
	Api::CoordinatorComponent FadeRootComponent();

	/** @brief Admitted roots the primary cull stands in for. */
	std::size_t FadeRootsStoodIn();

	void OwnerLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text);

	/** @brief Which code last changed a root's row, for the fade census. WriteBack is never stored, only reported. */
	enum class FadeRootWriter : std::uint8_t
	{
		None,
		Listed,
		EngineEvent,  // fadeAmount event from Actor::SetAlpha
		Recheck,      // recheck of a newly seeded actor root
		Reseed,       // placement snap
		WriteBack,
	};
	std::string_view FadeRootWriterName(FadeRootWriter a_writer);

	/** @brief Diagnostic capture. */
	void FadeRootsCensus(const Api::CoordinatorComponentContext& a_context, std::string& a_text);
	/** @brief Diagnostic capture. */
	void FadeRootsGrowth(const Api::CoordinatorComponentContext& a_context, std::string& a_text);
}
