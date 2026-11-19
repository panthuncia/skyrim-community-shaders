#pragma once

// Types for the shadow epoch, covering shadow maps and occlusion maps. Captured views come from the render thread hooks,
// shapes from the revision producer. Self-contained, because the shadow passes include it directly.

// volk must precede every Vulkan header in this translation unit, so Main/Resources.h comes first.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include <string>
// GrowableRows.h is not self-contained. It needs Versioned.h and <string> above.
#include "Features/DrawcallLimitFix/Core/Render/Payload/GrowableRows.h"

#include "RenderGraph/ComputeProgram.h"
#include <Render/LatchBlock.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi.h>

#include <d3d11.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	/** @brief A view the engine hook captured for this frame's shadow epoch. */
	struct PendingView
	{
		std::uint32_t viewId = 0, renderMode = 0, modeIndex = 0, targetIndex = 0, slice = 0;
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		std::uint32_t slot = 0;  // the placement's slot
		float minDepth = 0.0f, maxDepth = 1.0f;
		RE::NiPoint3 eye;
		bool hasViewProj = false;
		std::array<float, 16> viewProj{};
		// The engine's caster volume, when its culling process has one.
		float cullPlanes[6][4] = {};
		std::uint32_t cullPlaneMask = 0;
		// Taken at cull mode 1, which the Utility shader sets for every pass except two-sided ones.
		std::uint32_t rasterState = 0;
		// 0 for ordinary casters, 1 for the volumetric lighting copy, which draws only volumetric-only casters.
		std::uint32_t casterClass = 0;
		bool sunView = false;
		bool focus = false;    // draws only focusTarget's members
		const void* focusTarget = nullptr;
		std::uint32_t portalContext = 0;
		bool parabolic = false, parabolicBack = false, parabolicBothSides = false;
		std::array<float, 4> parabolicSphere{}, parabolicPlane{};
		float viewBlock[12] = {};  // PerTechnique: HighDetailRange, ParabolaParam, EyeDelta
		std::array<std::byte, 1024> perFrame{};
		std::uint32_t perFrameBytes = 0;
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
	};

	/** @brief One indirect draw with one pipeline over a range of a view slot's sequences. One per pipeline in the map row. */
	struct ShadowBucket
	{
		std::uint32_t pipeline = 0;  // index into ShadowIndirectState::pipelines
		std::uint32_t first = 0;     // range start, in sequences
		std::uint32_t capacity = 0;  // max draw count, grown by doubling

		bool operator==(const ShadowBucket&) const = default;
	};

	/** @brief One view of the shadow shape. */
	struct ShadowFrameView
	{
		std::uint32_t slot = 0;
		std::uint32_t modeIndex = 0;  // render modes 0xD, 0xE, 0xF and the occlusion modes
		std::uint32_t capacity = 0;   // ExecuteIndirect max count
		std::uint32_t x = 0, y = 0, width = 0, height = 0;  // viewport in its slice
		float minDepth = 0.0f, maxDepth = 1.0f;
		std::uint32_t target = 0;  // index into ShadowResources::depth
		std::uint32_t slice = 0;
		std::uint32_t sequenceDraws = 0;
		std::array<std::uint32_t, kShadowPushWords> push{};
		// Depth-only pipelines come first.
		std::vector<ShadowBucket> buckets;
		std::uint32_t rasterState = 0;  // the map row the buckets follow

		bool operator==(const ShadowFrameView&) const = default;
	};

	struct RowBuckets;
	/**
	 * @brief The shadow epoch's shape. Every view draws in one graph execution, because each execution has a fixed CPU cost.
	 * Placements come first, then the occlusion maps. Per-view matrices live in the latch, so the shape changes only with the layout or pipelines.
	 */
	struct ShadowFrame
	{
		rhi::DescriptorHeapHandle resourceHeap{};
		rhi::DescriptorHeapHandle samplerHeap{};
		ShadowIndirectState indirect{};
		std::vector<ShadowFrameView> views;
		std::shared_ptr<const org::LatchBlock> latch;
		// View blocks, zeroed counts, and the commit's SharedData and FeatureData.
		std::array<LatchedList, 3> latched;
		static constexpr std::size_t kCommitList = 2;
		// Not compared, because the latch and the buckets imply them.
		ShadowLatchLayout latchLayout;
		std::shared_ptr<const std::vector<RowBuckets>> rows;
		// Key slots are append-only per lookups instance, so this table only gains entries.
		std::shared_ptr<const std::vector<ShadowSlotEntry>> slotLayouts;
		// The views this variant records, as bits by index into views. Skipping unused views saves their indirect fetches
		// and target load and store. The first variant has all of them and the latch is written against it.
		std::uint64_t activeViews = ~0ull;

		bool SameShape(const ShadowFrame& o) const
		{
			// The version covers the pipelines the buckets bind.
			return latch == o.latch && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
			       SameHandle(indirect.layout, o.indirect.layout) && indirect.version == o.indirect.version && views == o.views && activeViews == o.activeViews;
		}
	};

	/** @brief The capacities the sealed shadow buffers hold, taking the smallest slot's. */
	struct ShadowCapacity
	{
		std::uint32_t sequenceDraws = 0;
		// A pipeline past these words waits for the buffer to grow.
		std::uint32_t bucketCountWords = 0;
		// A layout past these slots is not drawn.
		std::uint32_t viewSlots = 0;
	};

	/** @brief The shadow latch's first, small layout. */
	ShadowLatchLayout InitialShadowLatchLayout();

	/** @brief The shadow views' graph resources. Like the main path's per view slot, but without targets or an HZB. */
	struct ShadowResources
	{
		std::shared_ptr<org::Buffer> constants;
		Versioned visibility;
		// Inputs, geometries and material rows come from the installed payload.
		std::shared_ptr<SceneBuffers> scene;
		// One row per caster, partition and view family, laid out like the DrawSequence fields the vertex stage reads.
		// Each frame's culling writes the rows of what it draws.
		Versioned casterRows;
		Versioned casterFirsts;
		std::shared_ptr<org::Buffer> casterRowsUsed;
		std::shared_ptr<const ComputeProgram> assignCasterRows;
		rhi::CommandSignaturePtr rowsDispatchSignature;
		// Per view slot. Each slot must fit the scene's whole casting bound and there are many more slots than views,
		// so slots that never draw in the same frame share one buffer.
		std::vector<Versioned> drawArgs;
		// The slot whose arguments each slot uses. Slots past the end own theirs.
		std::vector<std::uint32_t> drawArgsOwner;
		/** @brief Whether this slot owns its arguments. A pass declares each buffer once. */
		bool OwnsDrawArgs(std::size_t a_slot) const { return a_slot >= drawArgsOwner.size() || drawArgsOwner[a_slot] == a_slot; }
		std::vector<std::shared_ptr<org::Buffer>> count;
		// Per view slot, one count word per bucket.
		std::vector<Versioned> bucketCounts;
		std::shared_ptr<IndexPool> pool;
		// Each view slot's b0 and b12 blocks, written by every commit.
		GrowableRows viewBlocks;
		std::uint64_t constantsAddress = 0;
		RenderGraph::BufferRef constantsRef;
		// D3D12 only. CBVs for SharedData and FeatureData, which a view's push words name. Null on Vulkan.
		std::shared_ptr<const FrameBlockViews> blockViews;
		std::array<std::uint32_t, 2> blockViewIndices{};
		// One dispatch culls every view of the epoch.
		std::shared_ptr<const ComputeProgram> buildDraws;
		// The engine's shadow map arrays, imported again when the engine recreates them.
		std::array<FrameAnchors::Texture, kShadowDepthTargets> depth;
		std::array<ID3D11Texture2D*, kShadowDepthTargets> depthTexture{};
		std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};
		// Targets the latched-copies pass declared.
		std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;
		rhi::CommandSignaturePtr dispatchSignature;
		// This frame's engine views, for the culling readback report.
		struct ViewLabel
		{
			std::uint32_t viewId = 0, renderMode = 0, slot = 0;
		};
		std::vector<ViewLabel> labels;
	};

	/** @brief The shadow shape variant a pass is being prepared for, or null. */
	inline std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const org::PassPrepareContext& a_preparation)
	{
		if (const auto* shapes = RevisionShapesOf(a_preparation))
			return shapes->shadow;
		return nullptr;
	}

	/** @brief Whether the culling dispatch builds this view, meaning the variant draws it. */
	inline bool ShadowBuildRow(const ShadowFrame& a_frame, std::size_t a_index)
	{
		return a_frame.views[a_index].modeIndex < kShadowModeCount && (a_index >= 64 || ((a_frame.activeViews >> a_index) & 1));
	}

	/** @brief The culling dispatch's Y size, one group row per built view. */
	inline std::uint32_t ShadowBuildRows(const ShadowFrame& a_frame)
	{
		std::uint32_t rows = 0;
		for (std::size_t i = 0; i < a_frame.views.size(); ++i)
			rows += ShadowBuildRow(a_frame, i) ? 1u : 0u;
		return rows;
	}
}
