#pragma once

// The CPU pipeline's view batches. The snapshot builder makes one immutable set per snapshot. Its resident sequences live in a
// pooled buffer version that must be GPU ready before the snapshot is posted. See dclf-architecture.md.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace org::runtime
{
	class IUploadService;
}

namespace org
{
	class RenderPassBuilder;
}

namespace RenderGraph
{
	struct DeclaredRefs;
}

namespace DCLF
{
	struct SceneTables;
	struct Lookups;
	struct PipelineCatalog;
	struct DrawSequence;
}

namespace DCLF::Draws::detail
{
	class SceneLists;
}

namespace DCLF::Draws
{
	struct ScenePayload;
	struct MainPayload;
	struct ShadowPayload;

	/** @brief What a snapshot's view batches are made from. keep holds it all alive as long as the set. */
	struct ViewBatchInputs
	{
		std::shared_ptr<const void> keep;
		const SceneTables* tables = nullptr;
		const Lookups* lookups = nullptr;
		const PipelineCatalog* catalog = nullptr;
		const ScenePayload* payload = nullptr;
		const MainPayload* main = nullptr;
		const ShadowPayload* shadow = nullptr;
		std::shared_ptr<org::runtime::IUploadService> uploads;
		// Only the forward family's lanes, rebuilt in full each set. Used by the GPU-driven graph's forward draws.
		bool forwardOnly = false;
	};

	/** @brief Makes view batch sets on the snapshot builder. Builder thread only. */
	class ViewBatchBuilder
	{
	public:
		ViewBatchBuilder();
		~ViewBatchBuilder();
		ViewBatchBuilder(const ViewBatchBuilder&) = delete;
		ViewBatchBuilder& operator=(const ViewBatchBuilder&) = delete;
		struct Pending;  // a sequences version in flight

		/**
		 * @brief Whether the set for a_in's payload is complete with its sequences GPU ready. The first call makes it.
		 * Also true with no set when it cannot be made, and then the CPU-mode epochs are not covered.
		 */
		bool Ready(const ViewBatchInputs& a_in);
		/** @brief The complete set, or null when there is none or it is not Ready yet. */
		const std::shared_ptr<Direct::ViewBatchSet>& Set() const { return set; }
		/** @brief The payload the set, or the one being made, is for. */
		const void* For() const { return payload; }
		/** @brief Forget the set and anything in flight. */
		void Reset();

	private:
		const void* payload = nullptr;  // the payload the set or the pending one is for
		std::uint32_t lanes = 0;        // ... and its batch lanes
		bool forwardOnly = false;       // ... and whether it is forward only
		std::shared_ptr<Direct::ViewBatchSet> set, building;
		std::shared_ptr<Pending> pending;
		std::shared_ptr<const Direct::SnapshotSpheres> spheres;
		std::uint64_t versions = 0;
		bool failed = false;         // Ready returns true with no set until the payload changes
		// Kept across payloads.
		std::unique_ptr<detail::SceneLists> lists;
		std::uint64_t listSets = 0;
	};

	/**
	 * @brief Builder thread. Make a_in's full set and write its resident sequences to a_sequences for the caller to upload.
	 * With no spheres yet, every lane is treated as moving.
	 */
	std::shared_ptr<Direct::ViewBatchSet> MakeViewBatches(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres,
		std::vector<DrawSequence>& a_sequences);

	/** @brief Any thread. Lanes per batch for the next sets. More lanes coalesce more draws into each culled batch. */
	std::uint32_t BatchLanes();
	/** @brief Any thread. Rounded up to a multiple of 8 within [8, 65528]. The next snapshot remakes its set. */
	void SetBatchLanes(std::uint32_t a_lanes);

	/** @brief Render thread. Keep a_set's sequence backing from reuse until a_point. Call wherever the installed payload is bound. */
	void BindViewBatches(const Direct::ViewBatchSet& a_set, std::uint64_t a_point);

	/** @brief Render thread. Make a_set's sequences the current version of the late-bound buffer the CPU sites read. No graph build. */
	void PublishViewBatches(const Direct::ViewBatchSet* a_set);

	/** @brief Graph build. Declare the pass's read of the view batch sequences, for CheckDeclaredRefs in Prepare. */
	void DeclareViewBatchReads(org::RenderPassBuilder& a_builder, RenderGraph::DeclaredRefs& a_out);
}
