#pragma once

#include "RenderGraph/BufferRef.h"

// The CPU-driven pipeline, an experiment beside the GPU one. Each snapshot makes per-view lists as fixed-size SoA batches.
// They are culled on the CPU per frame or once per snapshot, then drawn with direct draws recorded into DXVK's command buffer.
// This header is the contract between the batches, the cull, the recorder and the sites.
//
// It suspends two standing policies, only while dclf:pipeline=cpu: "DCLF owns all culling, on the GPU" and "No per-view lists".
// The GPU-driven graph also sends the forward family through this pipeline (GraphConditions::forwardDirect).

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace rhi
{
	class CommandList;
}

namespace org::services
{
	struct PublishedBufferVersion;
}

namespace DCLF
{
	struct DrawSequence;  // Core/Scene/Model/Records.h
}

namespace DCLF::Draws
{
	struct GpuCascade;
	struct GpuShadowVolume;
	struct SunEntryProcess;
}

namespace DCLF::Direct
{
	struct StrictGates;  // Cull.h

	/**
	 * @brief Default lanes per batch, always a multiple of 8 for the SIMD tests. 4096 halves Parallel mode's cost against 256 in
	 * heavy scenes. Larger leaves too few jobs and DXVK's CS thread waits for them.
	 */
	inline constexpr std::uint32_t kDefaultBatchLanes = 4096;

	/** @brief View types with their own list. Main's list serves both main segments, since without an HZB colour draws exactly depth's set. */
	enum class ViewType : std::uint8_t
	{
		Main,
		Decal,
		Forward,
		Refraction,
		Sun,
		Spot,
		Point,
		SkyOccluder,
		PrecipOccluder,
		Reflection,
		FirstPerson,
		kCount
	};
	inline constexpr std::size_t kViewTypes = static_cast<std::size_t>(ViewType::kCount);

	/** @brief Whether a view type is culled and recorded every frame or once per snapshot, accepting pops. Main is always PerFrame. */
	enum class Cadence : std::uint8_t
	{
		PerFrame,
		PerSnapshot
	};

	/** @brief CPU pipeline A/B switches (dclf:cpu-exp=<bits>). All on by default. Clearing a bit restores the old behaviour for comparison. */
	enum Experiment : std::uint32_t
	{
		kExpCullEarly = 1u << 0,  // CullBatch drops a lane outside the frustum before its other tests
		kExpPortalEarly = 1u << 1,  // stop the portal plane test at the deciding plane
		kExpParabolicPass = 1u << 3,  // paraboloid sphere test, four lanes at a time, before the lane tests
		kExpPrefillLive = 1u << 4,  // MakeViewBatches fills both live columns
		kExpPortalBatch = 1u << 2,  // main camera portal tests over the batch, four lanes at a time
		kExpIncremental = 1u << 5,  // view batch sets from the incremental scene lists instead of a full build
		// Frustum pass in the widest kernel the CPU has, AVX2 or AVX-512 with kExpCullAvx512. Off uses the SSE pass of four.
		kExpCullWide = 1u << 6,
		kExpCullAvx512 = 1u << 7,  // kExpCullWide may take AVX-512 (else AVX2 at most)
		// Read only the lanes the pass kept, sixteen bytes at a time. Off reads every lane.
		kExpCullPassedLanes = 1u << 8,
		// The portal batch test walks the program once per call, routing each 16-bound chunk to its operator. Off walks per group of 16.
		kExpPortalOpMajor = 1u << 9,
		// Draw colour lists with each key's pushed colour pipeline, pushing the sequence's 24 words. Off uses the pulled pipelines,
		// which cost 25-30 more registers on NVIDIA, one occupancy step. Vulkan only, D3D12 builds no unpulled pipelines.
		kExpPushedColour = 1u << 10,
		// The same for depth lists, except recordings with Terrain Blending's depth offset.
		kExpPushedDepth = 1u << 11,
		// Diagnostic. Times each per-lane section with a TSC read. Off by default because the reads cost a large part of what they time.
		kExpCullProfileLanes = 1u << 27,
		// Diagnostic. Checks kExpCullWide's verdicts against the SSE and paraboloid tests. Off by default.
		kExpCullWideCheck = 1u << 28,
		// Diagnostic. Checks every other incremental set against a full build.
		kExpIncrementalCheck = 1u << 29,
		// Diagnostic. Checks kExpPortalBatch's verdicts against the per-lane test. Off by default.
		kExpPortalCheck = 1u << 30,
		// Diagnostic. Times CullBatch's sections per view in TSC cycles. Off by default.
		kExpCullProfile = 1u << 31,
	};
	inline std::atomic<std::uint32_t> experimentBits{ ~(kExpCullProfile | kExpPortalCheck | kExpIncrementalCheck | kExpCullWideCheck | kExpCullProfileLanes) };  // constant-initialised, so the hot paths need no guard
	inline std::atomic<std::uint32_t>& ExperimentBits() { return experimentBits; }
	inline bool ExperimentOn(std::uint32_t a_bit) { return (experimentBits.load(std::memory_order_relaxed) & a_bit) != 0; }

