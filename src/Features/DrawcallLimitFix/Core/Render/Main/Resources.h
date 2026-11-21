#pragma once

// The main epochs' graph resources, bindings, bucket plans and shape inputs. The render thread owns them.
// Their buffers grow to what the revision producer requires. Self-contained so features can include it.

// volk must come before every Vulkan header, so FrameLayout.h goes first in its own include block.
#include "Features/DrawcallLimitFix/Core/Render/Main/FrameLayout.h"
#include "RenderGraph/BufferRef.h"

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Draws.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/FrameAnchors.h"
#include <ORGModuleServices/ElasticCapacity.h>
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/PixelBuffer.h>
#include <rhi.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Draws
{
	struct ViewResources;
	class FrameBlockViews;
	struct ShadowResources;

	/**
	 * @brief How the main epochs lay out the sequences and Z-prepass buckets. It grows in powers of two only when the scene's
	 * draw bound exceeds it, so recorded calls change only then. A set is named only if its buffers hold this layout.
	 */
	struct MainLayout
	{
		// Z-prepass plain draws: each pipeline slot's bucket range per phase, and the count words the buckets take.
		// The depth commit zeroes the count words.
		std::vector<std::uint32_t> zBucketCapacity, zBucketFirst;
		std::uint64_t zBucketsLayout = 0;
		std::uint32_t buckets = 0;
		// Draws per draw range and per decal sub-range, and how many decal sub-ranges the sequence buffer holds.
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
		std::uint32_t decalRanges = kDecalGroups;
		// Per pipeline slot, the draws its objects can produce, rounded up to a power of two when they grow.
		// Colour calls change only when a slot grows or the slots' classes move.
		std::vector<std::uint32_t> slotCapacity;
		std::uint64_t slotsLayout = 0;
		// Phase 2's ranges in order: terrain groups, forward halves, then refraction. Each is the sum of its slots' capacities.
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
		std::uint32_t forwardCapacity = 0;
		std::uint32_t refractionCapacity = 0;
		bool operator==(const MainLayout&) const = default;
	};
	/** @brief The first main layout. CS_DCLF_TABLE_START=small starts with a few draws to test growth. */
	MainLayout InitialMainLayout();

	/**
	 * @brief Shrink windows for the main layout's capacities. A capacity the scene asked much less of goes back to the power of
	 * two past the window's peak, so the buffers sized from it shrink too.
	 */
	struct MainLayoutWindows
	{
		std::vector<org::services::ShrinkWindow> slots, zBuckets;  // per pipeline slot
		org::services::ShrinkWindow refraction, sequenceDraws, sequenceDecals, decalRanges;

		/** @brief Whether a window would shrink at its next ask. A kept revision is rebuilt so that it gets asked. */
		bool Due(const org::services::ShrinkPolicy& a_policy, org::services::ShrinkWindow::Clock::time_point a_now) const
		{
			const auto due = [&](const org::services::ShrinkWindow& a_window) { return a_window.Due(a_policy, a_now); };
			return std::ranges::any_of(slots, due) || std::ranges::any_of(zBuckets, due) || due(refraction) || due(sequenceDraws) || due(sequenceDecals) ||
			       due(decalRanges);
		}
	};

	/**
	 * @brief The main epochs' resources. A new display size makes a second one that shares every part the display does not size
	 * (Resources.cpp, CopySizeless), so a new field goes there unless the display sizes it.
	 */
	struct Resources
	{
		std::vector<FrameBuffer> frameBuffers;
		std::shared_ptr<SceneBuffers> scene;
		Versioned sequences;
		std::shared_ptr<org::Buffer> count;
		// A count word per state class and range. BuildDraws appends through them. Zeroed by the colour commit.
		Versioned classCounts;
		std::array<Versioned, 2> zBucketCounts;
		std::shared_ptr<IndexPool> pool;
		std::shared_ptr<const ComputeProgram> buildDraws, clearPortalQueries, preparePortalQueries;
		rhi::CommandSignaturePtr portalQuerySignature;
		winrt::com_ptr<ID3D11Buffer> countD3D11;
		// The per-frame constant blocks sit at fixed slots so a build can name them before their contents exist.
		std::shared_ptr<org::Buffer> frameConstants;
		// Targets declared for the latched copies. Anything else is staged.
		std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;
		std::uint64_t frameConstantsAddress = 0;
		RenderGraph::BufferRef frameConstantsRef;
		std::shared_ptr<const FrameBlockViews> frameBlocks;
		// The zeroed light data every bindless draw's b3 reads. Constant, staged once by the first commit.
		bool sharedLightZeroed = false;
		std::uint32_t targetCount = 0;
		// An engine target's slot when ORG owns it, else DCLF's own import.
		std::array<FrameAnchors::Texture, kColorTargets> native;
		FrameAnchors::Texture nativeDepth;
		// Terrain Blending's terrain depth, which the Z-prepass draws near terrain into. Null without that feature.
		FrameAnchors::Texture terrainDepth;
		// Null without the engine's refraction normals target.
		FrameAnchors::Texture refractionNormals;
		DXGI_FORMAT refractionFormat = DXGI_FORMAT_UNKNOWN;
		// The engine's screen-space shadow mask, which the colour draws read. Null without it.
		FrameAnchors::Texture shadowMask;
		// Set once both main and shadow resources exist.
		std::shared_ptr<ShadowResources> shadow;
		Versioned visibility;
		// Per object, the last frame stamp at which depth phase 1 found it inside the frustum, ignoring occlusion.
		// Never cleared because a stale stamp is simply not this frame's.
		Versioned frustum;
		// Inputs depth phase 1 left OccludedRetest, which phase 2 runs over. An object has one input, so the list never
		// holds more entries than there are visibility words.
		Versioned retests;
		// Phase 2's dispatch arguments. The depth commit resets them, phase 1 adds a group per 64 retest entries.
		std::shared_ptr<org::Buffer> retestDispatch;
		std::shared_ptr<PassStats> passStats;
		std::shared_ptr<PreprocessStates> preprocessMain;
		std::shared_ptr<const ComputeProgram> forwardSort;
		// Null without the depth pyramid's producer. nativeDepth is imported with SRV and DSV so the graph sees one depth resource.
		std::shared_ptr<org::PixelBuffer> hzb;
		std::shared_ptr<const void> hzbState;
		// The downsample's finished-group count. Zeroed by the first depth commit, then by the producer.
		std::shared_ptr<org::Buffer> hzbScratch;
		bool hzbScratchZeroed = false;
		std::uint32_t hzbWidth = 0, hzbHeight = 0, hzbMips = 0;
		std::uint32_t width = 0, height = 0;
		// A bit per frame graph buffer registered on the graph, such as LightLimitFix's t35-t37. Those are read at their
		// registers instead of the captured frame buffers.
		std::uint32_t graphBuffers = 0;
		rhi::CommandSignaturePtr dispatchSignature;
		// Null when the pipeline sort is off.
		std::shared_ptr<DrawSort> sort;
		// A view drawing this frame's depth commit, such as the reflection faces, draws only on its frame and only while the
		// adopted set is the one it was committed against.
		struct Committed
		{
			std::uint32_t frame = ~0u, inputs = 0;
			std::uint64_t adoptedSerial = 0;
		};
		Committed depthCommitted;
		// The views drawing from these resources, by view slot. A main epoch hosting view epochs makes their passes from this
		// at the graph's build. Weak because a view holds its main resources.
		std::array<std::weak_ptr<ViewResources>, static_cast<std::size_t>(Api::EngineView::Count)> views;
	};

	struct PassBindings
	{
		std::array<org::DeclaredViewToken, kColorTargets> targets{};
		org::DeclaredViewToken depth;
		org::DeclaredViewToken terrainDepth;
		org::ResourceBindingToken sequences, count;
		org::ResourceBindingToken classCounts;
		org::ResourceBindingToken pool, bucketCounts;
		std::array<org::ResourceBindingToken, Extensions::kDrawFamilies> familyArguments;
		std::array<RenderGraph::BufferRef, Extensions::kDrawFamilies> familyRows{};
	};

	// The main epochs' shape producers. A segment's shape depends only on what a scene revision names and on engine setup,
	// never on a frame's values. The commit writes those into the latch.

	/**
	 * @brief The Z-prepass's plain draws, one bucket per group of pipeline slots sharing a depth pipeline. A slot without a
	 * published pipeline maps to kNoBucket and BuildDraws drops its draws.
	 */
	struct ZBucketPlan
	{
		std::vector<std::uint32_t> map;
		std::vector<std::uint32_t> table;  // phase 1's (first, capacity) per bucket, then phase 2's
		std::vector<PassFrame::ZCall> calls;
		std::uint32_t Buckets() const { return static_cast<std::uint32_t>(calls.size()); }
	};
	void PlanZBuckets(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out);

	/**
	 * @brief The colour segment's state class sub-ranges. Each class gets a sub-range of each range, sized from the capacities of
	 * the slots in that class. classes are part of the shape, table and map go into the latch.
	 */
	struct SetClassPlan
	{
		std::vector<SetClassRanges> classes;
		std::vector<std::uint32_t> table;  // [classes, kClassRanges, 0, 0], then kClassWords per class
		// Header [set indices, forward, main, terrain and decal bucket counts, 0, 0, 0], then IndirectState::classOf. Then for each
		// bucket kind present, each set index's bucket (kNoBucket for none) and each bucket's (first, capacity).
		std::vector<std::uint32_t> map;
		std::uint32_t decalRanges = 0;
		// A plain draw bucket. The forward header word stays 0, since the CPU pipeline's forward site draws the forward family.
		struct PlainBucket
		{
			std::uint32_t set = 0, first = 0, capacity = 0;
			std::uint32_t group = 0;  // a terrain bucket's group (1 or 2), 0 otherwise
		};
		/**
		 * The main range's plain draws, drawn in any order since they test EQUAL against the Z-prepass.
		 * Empty while a main set index has no plain colour pipeline.
		 */
		std::vector<PlainBucket> main;
		/**
		 * The terrain groups' plain draws, group 1 first, drawn in any order.
		 * Empty while a terrain set index has no plain colour pipeline.
		 */
		std::vector<PlainBucket> terrain;
		/**
		 * The decal groups' plain draws. The sorted half ranks decals by engine ordinal so the order is
		 * stable. Empty while a decal set index lacks a plain colour pipeline or a needed Z-prepass one.
		 */
		std::vector<PlainBucket> decals;
		// classCounts' words in order: per class and range, then main, terrain and decal buckets.
		std::uint32_t Words() const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(main.size() + terrain.size() + decals.size());
		}
		std::uint32_t DecalCountWord(std::size_t a_bucket) const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(main.size() + terrain.size() + a_bucket);
		}
		std::uint32_t TerrainCountWord(std::size_t a_bucket) const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(main.size() + a_bucket);
		}
		std::uint32_t MainCountWord(std::size_t a_bucket) const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(a_bucket);
		}
	};
	void PlanSetClasses(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, SetClassPlan& a_out);

	/** @brief A main epoch's frame block sizes per stage and register, which its latched copies take. */
	struct FrameBlockSizes
	{
		std::array<std::uint32_t, kConstantBufferRegisters> vs{}, ps{};
		bool operator==(const FrameBlockSizes&) const = default;
	};

	/** @brief A main commit's latched copies, in write order and packed as LatchedUploads packs them. */
	std::vector<LatchedCopy> MainLatchedLayout(const Resources& a_resources, bool a_depthOnly, const FrameBlockSizes& a_blocks, std::uint32_t a_buckets,
		std::uint32_t a_classWords);
	/** @brief The bytes a layout takes of its latch block's slot. */
	std::size_t LatchedBytes(const std::vector<LatchedCopy>& a_copies);

	/** @brief The main pass's render area and depth range from the segment's last capture. */
	struct MainViewport
	{
		std::uint32_t width = 0, height = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
		bool operator==(const MainViewport&) const = default;
	};

	struct MainShapeInputs
	{
		bool depthOnly = false;
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
		std::shared_ptr<const SetClassPlan> classPlan;
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
		std::uint32_t forwardCapacity = 0;
		std::uint32_t refractionCapacity = 0;
		MainViewport viewport;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};
		std::array<std::shared_ptr<const FamilyPipelines>, Extensions::kDrawFamilies> families{};
		std::uint32_t cullMode = 0;
		std::shared_ptr<const org::LatchBlock> latch;
		MainLatchLayout latchLayout;
		std::vector<PassFrame::ZCall> zCalls;
		std::shared_ptr<const ZBucketPlan> zPlan;
		LatchedList latched;
	};
	/** @brief A segment's shape inputs from the layout and the versions a_sealed names. The caller fills the latch and descriptor heaps. */
	MainShapeInputs MainShapeInputsOf(const Resources& a_resources, const MainLayout& a_layout, const NamedVersions& a_sealed, bool a_depthOnly, std::uint32_t a_cullMode);
	std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in);

	/**
	 * @brief CS_DCLF_PASS_STATS=1: the main draw passes' pipeline statistics per run lease index, read back when the index
	 * records again. Up to kRanges indices get a range.
	 */
	struct PassStats
	{
		static constexpr std::uint32_t kRanges = 64;
		enum Kind : std::uint32_t
		{
			kDepth,
			kDepthPhaseTwo,
			kColour,
			kDecals,  // the colour segment's second pass
			kKinds
		};
		static constexpr std::uint32_t kFields = 4;  // IA vertices, IA primitives, VS invocations, PS invocations
		rhi::QueryPoolPtr pool;
		std::vector<std::shared_ptr<org::Buffer>> readback;
		std::vector<std::array<bool, kKinds>> written;
		std::vector<std::uint32_t> rangeOf;  // by lease index, UINT32_MAX for none
		std::uint32_t ranges = 0;
		std::mutex mutex;
		std::array<std::array<std::uint64_t, kFields>, kKinds> totals{};
		std::array<std::uint32_t, kKinds> samples{};

		static bool Enabled()
		{
			return Diagnostics::Observe().passStats;
		}

		/** @brief The run's range, given on first use. A leased run or one past kRanges gets UINT32_MAX and is not counted. */
		std::uint32_t RangeOf(const org::RunRef& a_run)
		{
			if (!a_run.Stable())
				return UINT32_MAX;
			const std::lock_guard lock(mutex);
			if (rangeOf.size() <= a_run.lease)
				rangeOf.resize(std::size_t(a_run.lease) + 1, UINT32_MAX);
			auto& range = rangeOf[a_run.lease];
			if (range == UINT32_MAX && ranges < readback.size())
				range = ranges++;
			return range;
		}

		/** @brief Reads the range's results from its last use, then resets its query. Call before the pass begins. */
		void Start(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
		{
			if (a_slot >= readback.size())
				return;
			const std::lock_guard lock(mutex);
			if (written[a_slot][a_kind]) {
				auto resource = readback[a_slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					const auto* values = static_cast<const std::uint64_t*>(mapped) + std::size_t(a_kind) * kFields;
					for (std::uint32_t f = 0; f < kFields; ++f)
						totals[a_kind][f] += values[f];
					++samples[a_kind];
					resource.Unmap(0, 0);
				}
			}
			if (a_kind == kColour && samples[kColour] >= Reports::Interval()) {
				Reports::PassStatsLine(totals, samples);
				totals = {};
				samples = {};
			}
			a_commands.ResetQueries(pool->GetHandle(), a_slot * kKinds + a_kind, 1);
		}
		void Begin(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
		{
			if (a_slot < readback.size())
				a_commands.BeginQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
		}
		void End(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
		{
			if (a_slot < readback.size())
				a_commands.EndQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
		}
		/** @brief Call after the pass ends. */
		void Resolve(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
		{
			if (a_slot >= readback.size())
				return;
			a_commands.ResolveQueryData(pool->GetHandle(), a_slot * kKinds + a_kind, 1, readback[a_slot]->GetAPIResource().GetHandle(),
				std::uint64_t(a_kind) * kFields * sizeof(std::uint64_t));
			const std::lock_guard lock(mutex);
			written[a_slot][a_kind] = true;
		}
	};

	inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef);

	/** @brief The pipelines a main pass draws a draw family with, null for nothing, and its row's ref. */
	struct FamilyPrepared
	{
		std::shared_ptr<const void> drawn;
		RenderGraph::BufferRef row;
	};

	struct PreparedDraws
	{
		PassStats* stats = nullptr;
		PreprocessStates* preprocess = nullptr;  // a list per run lease index
		std::array<std::uint32_t, kFramePushWords> framePushWords{};
		std::shared_ptr<const PassFrame> frame;
		bool terrainDepth = false;
		std::uint32_t targetCount = 0;
		bool phaseTwo = false;
		bool zPrepass = false;
		std::array<FamilyPrepared, Extensions::kDrawFamilies> families{};
	};
}
