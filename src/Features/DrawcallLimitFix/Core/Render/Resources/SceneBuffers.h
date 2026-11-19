#pragma once

// The scene's GPU tables shared by every epoch, and readers for the root states and cull bindings. Render thread.
// Features include this header directly, so it must stay self-contained.

// volk must come before every Vulkan header in this translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/ContentOwners.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct IndexPool;

	/** @brief The scene's tables. Each is a buffer of the scene's set, and the revision producer grows it to hold the tables. */
	struct SceneBuffers
	{
		// The second vertex stream the passes bind, resolved once at setup. Null without a stream.
		Versioned vertexStream;
		// Kept current by every commit that draws from it.
		std::shared_ptr<IndexPool> pool;
		// The features' scene resource states, in registration order, held as long as these tables.
		std::vector<std::shared_ptr<void>> sceneResources;
		// The passes' setup states, in registration order. Null when a pass has no setup or is not added.
		std::vector<std::shared_ptr<const void>> passStates;
		// Visibility readback buffers. Every version is zero-filled, so the decoder restarts with the first batch of a version it has not followed.
		Versioned visibilityEvents, visibilityRootReported, visibilityObjectReported;
		std::array<org::services::FilledVersion, 2> visibilityReportedFollowed;
		std::shared_ptr<const ComputeProgram> visibilityEventsProgram;
		std::shared_ptr<VisibilityReadback> visibilityReadback;
		// Each draw family's pipelines, published once built. Read on the graph host's thread.
		std::array<std::atomic<std::shared_ptr<const FamilyPipelines>>, Extensions::kDrawFamilies> familyPipelines;
		// Owners of the descriptors these tables' rows name. Executions hold them while the bytes are current.
		ContentOwners content;
	};

	/** @brief The root states' family in a_tables, or null. Its objectRow gives each object's root slot. */
	inline const Extensions::TableFamilyColumns* RootFamilyOf(const SceneTables& a_tables)
	{
		return a_tables.families.Find(Extensions::RootStatesFamily());
	}

	/** @brief Cull binding a_slot's descriptor for a_frame, or 0 when unbound. Render thread. */
	inline std::uint32_t CullBindingIndex(Api::CullBindingSlot a_slot, const SceneBuffers& a_scene, std::uint32_t a_frame, const NamedVersions& a_adopted)
	{
		const auto select = Extensions::SelectCullBinding(a_slot, a_scene, a_frame, &a_adopted);
		if (select.versioned && *select.versioned)
			return a_adopted.Of(**select.versioned).srvIndex;
		if (select.buffer && *select.buffer)
			return (*select.buffer)->GetSRVInfo(0).slot.index;
		return select.srv;
	}

	/** @brief Whether the root states' rows are bound and hold the tables' roots. Core's cull binds them only then. */
	inline bool RootRowsCurrent(const SceneBuffers& a_scene)
	{
		const auto select = Extensions::SelectCullBinding(Api::CullBindingSlot::RootRows, a_scene);
		return select.versioned && *select.versioned && select.rows;
	}

	/** @brief Sends sorted word indices as runs a_send(first, count), merging gaps up to kWordRunGap. */
	constexpr std::uint32_t kWordRunGap = 32;
	template <class Send>
	void SendWordRuns(const std::vector<std::uint32_t>& a_sorted, Send&& a_send)
	{
		for (std::size_t i = 0; i < a_sorted.size();) {
			const std::uint32_t first = a_sorted[i];
			std::uint32_t lastIndex = first;
			while (++i < a_sorted.size() && a_sorted[i] - lastIndex <= kWordRunGap)
				lastIndex = a_sorted[i];
			a_send(first, lastIndex - first + 1);
		}
	}
}