	/** @brief How per-frame views are recorded (dclf:cpu-record). */
	enum class RecordMode : std::uint8_t
	{
		// Culled in parallel. The CS thread drains the batches in the site's callback and records them in list order.
		Serial,
		// Culled and recorded in parallel, one secondary per batch. The callback executes them in list order.
		Parallel,
		// Cull jobs write draws into the slot's draw buffer, and the callback records one indirect draw per pipeline run.
		// Decals, forward, refraction and first person stay direct.
		Indirect
	};

	enum LaneFlags : std::uint32_t
	{
		kLaneFadeTest = 1u << 0,    // the fade distance test (kObjectFadeTest)
		kLaneHeightTest = 1u << 1,  // the tree height test (kObjectHeightTest)
		kLaneSunTest = 1u << 2,     // sun-miss bit computed per draw (kObjectSunTest)
		kLaneMover = 1u << 3,       // its sphere is resampled per frame (the live column)
		kLaneForward = 1u << 4,
		kLaneDecal = 1u << 5,
		kLaneVolumetricOnly = 1u << 6,  // a shadow caster drawn only by the volumetric copy (kObjectVolumetricOnly)
		kLaneLandscapeLights = 1u << 7,
		kLaneCrossfadeCopy = 1u << 8,  // a lane that draws the cross-fade copy's partitions
		// Terrain Blending's held groups. Drawn in depth with the rest, but in colour by their own range after the decals.
		kLaneTerrainGroup1 = 1u << 9,
		kLaneTerrainGroup2 = 1u << 10,
		// Culled only for their kAccumulated verdicts. They have no sequences and come after every drawn lane.
		kLaneCullOnly = 1u << 11,
		// Only a focus view takes it.
		kLaneFocusOnly = 1u << 12,
	};

	/**
	 * @brief One fixed-size SoA batch of a view type's list. Immutable after the snapshot builder makes it, except the live spheres.
	 * Lanes are sorted by pipeline bucket, then object, so the recorder binds only on change. Lanes past count are never read.
	 */
	struct ViewBatch
	{
		std::uint32_t count = 0;
		std::uint32_t index = 0;  // in its list, the reorder window's key
		// Per lane:
		std::uint32_t* object = nullptr;  // bits 0-25 of the object word
		std::uint32_t* sequence = nullptr;
		std::uint16_t* sequenceCount = nullptr;   // its partitions' sequences, consecutive from sequence
		std::uint32_t* pipelineBucket = nullptr;  // the view's pipeline bucket (BucketsOf)
		std::uint32_t* flags = nullptr;           // LaneFlags
		std::uint32_t* fadeRoot = nullptr;        // 0 without one, else root + 1
		float* fadeDistance = nullptr;
		// Partition masks, byte L for LOD level L. A LOD lane has one sequence per set bit across its four bytes, in bit order,
		// and the cull picks the level's. Without LOD every sequence is drawn.
		std::uint32_t* partitions = nullptr;
		std::uint32_t* lodPartitions = nullptr;
		// Snapshot spheres, read by per-snapshot culls and static lanes.
		float *cx = nullptr, *cy = nullptr, *cz = nullptr, *r = nullptr;
		// Live spheres per frame parity. Movers are written by FrameValues' producer before it signals done.
		struct Spheres
		{
			float *cx = nullptr, *cy = nullptr, *cz = nullptr, *r = nullptr;
		};
		std::array<Spheres, 2> liveSpheres{};
		// Tombstones for a base batch shared by several sets. A set bit means this set's lane is gone. Null means every lane is live.
		const std::uint64_t* dead = nullptr;
	};

