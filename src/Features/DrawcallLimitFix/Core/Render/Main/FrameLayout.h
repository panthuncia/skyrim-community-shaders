#pragma once

// A main segment's shape (PassFrame), the frame buffers, the sorts and the DGC preprocess state lists.
// The commits and the shape producer make them, and the render thread records against them. Self-contained so features can include it.

// volk must come before every Vulkan header.
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/PrefixSum.h"
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct ZBucketPlan;
	struct SetClassPlan;
	/**
	 * @brief A state class's sub-range of each colour range, and its sort keys. A capacity of 0 means the class draws
	 * nothing there and no call is made.
	 */
	struct SetClassRanges
	{
		struct Range
		{
			std::uint32_t first = 0, capacity = 0;
			bool operator==(const Range&) const = default;
		};
		std::array<Range, kClassRanges> ranges{};
		std::uint32_t keyBase = 0, size = 0;
		bool operator==(const SetClassRanges&) const = default;
	};
	/**
	 * @brief What one segment's passes record against, and nothing that changes every frame. Per-frame values live in the
	 * latch. Replaced only when it changes, so its identity is what a recording is prepared for.
	 */
	struct PassFrame
	{
		// The colour segment's state class sub-ranges, in creation order. Empty in the depth segment, whose draws are bucketed.
		std::vector<SetClassRanges> classes;
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
		// Each half of the forward range. BuildDraws appends to the staging half, the forward epoch draws the sorted half.
		std::uint32_t forwardCapacity = 0;
		std::uint32_t refractionCapacity = 0;
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
		std::uint32_t cullMode = 0;
		std::uint32_t width = 0, height = 0;  // the main pass viewport
		float minDepth = 0.0f, maxDepth = 1.0f;  // the engine uses [0, 0.999998]
		rhi::DescriptorHeapHandle resourceHeap{};
		rhi::DescriptorHeapHandle samplerHeap{};
		IndirectState indirect{};
		// The revision's draw family pipelines, null when a family is not drawn. Recording reads only these, so an epoch's
		// recording stays its revision's whatever the render thread has published since.
		std::array<std::shared_ptr<const FamilyPipelines>, Extensions::kDrawFamilies> families{};
		// A grown layout gets a new latch block in the next set, so a frame in flight keeps reading its own.
		std::shared_ptr<const org::LatchBlock> latch;
		// One Z-prepass call per bucket. Phase 2's ranges are the same, offset by sequenceDraws.
		struct ZCall
		{
			std::uint32_t bucket = 0, first = 0, capacity = 0;
			rhi::PipelineHandle pipeline{};
			// A terrain group 1 bucket's near twin. Drawn into main depth with Terrain Blending's depth offset, then into its
			// terrain depth without.
			bool terrainNear = false;
			bool operator==(const ZCall& o) const
			{
				return bucket == o.bucket && first == o.first && capacity == o.capacity && SameHandle(pipeline, o.pipeline) && terrainNear == o.terrainNear;
			}
		};
		std::vector<ZCall> zCalls;
		LatchedList latched;
		// Not compared. What a commit writes its values against, which is a revision's shape that this commit did not make.
		MainLatchLayout latchLayout;
		std::shared_ptr<const ZBucketPlan> zPlan;
		std::shared_ptr<const SetClassPlan> classPlan;

		bool SameShape(const PassFrame& o) const
		{
			return zCalls == o.zCalls && latched == o.latched && classes == o.classes &&
			       terrainCapacity == o.terrainCapacity && forwardCapacity == o.forwardCapacity && refractionCapacity == o.refractionCapacity &&
			       sequenceDraws == o.sequenceDraws &&
			       sequenceDecals == o.sequenceDecals && cullMode == o.cullMode &&
			       width == o.width && height == o.height &&
			       minDepth == o.minDepth && maxDepth == o.maxDepth && SameHandle(resourceHeap, o.resourceHeap) &&
			       SameHandle(samplerHeap, o.samplerHeap) && SameIndirect(indirect, o.indirect) && latch == o.latch;
		}
	};


	// A CPU-written buffer the main pass reads as an SRV at t16 and up. The graph reads a copy, refilled every epoch from the CPU mirror.
	struct FrameBuffer
	{
		std::uint32_t textureRegister = 0;
		std::uint32_t stride = 0;
		std::uint32_t firstElement = 0;
		std::uint32_t elements = 0;
		std::shared_ptr<org::Buffer> copy;

		bool SameShape(const FrameBuffer& a_other) const
		{
			return textureRegister == a_other.textureRegister && stride == a_other.stride && firstElement == a_other.firstElement && elements == a_other.elements;
		}
	};

	/** @brief A content epoch per kept table, which a build trims that table's journal to. */
	struct TablesContentEpochs
	{
		std::uint64_t objects = 0, extras = 0, geometries = 0;
		bool operator==(const TablesContentEpochs&) const = default;
	};

	struct SortSequencesConstants
	{
		std::uint32_t stagingIndex;
		std::uint32_t ranksIndex;
		std::uint32_t offsetsIndex;
		std::uint32_t countIndex;
		std::uint32_t sequencesIndex;
		std::uint32_t sequenceStride;  // bytes
		std::uint32_t drawLimit;
		std::uint32_t padding;
	};
	constexpr std::uint32_t kSortSequencesConstantWords = sizeof(SortSequencesConstants) / 4;
	/**
	 * @brief ForwardSortCS.hlsl's push constants. BuildDraws appends visible forward draws to the staging half with their view
	 * depth in firstInstance, and the sort writes the sorted half the forward epoch draws.
	 */
	struct ForwardSortConstants
	{
		std::uint32_t sequencesIndex;  // the main sequences, or a view's
		std::uint32_t countIndex;
		std::uint32_t stagingBase;
		std::uint32_t capacity;  // slots per half
		std::uint32_t sequenceStride;  // bytes
		// With kCountOffset in options, the count is the word at countOffset bytes.
		std::uint32_t countOffset = 0;
		std::uint32_t options = 0;
		std::uint32_t padding = 0;
		static constexpr std::uint32_t kCountOffset = 1;
		// The sorted draw's firstInstance is its rank, so a forward bucket's plain draw finds its sequence by its instance.
		static constexpr std::uint32_t kRankInstance = 2;
	};
	constexpr std::uint32_t kForwardSortConstantWords = sizeof(ForwardSortConstants) / 4;
	constexpr std::uint32_t kForwardSortGroup = 64;  // must match ForwardSortCS.hlsl
	/** @brief The forward range's first slot in a colour shape's sequences, after phase 2's terrain ranges. */
	inline std::uint64_t ForwardStagingSlot(const PassFrame& a_frame)
	{
		return std::uint64_t(a_frame.sequenceDraws) + a_frame.terrainCapacity[0] + a_frame.terrainCapacity[1];
	}
	/** @brief The refraction range's first slot in a colour shape's sequences, after both forward halves. */
	inline std::uint64_t RefractionSlot(const PassFrame& a_frame)
	{
		return ForwardStagingSlot(a_frame) + 2ull * a_frame.forwardCapacity;
	}
	constexpr std::uint32_t kSortSequencesGroup = 64;  // must match SortSequencesCS.hlsl
	// The rank word holds the sort key in its top 12 bits and the rank below. A draw range past this is a hard failure
	// until ranks get a word of their own.
	constexpr std::uint32_t kSortRankLimit = 1u << 20;
	static_assert(DrawPipelines::kMaxPipelines <= (1u << 12));

	/**
	 * @brief Sorts the colour segment's sequences by pipeline. BuildDraws counts per sort key, PrefixSum turns counts into first
	 * slots, and SortSequencesPass writes each class's sequences grouped by pipeline. Shadow views are not sorted because it gained nothing.
	 */
	struct DrawSort
	{
		std::shared_ptr<org::Buffer> counts, offsets, blockSums;
		Versioned staging, ranks;
		std::shared_ptr<const PrefixSum::Programs> prefixSum;
		std::shared_ptr<const ComputeProgram> scatter;
		bool countsZeroed = false;  // render thread

		static constexpr std::uint32_t kKeys = DrawPipelines::kMaxPipelines;

		/** @brief With the programs queued at setup (SetupPrograms). Staging and ranks are sized for a_draws. */
		static std::shared_ptr<DrawSort> Create(std::shared_ptr<const PrefixSum::Programs> a_prefixSum, std::shared_ptr<const ComputeProgram> a_scatter, std::uint32_t a_draws)
		{
			auto sort = std::make_shared<DrawSort>();
			sort->prefixSum = std::move(a_prefixSum);
			sort->scatter = std::move(a_scatter);
			sort->counts = CreateWords(kKeys, true, "cs.dclf.sort-counts");
			sort->offsets = CreateWords(kKeys, true, "cs.dclf.sort-offsets");
			sort->blockSums = CreateWords(PrefixSum::Blocks(kKeys), true, "cs.dclf.sort-block-sums");
			// Sized exactly, since the main layout already grows by doubling.
			sort->staging = MakeVersioned(CreateWords(std::uint64_t(a_draws) * sizeof(DrawSequence) / 4, true, "cs.dclf.sort-staging"),
				{ .extent = { 0, sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
			sort->ranks = MakeVersioned(CreateWords(a_draws, true, "cs.dclf.sort-ranks"), { .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
			return sort;
		}

		void Register(org::RenderGraph& a_graph) const
		{
			// Staging and ranks are versioned and their passes declare them.
			for (const auto& buffer : { counts, offsets, blockSums })
				a_graph.RegisterResource(org::ResourceIdentifier(buffer->GetName()), buffer);
		}

		/**
		 * @brief The scan pass, which also clears the counts so they stay zero whether or not a build ran. An inactive execution
		 * must follow one whose counts nothing raised since.
		 */
		std::shared_ptr<org::RenderPass> ScanPass(std::function<bool(const org::PassPrepareContext&)> a_active = {}) const
		{
			PrefixSum::Desc scan{};
			scan.programs = prefixSum;
			scan.counts = counts;
			scan.offsets = offsets;
			scan.blockSums = blockSums;
			scan.elements = kKeys;
			scan.clearCounts = true;
			scan.active = std::move(a_active);
			return PrefixSum::CreatePass(std::move(scan));
		}

		template <class Uploads>
		void ZeroCountsOnce(Uploads& a_uploads)
		{
			if (countsZeroed)
				return;
			static const std::array<std::uint32_t, kKeys> zeros{};
			a_uploads(counts, zeros.data(), sizeof(zeros), 0);
			countsZeroed = true;
		}
	};

	struct PassStats;

	/**
	 * @brief Graphics lists, never submitted, that one site's explicit DGC preprocesses run against, one per run lease index.
	 * The preprocess must see exactly the state the execution will have, so each pass is set up on this list first.
	 */
	class PreprocessStates
	{
	public:
		static std::shared_ptr<PreprocessStates> Create(rhi::Device a_device, const char* a_site)
		{
			auto states = std::make_shared<PreprocessStates>();
			states->device = a_device;
			states->site = a_site ? a_site : "a site";
			// One allocator for the site's lists, since a command pool costs the driver about 2 MB. A site records on one thread at a time.
			if (rhi::Failed(a_device.CreateCommandAllocator(rhi::QueueKind::Graphics, states->allocator)) || !states->allocator) {
				logger::error("[DCLF] DGC preprocess: no state allocator for {}; its draws cannot execute", states->site);
				return nullptr;
			}
			// D3D12 cannot hold several open lists on one allocator, so each list keeps its own there.
			states->shared = states->allocator->SupportsSharedLists();
			return states;
		}

		// The run's list, reset and recording. Throws when it cannot be made, which fails the epoch.
		rhi::CommandList& Begin(const org::RunRef& a_run)
		{
			DCLF_ZONE("CS.DCLF.Record.RecyclePreprocessState");
			if (slots.size() <= a_run.lease)
				slots.resize(static_cast<std::size_t>(a_run.lease) + 1);
			auto& slot = slots[a_run.lease];
			if (!slot.list) {
				if (!shared && (rhi::Failed(device.CreateCommandAllocator(rhi::QueueKind::Graphics, slot.allocator)) || !slot.allocator))
					throw std::runtime_error(fmt::format("DGC preprocess: no state allocator for {}", site));
				auto& owner = shared ? allocator : slot.allocator;
				if (rhi::Failed(device.CreateCommandList(rhi::QueueKind::Graphics, owner.Get(), slot.list)) || !slot.list)
					throw std::runtime_error(fmt::format("DGC preprocess: no state list for {}", site));
			}
			slot.list->Recycle(slot.allocator ? slot.allocator.Get() : allocator.Get());
			return slot.list.Get();
		}

	private:
		struct Slot
		{
			rhi::CommandAllocatorPtr allocator;  // D3D12 only
			rhi::CommandListPtr list;
		};
		rhi::Device device;
		std::string site;
		bool shared = true;
		// Declared before the lists so it outlives them.
		rhi::CommandAllocatorPtr allocator;
		std::vector<Slot> slots;  // by lease index
	};

	struct IndexPool;
}
