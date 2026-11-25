#pragma once

// The CPU-driven pipeline's recorder. It turns a CulledBatch into direct indexed draws on a command list, binding a pipeline
// only when the lane's bucket changes. Colour and depth lists push the sequence's 24 words to the key's own pipeline,
// other lists push the sequence's address and object word to a pulled pipeline.
// Recording reads only immutable inputs, so any thread may record its own list.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

#include <ORGModuleServices/Passes/IndirectRuns.h>
#include <rhi.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct IndirectState;
	struct ShadowIndirectState;
	struct BindlessPlacement;
}

namespace org::services
{
	class BumpArena;
}

namespace DCLF::Direct
{
	using CullArena = org::services::BumpArena;

	/** @brief The two fields of an object's placement row the forward sort reads, copied per frame for the sort on the CS thread. */
	struct ForwardSortRow
	{
		float forwardSort[4];  // BindlessPlacement::forwardSort
		float bound[4];        // BindlessPlacement::bound
	};

	/** @brief a_row's sort fields. */
	ForwardSortRow ForwardSortRowOf(const BindlessPlacement& a_row);

	/**
	 * @brief One draw in a frame slot's draw buffer, also the stride of an indirect run. D3D12's SV_InstanceID does not count
	 * the first instance, so below Shader Model 6.8 D3D12's signature writes the instance word into kDrawPushInstance first.
	 * Vulkan, and D3D12 with 6.8 (SV_StartInstanceLocation), read the arguments alone.
	 */
	struct IndirectDraw
	{
		std::uint32_t instance;  // its index in the buffer, as firstInstance
		std::uint32_t indexCount;
		std::uint32_t instanceCount;
		std::uint32_t firstIndex;
		std::int32_t vertexOffset;
		std::uint32_t firstInstance;
		std::uint32_t sequence;    // the resident sequence's index
		std::uint32_t objectWord;  // with its per-frame bits
	};
	static_assert(sizeof(IndirectDraw) == 32 && offsetof(IndirectDraw, indexCount) == 4 && offsetof(IndirectDraw, sequence) == 24);  // must match DCLFDrawSequence

	/** @brief A frame slot's draw buffer as its recordings read it. */
	struct IndirectBuffer
	{
		rhi::CommandSignatureHandle signature{};  // the plain one, for every layout
		// The instance word signature (D3D12 below Shader Model 6.8), made per bound layout because it names a root signature. Else null.
		rhi::CommandSignatureHandle (*signatureFor)(rhi::PipelineLayoutHandle a_layout) = nullptr;
		std::uint64_t argumentsOffset = 0;  // the plain signature starts past the instance word
		rhi::ResourceHandle buffer{};
		RenderGraph::BufferRef ref;  // the draws' buffer view ref, which the pulled stages read their records through
		// Draws past capacity are not written. Those batches draw directly, and the slot's next frame gets a larger buffer.
		IndirectDraw* mapped = nullptr;
		std::uint32_t capacity = 0;
		std::atomic<std::uint32_t>* cursor = nullptr;
	};

	/** @brief Which pass a recording is in, which decides the pipeline a lane's bucket binds. */
	enum class RecordPass : std::uint8_t
	{
		Depth,       // the Z pipelines (IndirectState::zPipelines by zGroups): main depth, reflection's faces, first person's depth
		Colour,      // the plain colour pipelines (plainColour): main colour (EQUAL), the decals' second pass, forward
		DecalDepth,  // the opaque decals' depth pass (plainDepth)
		Shadow,      // a shadow view or occlusion map (ShadowIndirectState::pipelines)
		Refraction,  // the refraction normals (IndirectState::refraction)
		WaterMask,     // deferred range 0's pipelines (IndirectState::deferred), each with its stencil reference
		WaterSurface,  // deferred range 1's
	};

	/** @brief A draw family's main draw this frame. Null pipelines mean it is not drawn. */
	struct FamilyDraw
	{
		std::shared_ptr<const void> pipelines;
		RenderGraph::BufferRef row;
		rhi::ResourceHandle arguments{};
	};

	/** @brief What a site's recordings bind. Immutable while read. Handles stay valid while `main` and `shadow` hold their set versions. */
	struct RecordBindings
	{
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		const IndirectState* main = nullptr;          // the frame's (FrameIndirectState or the revision's catalog's)
		const ShadowIndirectState* shadow = nullptr;  // the frame's shadow set (shadow views only)
		rhi::ResourceHandle indexPool{};              // the index pool's indices (R16, the whole buffer)
		// Per geometry slot, the first index of its range as of the frame's first drawing commit. kNoRange is not drawn.
		std::span<const std::uint32_t> poolFirsts;
		std::array<std::uint32_t, kFramePushWords> framePush{};
		// A shadow view's push words are addresses of its latched blocks.
		std::array<std::uint32_t, kShadowPushWords> shadowPush{};
		// Per key slot, the ShadowIndirectState::pipelines index. Lookups::kNone skips the draws.
		std::span<const std::uint32_t> shadowMapRow;
		std::uint64_t lodFadeFrame = 0;
		// D3D12 pushes the ref instead, since no layout turns the address into a view there.
		RenderGraph::BufferRef lodFadeFrameRef;
		// The frame's animated material rows, 0 for the installed rows.
		std::uint64_t animatedRows = 0;
		RenderGraph::BufferRef animatedRowsRef;
		// Terrain Blending's depth offset on every draw, for a terrain group 1 twin's recording.
		bool offsetDepth = false;
		// For each forward draw's sort key, as BuildDrawsCS's StageForwardKey makes it. Indexed by object.
		std::span<const ForwardSortRow> sortRows;
		std::array<float, 16> viewProj{};
		std::span<const FamilyDraw> families;
	};