	/** @brief Where a mover's lanes are, for FrameValues' producer. */
	struct LaneRef
	{
		std::uint16_t view;   // ViewType
		std::uint32_t batch;  // in the view's list
		std::uint16_t lane;
	};

	/**
	 * @brief A view type's pipeline buckets. For main-family views a bucket is a main set index, for shadow views a shadow key slot,
	 * and Refraction has a single pipeline in bucket 0.
	 */
	struct ViewBuckets
	{
		std::vector<std::uint32_t> pipeline;  // index in the frame's pipeline catalog
		// Per bucket, the pipeline slot for views that use the slot's own pipeline. ~0u for shadow views.
		std::vector<std::uint32_t> pipelineSlot;
	};

	/**
	 * @brief A sequence's draw arguments for the recorder. The pool's first index for the slot is added at record time,
	 * because the pool can reassign ranges after the set was made. kNoRange draws nothing.
	 */
	struct DrawArgs
	{
		std::uint32_t indexCount = 0;
		std::uint32_t firstIndex = 0;
		std::uint32_t geometrySlot = 0;
	};

	/**
	 * @brief Persistent sequence arguments shared by a base's sets. Chunks are only appended and never moved,
	 * so a set can read its prefix while the builder appends the next.
	 */
	struct ArgsChunks
	{
		static constexpr std::uint32_t kShift = 12, kChunk = 1u << kShift, kChunks = 4096;  // 16M sequences
		std::unique_ptr<std::unique_ptr<DrawArgs[]>[]> chunks = std::make_unique<std::unique_ptr<DrawArgs[]>[]>(kChunks);
		const DrawArgs& At(std::uint32_t a_sequence) const { return chunks[a_sequence >> kShift][a_sequence & (kChunk - 1)]; }
	};

