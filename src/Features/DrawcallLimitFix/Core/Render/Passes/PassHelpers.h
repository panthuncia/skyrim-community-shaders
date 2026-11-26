#pragma once

// What the passes and graph extensions share: recording helpers and registration. Feature-facing helpers live in
// Extensions/PassContext.h and Passes/DrawHelpers.h. Part of IndirectDraws' implementation, included by Internal.h.

#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"

#include <Render/RunRef.h>

namespace DCLF::Draws
{
	/**
	 * @brief Folds the camera translation into the view-projection, for CPU readers that project absolute positions. The fold
	 * is not exact in float. Around 2^18 units the clip position is off by a few 1/32 units, so a cull from it needs a margin.
	 */
	void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16]);

	/** @brief Writes a view's culling camera into its latch: the camera-relative view-projection, its eye and the absolute-plane margin. */
	inline void LatchCullCamera(BuildDrawsLatch& a_latch, const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye)
	{
		std::memcpy(a_latch.viewProj, a_viewProj.data(), sizeof(a_latch.viewProj));
		a_latch.cullEye[0] = a_eye.x;
		a_latch.cullEye[1] = a_eye.y;
		a_latch.cullEye[2] = a_eye.z;
		a_latch.cullEye[3] = CullPlaneMargin(a_eye.x, a_eye.y, a_eye.z);
	}

	// MainPayload::objectState values besides the Skip reasons.
	constexpr std::uint8_t kObjectStateDrawable = 0xF0;
	constexpr std::uint8_t kObjectStateDecal = 0xF1;
	constexpr std::uint8_t kObjectStateAbsent = 0xFF;

	// Imports a DXVK image for the graph to use in place, in the layout DXVK rests it in. Every importer shares the one graph
	// resource. The slot keeps a_texture alive.
	FrameAnchors::Texture ImportImage(rhi::Device a_device, ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
		const char* a_name);

	/** @brief The scene's index pool, made by whichever extension sets up first. Null, logged, when its copy program cannot be created. */
	std::shared_ptr<IndexPool> EnsureIndexPool(SceneBuffers& a_scene, rhi::Device a_device, bool a_small);

	/**
	 * @brief Brings the index pool up to the sealed tables. The first commit to draw from it in a frame writes the copy dispatch,
	 * and later commits find nothing to copy. Returns the copy count. With none, the depth commit skips the dispatch.
	 */
	std::uint32_t UpdateIndexPool(const NamedVersions& a_sealed, IndexPool& a_pool, const SceneTables& a_tables, std::uint32_t a_generation, const org::LatchBlock& a_latch,
		const org::RunRef& a_latchRun, std::uint32_t a_poolOffset, CommitUploads& a_uploads);

	// The graph extensions that add DCLF's passes, made from a build state. Null when the state has no resources for them.
	struct BuildState;
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<const BuildState> a_build);
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<const BuildState> a_build);

	/** @brief Registers the scene tables. Every extension does, and a second registration is an update. */
	void RegisterSceneBuffers(org::RenderGraph& a_graph, const SceneBuffers& a_scene);

	/** @brief A pass of an extension. Only the schedule's executor calls AddPass. */
	using PassKind = Api::PassKind;
	using PassQueue = Api::PassQueue;
	template <class Epoch>
	void AddPass(std::vector<org::RenderGraph::ExternalPassDesc>& a_out, PassKind a_kind, std::string a_name, std::shared_ptr<org::RenderPass> a_pass, Epoch a_epoch,
		PassQueue a_queue = PassQueue::Default, bool a_recordAtExecution = false)
	{
		using Desc = org::RenderGraph::ExternalPassDesc;
		auto desc = a_kind == PassKind::Copy ? Desc::Copy(std::move(a_name), std::move(a_pass)) :
		            a_kind == PassKind::Compute ? Desc::Compute(std::move(a_name), std::move(a_pass)) :
		                                          Desc::Render(std::move(a_name), std::move(a_pass));
		// ORG reads an external pass's queue from its description.
		if (a_queue == PassQueue::Compute)
			desc.PreferQueue(AsyncComputeQueue());
		else if (a_queue == PassQueue::Default && a_kind != PassKind::Render)
			desc.PreferQueue(org::QueueKind::Graphics);
		// Recorded in ORG's late batch by its external epoch's execution.
		if (a_recordAtExecution)
			desc.RecordAtExecution();
		a_out.push_back(std::move(desc.Epoch(a_epoch)));
	}

	/** @brief A forward range's back-to-front sort, one dispatch per state class with a forward sub-range. No program means nothing to sort. */
	struct ForwardSortSource
	{
		struct Range
		{
			std::uint32_t stagingBase = 0, capacity = 0, countOffset = 0;
			std::uint32_t options = 0;  // added to the range's kCountOffset
		};
		std::shared_ptr<const ComputeProgram> program;
		std::vector<Range> ranges;
	};
	/** @brief Sorts a forward range of a_sequences, the main one or a view's, reading class counts from a_counts. */
	std::shared_ptr<org::RenderPass> MakeForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source);

	/** @brief Sets the row strides in BuildDraws' constants. */
	inline void SetRowStrides(BuildDrawsConstants& a_constants, bool a_pipelineRows = true)
	{
		a_constants.materialRowStride = kMaterialRowBytes;
		a_constants.pipelineRowStride = a_pipelineRows ? kPipelineRowBytes : 0u;
	}
	/** @brief Writes the installed payload's main parts into a culling latch. */
	inline void PayloadLatch(const PayloadVersions& a_payload, BuildDrawsLatch& a_latch)
	{
		const std::uint64_t materialRows = a_payload[PayloadPart::MaterialRows].address, pipelineRows = a_payload[PayloadPart::PipelineRows].address;
		a_latch.inputsIndex = a_payload[PayloadPart::Inputs].srvIndex;
		a_latch.geometriesIndex = a_payload[PayloadPart::Geometries].srvIndex;
		a_latch.materialRowsLo = static_cast<std::uint32_t>(materialRows);
		a_latch.materialRowsHi = static_cast<std::uint32_t>(materialRows >> 32);
		a_latch.pipelineRowsLo = static_cast<std::uint32_t>(pipelineRows);
		a_latch.pipelineRowsHi = static_cast<std::uint32_t>(pipelineRows >> 32);
		a_latch.materialRowsRef = a_payload[PayloadPart::MaterialRows].ref;
		// The frame's copy of animated rows, which their draws read instead of the installed rows.
		const auto animatedRows = PassAnimatedRows();
		a_latch.animatedRowsLo = static_cast<std::uint32_t>(animatedRows.address);
		a_latch.animatedRowsHi = static_cast<std::uint32_t>(animatedRows.address >> 32);
		a_latch.animatedRowsRef = animatedRows.ref;
		// The frame's per-object rows, which their pass writes from this latch.
		const auto frameRows = PassFrameRows();
		a_latch.frameRowsRef = frameRows.ref;
		a_latch.frameRowsCount = frameRows.ref ? frameRows.count : 0u;
	}
	/** @brief Writes the installed payload's shadow parts into a shadow or occlusion view's latch. */
	inline void ShadowPayloadLatch(const PayloadVersions& a_payload, BuildDrawsLatch& a_latch)
	{
		const std::uint64_t shadowRows = a_payload[PayloadPart::ShadowRows].address;
		a_latch.inputsIndex = a_payload[PayloadPart::Inputs].srvIndex;
		a_latch.geometriesIndex = a_payload[PayloadPart::Geometries].srvIndex;
		a_latch.materialRowsLo = static_cast<std::uint32_t>(shadowRows);
		a_latch.materialRowsHi = static_cast<std::uint32_t>(shadowRows >> 32);
		a_latch.materialRowsRef = a_payload[PayloadPart::ShadowRows].ref;
		a_latch.pipelineRowsLo = a_latch.pipelineRowsHi = 0;
	}
	/** @brief Writes a bucket map into a run's latch region. Slots added since the map was made get kNoBucket. */
	inline void WriteBucketMap(const org::LatchBlock& a_latch, std::string_view a_what, const org::RunRef& a_run, std::uint32_t a_offset, std::span<const std::uint32_t> a_map,
		std::uint32_t a_slots)
	{
		if (!a_map.empty())
			LatchWrite(a_latch, a_what, a_run, a_offset, std::as_bytes(a_map));
		if (a_slots <= a_map.size())
			return;
		static thread_local std::vector<std::uint32_t> none;
		none.assign(a_slots - a_map.size(), kNoBucket);
		LatchWrite(a_latch, a_what, a_run, a_offset + static_cast<std::uint32_t>(a_map.size() * sizeof(std::uint32_t)), std::as_bytes(std::span(none)));
	}
}