	/** @brief One recording's state and what BeginList bound. */
	struct RecordContext
	{
		rhi::CommandList& list;
		const ViewBatchSet& set;
		ViewType view = ViewType::Main;
		std::uint32_t viewIndex = 0;
		Site site = Site::Depth;
		bool depthPass = false;  // with view Decal, the decal depth pass
		const RecordBindings& bindings;
		FrameCounters* counters = nullptr;  // atomic, parallel recordings share it. Null is not counted.
		// Indirect mode. Batches draw their runs from this buffer, and direct draws use the pulled pipelines too. Null draws directly.
		const IndirectBuffer* indirect = nullptr;
		// First person's colour pass has only two targets, so its draws keep their view pipelines (VUID-vkCmdDrawIndexed-colorAttachmentCount-06179).
		bool mainTargets = true;

		RecordPass pass = RecordPass::Colour;
		rhi::PipelineLayoutHandle layout{};  // an indirect run's D3D12 signature is made for it
		std::uint32_t boundBucket = ~0u;
		rhi::PipelineHandle boundPipeline{};
		std::uint64_t boundVertexLayout = 0;
		std::uint32_t stencilRef = ~0u;  // the water passes' last reference set
		// Whether colour or depth draws use the keys' own pipelines under the main layout, and whether that layout is bound.
		bool pushedColour = false;
		bool mainLayoutBound = false;
		std::uint32_t drawn = 0;
		// sameBinds counts binds of the pipeline already bound, by another bucket of the same pipeline.
		std::uint32_t binds = 0, sameBinds = 0, pushes = 0, indirects = 0;
		std::uint32_t countedBinds = 0, countedSameBinds = 0, countedPushes = 0, countedIndirects = 0;
		std::uint64_t indices = 0, countedIndices = 0;
		// Whether the last pushed draw words are a run's. A direct draw replaces them.
		bool runWordsPushed = false;
	};

	/** @brief The pass a view type's lanes draw in. */
	RecordPass PassOf(ViewType a_view, bool a_depthPass);

	/**
	 * @brief Binds what the list's draws share. A secondary inherits nothing, so each one begins with this.
	 * False when the pass's state is not valid, and then nothing may be recorded.
	 */
	bool BeginList(RecordContext& a_context);

	/**
	 * @brief Cull job, Indirect mode. Writes a_batch's draws and runs into a_buffer. Draws that do not fit are not written, and the
	 * recorder draws those lanes directly. Returns the indices written.
	 */
	std::uint64_t WriteIndirectDraws(CulledBatch& a_batch, const ViewBatchSet& a_set, ViewType a_view, bool a_depthPass, const RecordBindings& a_bindings, const IndirectBuffer& a_buffer, CullArena& a_arena);

	/** @brief Records a_batch's draws in lane order. Main colour leaves out the terrain group lanes. Cull-only lanes draw nothing. */
	void RecordBatch(RecordContext& a_context, const CulledBatch& a_batch);

	/**
	 * @brief Records batches nearest a_plane first, in 8 depth slabs sorted by bucket within each.
	 * Only for depth-only draws, whose result does not depend on order.
	 */
	void RecordSortedBatches(RecordContext& a_context, std::span<const CulledBatch* const> a_batches, const std::array<float, 4>& a_plane);

	/** @brief A terrain group's colour draws, called at the group's main range position in the second pass. */
	void RecordTerrainGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches);

	/**
	 * @brief The forward list's draws in the engine's blended order, merged across batches. Farther first, then a plain draw before
	 * a group's at equal depth, then by group, ordinal, object and first index, as ForwardSortCS ranks them.
	 */
	void RecordForward(RecordContext& a_context, std::span<const CulledBatch* const> a_batches);

	/** @brief A forward draw in RecordForward's order (StageForward). */
	struct StagedForwardDraw
	{
		std::uint32_t bucket = 0, sequence = 0, objectWord = 0;
	};

	/**
	 * @brief Any thread. RecordForward's draws in its order, into a_out, for RecordStagedForward. Reads a_bindings' sort rows,
	 * view-projection and pool firsts, so those must be final.
	 */
	void StageForward(const ViewBatchSet& a_set, const RecordBindings& a_bindings, std::span<const CulledBatch* const> a_batches, std::vector<StagedForwardDraw>& a_out);

	/** @brief Records the draws StageForward ordered. */
	void RecordStagedForward(RecordContext& a_context, std::span<const StagedForwardDraw> a_draws);

	/** @brief One decal group's draws in the decal list's order, called per group at the colour pass's positions. */
	void RecordDecalGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches);

	/**
	 * @brief The draw families' instanced indirect draws. Leaves the families' pipeline and push data bound, so call BeginList again
	 * before more DCLF draws.
	 */
	void RecordFamilyDraws(RecordContext& a_context);
}