	/** @brief One snapshot's per-view lists. Immutable once posted. All batch columns live in one `storage` allocation. */
	struct ViewBatchSet
	{
		std::uint64_t serial = 0;
		std::uint32_t lanes = kDefaultBatchLanes;
		bool forwardOnly = false;  // only the forward family's lanes are filled
		std::array<std::vector<ViewBatch>, kViewTypes> lists;
		std::array<ViewBuckets, kViewTypes> buckets;
		// One resolved 136 B DrawSequence per drawn (object, partition, view class), append-only. The per-frame object word bits
		// are not in them, the recorder pushes that word per draw.
		std::uint64_t sequencesAddress = 0;
		RenderGraph::BufferRef sequencesRef;
		std::uint32_t sequenceCount = 0;
		// Rows in the sequences are offsets from these bases (kDirectFlagRelativeRows), pushed once per list.
		std::uint64_t pipelineRowsBase = 0, materialRowsBase = 0, shadowRowsBase = 0;
		RenderGraph::BufferRef pipelineRowsRef, materialRowsRef, shadowRowsRef;
		// Draw arguments: the first persistentSequences come from persistentArgs, the rest from sequenceArgs. Read through Args.
		std::shared_ptr<const ArgsChunks> persistentArgs;
		std::uint32_t persistentSequences = 0;
		std::vector<DrawArgs> sequenceArgs;
		// CPU copies for the pushed colour draws. Same split as the arguments. Read through Sequence.
		std::shared_ptr<const std::vector<DrawSequence>> ownSequences;
		// Per material row, its animated row or ~0u. A pushed draw is pointed at it in the frame's copy.
		std::vector<std::uint32_t> animatedRowOf;
		/** @brief A sequence's CPU copy, null past the set's. */
		const DrawSequence* Sequence(std::uint32_t a_sequence) const;
		/** @brief A sequence's arguments, null past the set's. */
		const DrawArgs* Args(std::uint32_t a_sequence) const
		{
			if (a_sequence < persistentSequences)
				return &persistentArgs->At(a_sequence);
			a_sequence -= persistentSequences;
			return a_sequence < sequenceArgs.size() ? &sequenceArgs[a_sequence] : nullptr;
		}
		// Decal group g covers lanes [decalGroupFirst[g], decalGroupFirst[g + 1]), counted as batch * lanes + lane. Empty means no decals.
		std::vector<std::uint32_t> decalGroupFirst;
		// A per-snapshot recording made against another pool serial is stale.
		std::uint64_t indexPoolSerial = 0;
		// Per geometry slot, the index buffer its sequences were resolved against. Held by `keep`, so no other buffer can take its address.
		std::vector<std::uint64_t> geometryIndexAddress;
		// object -> [moverLaneFirst[object], moverLaneFirst[object + 1]) in moverLanes. Only movers have lanes there.
		std::vector<std::uint32_t> moverLaneFirst;
		std::vector<LaneRef> moverLanes;
		std::shared_ptr<std::byte[]> storage;  // recycled block, returned when the set goes
		// Bound to the retirement point of every frame that draws with the set, so its backing is not reused while read.
		std::shared_ptr<const org::services::PublishedBufferVersion> sequencesVersion;
		// Holds what the sequences' addresses point into for as long as the set lives.
		std::shared_ptr<const void> keep;
		std::shared_ptr<const void> shared;
		std::vector<std::uint64_t> deadBits;
		// The first frame of each parity fills the live columns' static lanes once.
		mutable std::array<std::atomic<bool>, 2> liveFilled{};

		const std::vector<ViewBatch>& List(ViewType a_view) const { return lists[static_cast<std::size_t>(a_view)]; }
	};

	/** @brief The live sphere column for a frame, chosen by its producer sequence's parity. */
	inline std::size_t LiveParity(std::uint64_t a_frameValuesSeq) { return static_cast<std::size_t>(a_frameValuesSeq & 1u); }

	/**
	 * @brief Placement spheres as a FrameValues producer left them, so the snapshot builder never reads the producer's rows.
	 * A slot that moves or changed owner has no snapshot sphere, and its lanes become movers with radius -1.
	 */
	struct SnapshotSpheres
	{
		std::uint64_t seq = 0;  // the producer's
		std::vector<std::array<float, 4>> sphere;
		std::vector<const void*> owner;
		std::vector<std::uint8_t> moving;
	};

	/** @brief One view's culling inputs, the CPU equivalent of BuildDrawsLatch. */
	struct CullView
	{
		ViewType type = ViewType::Main;
		std::uint32_t viewIndex = 0;  // a shadow slot or a reflection face
		std::array<float, 16> viewProj{};  // eye folded in, so absolute positions project directly
		std::array<std::array<float, 4>, 6> planes{};  // world space, inward, pushed out by planeMargin
		std::uint32_t planeMask = 0;
		std::uint32_t cullFlags = 0;
		std::array<float, 3> eye{};
		float fadeEyeDistanceScale = 1.0f;
		float treeHeight = 0.0f;
		std::uint32_t focusRoot = 0;  // 0 none, else root + 1
		std::uint32_t frame = 0;
		bool frustumCull = true;  // false lets first person draw every member
		// Six normalised world-space clip planes, inward, tested as spheres and pushed out by planeMargin.
		std::array<std::array<float, 4>, 6> clipPlanes{};
		// Added to a bound's radius so rounding on absolute positions cannot cull it.
		float planeMargin = 0.0f;
		float treeHeightBase = 0.0f;  // BSTreeNode::OnVisible's height test base, +infinity when off
		float minCasterRadius = 0.0f;  // kCullMinTexels
		std::uint32_t strictContext = 0;  // portal-strict point light's atlas entry + 1, 0 when not gated
		bool sunTest = false;  // the colour pass's sun test (kSunTestOn)
		// Main camera lists culled only for their kAccumulated verdicts. Nothing is kept.
		bool stampOnly = false;
		// Latch-block spans the caller holds for the cull's duration.
		const Draws::GpuCascade* cascades = nullptr;
		std::uint32_t cascadeCount = 0;
		const Draws::GpuShadowVolume* shadowVolumes = nullptr;
		std::uint32_t shadowVolumeCount = 0;
		const Draws::SunEntryProcess* sunEntry = nullptr;
		std::uint32_t sunEntryCount = 0;
		const StrictGates* strict = nullptr;
	};

	/**
	 * @brief A batch's culled draws for one view. Lives in an arena owned by the job that culled it and reset when the frame slot retires.
	 * Consumers get a pointer, never a copy.
	 */
	struct IndirectRun;

	struct CulledBatch
	{
		const ViewBatch* batch = nullptr;
		std::uint32_t view = 0;
		std::uint32_t count = 0;
		const std::uint16_t* lanes = nullptr;
		const std::uint32_t* objectWords = nullptr;  // index, local shadow mask, cross-fade copy, sun miss
		// Per kept lane: bits 0-15 the sequences drawn, bits 16-31 those drawn as the LOD cross-fade copy. Null draws every sequence.
		const std::uint32_t* drawMasks = nullptr;
		rhi::CommandList* commands = nullptr;        // Parallel mode's secondary (null in Serial mode)
		bool last = false;                           // the view's list ends with it
		// Indirect mode's runs by part: 0 the pass's draws, 1 and 2 main colour's terrain groups. Null means the lanes draw directly.
		const IndirectRun* runs = nullptr;
		std::array<std::uint32_t, 4> runStart{};
		bool hasRuns = false;
	};

	/** @brief A site's queue of culled batches. Cull jobs push and the site's callback drains. */
	using CulledQueue = EventQueue<const CulledBatch*, 1024>;

	/** @brief One draw site per draw epoch it replaces, at the same trigger. */
	enum class Site : std::uint8_t
	{
		Shadow,       // shadow-maps.after: every shadow view and the occlusion maps
		Depth,        // depth.committed: main depth, the reflection's faces, first person's depth
		Colour,       // opaque.after: main colour and decals
		Forward,      // blended.draw: forward and refraction
		FirstPerson,  // fp.colour-render
		kCount
	};

	/** @brief The live mode (dclf:pipeline, dclf:cpu-record, dclf:cpu-cadence-<view>). */
	struct Mode
	{
		bool cpu = false;
		RecordMode record = RecordMode::Serial;
		std::array<Cadence, kViewTypes> cadence{};  // Main's is always PerFrame

		bool operator==(const Mode&) const = default;
	};

	/** @brief One frame's counters for the [CPU] report line. */

	struct FrameCounters
	{
		std::array<std::uint32_t, kViewTypes> batches{}, drawn{};
		std::array<double, kViewTypes> cullUs{};
		std::array<double, static_cast<std::size_t>(Site::kCount)> recordUs{};  // on DXVK's CS thread
		std::uint32_t secondaries = 0;
		std::uint32_t waits = 0;  // callbacks waiting for a batch not yet published
		// Per-snapshot recordings made and executed instead of a per-frame cull.
		std::uint32_t snapshotRecorded = 0, snapshotExecuted = 0;
		// Slots whose pool range moved to another buffer this frame. Their draws are skipped.
		std::uint32_t staleGeometry = 0;
		std::uint32_t binds = 0, sameBinds = 0, pushes = 0;
		std::uint32_t indirects = 0;
		// Indices submitted per site. Every draw is one instance, so triangles are a third.
		std::array<std::uint64_t, static_cast<std::size_t>(Site::kCount)> indices{};
		// Per view type, to compare with the engine's own passes.
		std::array<std::uint64_t, kViewTypes> viewIndices{};
	};
}
