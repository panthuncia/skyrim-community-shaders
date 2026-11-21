#pragma once

#include "RenderGraph/BufferRef.h"

// Capacities and GPU layouts for the DCLF draws. They must match the HLSL, mainly BuildDrawsCS.
// Features include this header directly, so it must stay self-contained.

#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace DCLF::Draws
{
	constexpr const char* kExtensionId = "cs.dclf.main-opaque";
	constexpr const char* kShadowExtensionId = "cs.dclf.shadow";
	constexpr const char* kReflectionExtensionId = "cs.dclf.reflection";
	constexpr const char* kFirstPersonExtensionId = "cs.dclf.first-person";
	// Size of the point light table. Shaders get it as DCLF_POINT_LIGHT_TABLE_LIGHTS.
	constexpr std::uint32_t kPointLightTableLights = 128;
	// Shadow render modes 0xD plain, 0xE clamped, 0xF paraboloid, then one per occlusion view (render mode 0x1C).
	constexpr std::uint32_t kFirstOcclusionMode = 3;
	constexpr std::uint32_t kShadowModeCount = kFirstOcclusionMode + kOcclusionViews;
	constexpr std::uint32_t kOcclusionRenderMode = 0x1C;
	inline constexpr bool IsOcclusionMode(std::uint32_t a_mode) { return a_mode >= kFirstOcclusionMode; }
	inline constexpr std::uint32_t OcclusionOfMode(std::uint32_t a_mode) { return a_mode - kFirstOcclusionMode; }
	inline constexpr std::uint32_t OcclusionModeOf(std::uint32_t a_view) { return kFirstOcclusionMode + a_view; }
	// Occlusion views take the first view slots, then come the frame's shadow views. These capacities grow before each shadow epoch.
	constexpr std::uint32_t kInitialShadowViewSlots = 8;
	// Half again the draws, so the buckets can round up to powers of two.
	inline constexpr std::uint64_t ShadowSlotSequences(std::uint64_t a_draws) { return a_draws + a_draws / 2; }
	inline constexpr std::uint32_t OcclusionSlot(std::uint32_t a_view) { return a_view; }
	constexpr std::uint32_t kFirstShadowViewSlot = kOcclusionViews;
	constexpr std::uint32_t kInitialShadowKeySlots = 1024;
	constexpr std::uint32_t kInitialShadowRasterStates = 8;
	constexpr std::uint32_t kInitialSunProcesses = 8;
	constexpr std::uint32_t kInitialSunCascades = 4;
	constexpr std::uint32_t kInitialShadowVolumes = 8;
	constexpr std::uint64_t kShadowConstantBytes = 1ull << 20;
	// A view slot holds Utility PerTechnique (b0), then its VS_PerFrame copy (b12).
	constexpr std::uint64_t kShadowPerFrameOffset = 256;
	constexpr std::uint64_t kShadowViewSlotBytes = 256 + 1024;
	constexpr std::uint64_t kShadowFrameRecordOffset = 0;
	constexpr std::uint64_t kShadowArenaBlocksOffset = 1024;
	// The arena's blocks sit at fixed offsets so shapes can name them without a build.
	constexpr std::uint64_t kShadowZeroBlockBytes = 1024;
	// 256-byte aligned for D3D11 constant buffer offsets. The descriptor-buffer model needs 16.
	static_assert(kShadowPerFrameOffset % 256 == 0 && kShadowViewSlotBytes % 256 == 0 && kShadowFrameRecordOffset % 256 == 0 &&
				  kShadowArenaBlocksOffset % 256 == 0 && kShadowZeroBlockBytes % 256 == 0);
	// Depth targets: [0] cascades and spot lights, [1] point and focus lights, [2] the volumetric copy of each cascade, which
	// draws only batch group 15 (FUN_1414b44f0). [3] is depth target 10 as Skylighting swaps it in, [4] the precipitation mask.
	constexpr std::uint32_t kShadowLightTarget = 1;
	constexpr std::uint32_t kFirstOcclusionTarget = 3;
	constexpr std::uint32_t kShadowDepthTargets = kFirstOcclusionTarget + kOcclusionViews;
	inline constexpr std::uint32_t OcclusionDepthTarget(std::uint32_t a_view) { return kFirstOcclusionTarget + a_view; }
	inline constexpr bool IsOcclusionTarget(std::uint32_t a_target) { return a_target >= kFirstOcclusionTarget; }
	// Start sizes. Each grows before its epoch to the scene's draw bound. A bound over maxSequenceCount is a hard failure.
	constexpr std::uint32_t kInitialSequenceDraws = 16384;
	constexpr std::uint32_t kInitialDecalDraws = 2048;
	constexpr std::uint64_t kConstantAlignment = 256;  // uniform buffer address alignment (conservative)
	static_assert((kConstantAlignment & (kConstantAlignment - 1)) == 0 && kConstantAlignment % 16 == 0);
	constexpr std::uint32_t kColorTargets = 8;
	// First scene table capacities. They double as the scene grows. The object capacity sizes every per-object buffer.
	constexpr std::uint32_t kInitialObjects = 32768;
	constexpr std::uint32_t kInitialGeometries = 16384;
	constexpr std::uint32_t kInitialPoolIndices = 1u << 23;  // shadow index pool, 16 MB
	constexpr std::uint32_t kNoRecord = ~0u;
	constexpr std::uint32_t kNoSkip = ~0u;
	// BuildDrawsCS counter words. [0] is drawn, [1] culled, [2] tested. The rest are named below.
	constexpr std::uint32_t kCountWords = 53;
	// Casters a shadow view dropped because it ran out of caster rows.
	constexpr std::uint32_t kCountCasterRowsLostWord = 52;
	/** @brief Zeros uploaded to reset the counters. */
	inline constexpr std::uint32_t kZeroCounts[kCountWords] = {};
	// Sun test counters. Inputs tested, and those that missed every cascade.
	constexpr std::uint32_t kCountSunTestedWord = 23;
	constexpr std::uint32_t kCountSunMissedWord = 24;
	// Fade test counters. Inputs tested in the frustum, and those it hid.
	constexpr std::uint32_t kCountFadeTestedWord = 25;
	constexpr std::uint32_t kCountFadeHiddenWord = 26;
	// Decal counters. The CPU writes the group slot counts and the draws read them.
	constexpr std::uint32_t kCountDecalGroupWord = 19;  // group 1 at word 19, group 2 at word 20
	constexpr std::uint32_t kCountDecalsCulledWord = 21;
	constexpr std::uint32_t kCountDecalsTestedWord = 22;
	constexpr std::uint32_t kCountDecalLayerWord = 27;  // group 3 (the multi-index layers)
	// Terrain counters. The group words are tallies, and the per-class counts live in classCounts.
	constexpr std::uint32_t kCountTerrainGroupWord = 28;  // group 1 at word 28, group 2 at word 29
	constexpr std::uint32_t kCountTerrainNearTestedWord = 30;
	constexpr std::uint32_t kCountTerrainNearWord = 31;
	// Forward counters. Must also match ForwardSortCS.hlsl.
	constexpr std::uint32_t kCountForwardWord = 32;
	constexpr std::uint32_t kCountForwardTestedWord = 33;
	constexpr std::uint32_t kCountForwardInViewWord = 34;
	constexpr std::uint32_t kCountForwardOccludedWord = 35;
	constexpr std::uint32_t kCountRefractionWord = 36;
	constexpr std::uint32_t kCountRefractionTestedWord = 37;
	constexpr std::uint32_t kCountRefractionInViewWord = 38;
	constexpr std::uint32_t kCountRefractionOccludedWord = 39;
	// Draws dropped for lack of a state class sub-range, either unplanned or past capacity.
	// Both must stay 0. A draw is never drawn with another class's state.
	constexpr std::uint32_t kCountClassUnplannedWord = 40;
	constexpr std::uint32_t kCountClassOverflowWord = 41;
	// Length of depth phase 1's retest list. Phase 2's threads stop there.
	constexpr std::uint32_t kCountRetestsWord = 42;
	constexpr std::uint32_t kCountBelowTexelsWord = 43;
	// Words 44-51 are unused.
	// Phase 2's dispatch reset value. Phase 1 adds one group per 64 retests.
	inline constexpr std::uint32_t kRetestDispatchReset[4] = { 0, 1, 1, 0 };
	// Byte offset of a count word the indirect draws read. Must match BuildDrawsCS.hlsl.
	constexpr std::uint64_t kCountDrawnPhaseTwoBytes = 68;
	constexpr const char* kBuildDrawsShader = "DrawcallLimitFix/BuildDrawsCS.hlsl";
	constexpr const char* kIndexPoolShader = "DrawcallLimitFix/IndexPoolCS.hlsl";
	struct IndexPoolConstants
	{
		std::uint32_t latchIndex = 0, latchOffset = 0, copiesIndex = 0, poolIndex = 0;
	};
	constexpr std::uint32_t kIndexPoolConstantWords = sizeof(IndexPoolConstants) / 4;
	// One group per copy, in rows of IndexPoolCS's kGroupsX.
	constexpr std::uint32_t kIndexPoolGroupsX = 65535;
	// Start size of the visibility readback's root rows. It grows with the root states.
	constexpr std::uint32_t kInitialVisibilityRoots = 4096;
	// Emits main-camera visibility changes per root and per object. Threads cover root slots, then objects.
	constexpr const char* kVisibilityEventsShader = "DrawcallLimitFix/VisibilityEventsCS.hlsl";
	struct VisibilityEventsConstants
	{
		std::uint32_t rootsIndex = 0, statesIndex = 0, frameIndex = 0, frustumIndex = 0;
		std::uint32_t rootReportedIndex = 0, objectReportedIndex = 0, eventsIndex = 0, eventCapacity = 0;
		std::uint32_t rootSlots = 0, objectSlots = 0;
		std::uint32_t padding[2]{};
	};
	static_assert(sizeof(VisibilityEventsConstants) == 48);
	constexpr std::uint32_t kVisibilityEventsConstantWords = sizeof(VisibilityEventsConstants) / 4;
	constexpr std::uint32_t kVisibilityEventsGroup = 64;
	/** @brief A slot whose visibility changed since it was last reported. A root's generation is its listing's, an object's is 0. */
	struct VisibilityEvent
	{
		std::uint32_t slot;
		std::uint32_t generation;
		std::uint32_t kind;  // kVisibilityKind*
		std::uint32_t bits;  // kVisibilityBit*
	};
	static_assert(sizeof(VisibilityEvent) == 16);
	constexpr std::uint32_t kVisibilityKindRoot = 0, kVisibilityKindObject = 1;
	constexpr std::uint32_t kVisibilityBitVisible = 1u << 0;     // root: InView. object: stamped and not fade-hidden
	constexpr std::uint32_t kVisibilityBitInMargin = 1u << 1;    // reserved, not implemented
	constexpr std::uint32_t kVisibilityBitVerdict = 1u << 2;     // root: FadeStateCS updated it this frame
	constexpr std::uint32_t kVisibilityBitFadeHidden = 1u << 3;  // object: in view but hidden by its root's fade
	// Header: the appended count, which can pass capacity and sets how far the list grows, then the run's readback stamp.
	constexpr std::uint32_t kVisibilityEventHeaderWords = 4;
	constexpr std::uint32_t kInitialVisibilityEvents = 4096;
	inline constexpr std::uint64_t VisibilityEventBytes(std::uint32_t a_events)
	{
		return (kVisibilityEventHeaderWords + std::uint64_t(a_events) * 4) * sizeof(std::uint32_t);
	}

	// A stream vertex is a float4.
	constexpr std::uint32_t kStreamVertexBytes = 16;

	struct DrawInput
	{
		std::uint32_t pipelineIndex;  // main set index, or a shadow input's key slot
		std::uint32_t recordIndex;    // DrawBindings record
		std::uint32_t geometryIndex;  // GeometryDraw
		std::uint32_t flags;          // object flags, plus kInputDrawable for this epoch
		/** @brief The view bits it takes part in, and the key each view family draws it by. */
		struct ViewWords
		{
			std::uint32_t mask = 0;
			std::uint32_t shadowKey = ~0u;              // caster key slot, shared by every caster mode
			std::uint32_t occlusionKey[2]{ ~0u, ~0u };  // each occlusion map's key slot
		} view;
		// Lets the colour segment find the visibility the depth segment published.
		std::uint32_t objectIndex;
		std::uint32_t decalOrdinal;  // decals only: the slot in the group's range
		// Skins: bit i draws partition i. 0 draws the whole geometry, kNoPartitions nothing.
		std::uint32_t partitions;
		// A face shape's second vertex stream: the GeometryDraw holding its row, or ~0u.
		std::uint32_t streamIndex = ~0u;
		// Fade root slot or ~0u. Depth phase 1 drops the input while OnVisible stops its root, shadow views while it fades.
		std::uint32_t fadeRoot = ~0u;
		std::uint32_t shadowRow = 0;  // a caster's ShadowMaterialRow
		// LOD skins: byte L masks the partitions at level L. 0 uses partitions as is.
		std::uint32_t lodPartitions = 0;
		float fadeDistance = 0.0f;
	};
	static_assert(sizeof(DrawInput) == 64);
	// The depth segment culls every candidate but records only inputs with a bindings record.
	constexpr std::uint32_t kInputDrawable = 1u << 16;
	// View types an object takes part in. Must match BuildDrawsCS.hlsl.
	constexpr std::uint32_t kViewMainCull = 1u << 0;     // main-camera visibility candidate, the depth segment's inputs
	constexpr std::uint32_t kViewMain = 1u << 1;         // drawn by the main camera (the set's main phase)
	constexpr std::uint32_t kViewSunCaster = 1u << 2;    // the sun's cascades
	constexpr std::uint32_t kViewSpotCaster = 1u << 3;   // the other clamped views (spot lights)
	constexpr std::uint32_t kViewPointCaster = 1u << 4;  // the point lights' paraboloids
	// Bits 5 and 6 are the occlusion maps' (sky and precipitation). Bit 7 is the water reflection's.
	constexpr std::uint32_t kViewDecal = 1u << 8;        // a decal: the colour segment's fixed ordinal slots only
	constexpr std::uint32_t kViewForward = 1u << 9;      // a forward object: the colour segment's forward range only
	constexpr std::uint32_t kViewRefraction = 1u << 10;  // a refraction object (kRasterRefraction): the refraction range only
	// Bit 11 is First Person's.
	/** @brief Core's own view bits. A feature's view bit must be none of them. */
	constexpr std::uint32_t kCoreViewBits = kViewMainCull | kViewMain | kViewSunCaster | kViewSpotCaster | kViewPointCaster | kViewDecal |
	                                        kViewForward | kViewRefraction;
	// The main segments' views of the one main list: depth culls every candidate; colour draws members and every decal's slot.
	constexpr std::uint32_t kDepthViewBits = kViewMainCull;
	constexpr std::uint32_t kColourViewBits = kViewMain | kViewDecal | kViewForward | kViewRefraction;
	static_assert(offsetof(DrawInput, view) == 16 && offsetof(DrawInput, shadowRow) == 52 && offsetof(DrawInput, fadeDistance) == 60);
	// Main sequence buffer: phase 1 and colour draws, phase 2's range, then one decal sub-range per group and state class.
	// Each decal owns its ordinal's slot in every sub-range of its group, so decals keep engine order. Unused slots draw zero indices.
	constexpr std::uint32_t kDecalGroups = 3;
	/** @brief The count word a decal group's draws read their slot count from (each of the group's class sub-ranges). */
	constexpr std::uint32_t DecalCountWord(std::uint32_t a_range) { return a_range < 2 ? kCountDecalGroupWord + a_range : kCountDecalLayerWord; }
	/** @brief The main sequence buffer's slots for a_draws per draw range and a_decals per decal sub-range, a_decalRanges of them. */
	constexpr std::uint64_t SequenceSlots(std::uint32_t a_draws, std::uint32_t a_decals, std::uint32_t a_decalRanges)
	{
		return 2ull * a_draws + std::uint64_t(a_decalRanges) * a_decals;
	}
	// Terrain colour draws reuse phase 2's range once depth is done with it. Group 2 starts after group 1's capacity.
	constexpr std::uint32_t kTerrainGroups = 2;
	// Second-pass range slots. Decal groups are the fixed slots and terrain groups follow. BuildDrawsCS's layout fixes them.
	constexpr std::uint32_t kMainRangeSlotCount = kDecalGroups + kTerrainGroups;
	inline constexpr bool MainRangeFixed(std::uint32_t a_slot) { return a_slot < kDecalGroups; }
	/** @brief The count word a range slot's draws read. */
	inline constexpr std::uint32_t MainRangeCountWord(std::uint32_t a_slot)
	{
		return MainRangeFixed(a_slot) ? DecalCountWord(a_slot) : kCountTerrainGroupWord + (a_slot - kDecalGroups);
	}
	// The forward range follows the terrain ranges. BuildDraws appends to a staging half with view depth in firstInstance,
	// then ForwardSortCS fills the sorted half back to front per state class.
	constexpr const char* kForwardSortShader = "DrawcallLimitFix/ForwardSortCS.hlsl";
	// The refraction range follows the forward halves. It is drawn with one plain indexed indirect draw.
	// Marks a terrain group 1 bucket whose twin, the next bucket, takes the slot's near blocks.
	constexpr std::uint32_t kBucketTerrainPair = 1u << 31;

	// State class sub-ranges. Each device-generated call draws one (range, class) pair, since a set's pipelines may differ in shaders.
	// BuildDrawsCS routes each draw through the latch's class map and class table. Must match BuildDrawsCS.hlsl.
	constexpr std::uint32_t kClassRangeMain = 0;
	inline constexpr std::uint32_t ClassRangeOfSlot(std::uint32_t a_slot) { return 1 + a_slot; }
	constexpr std::uint32_t kClassRangeForward = 1 + kMainRangeSlotCount;
	constexpr std::uint32_t kClassRanges = kClassRangeForward + 1;
	constexpr std::uint32_t kClassHeaderWords = 8;
	constexpr std::uint32_t kClassWords = 16;
	static_assert(2 + 2 * kClassRanges <= kClassWords);
	/** @brief The classCounts word of class a_class's sub-range a_range. */
	inline constexpr std::uint32_t ClassCountWord(std::uint32_t a_class, std::uint32_t a_range) { return a_class * kClassRanges + a_range; }

#pragma pack(push, 4)
	struct GeometryDraw
	{
		RenderGraph::BufferRef vertexBufferRef;  // pulled by the vertex stages
		std::uint32_t vertexBufferSize;
		std::uint32_t vertexStride;
		std::uint64_t indexBufferAddress;  // device address
		std::uint32_t indexBufferSize;
		std::uint32_t indexCount;
		std::uint32_t firstIndex;
		std::uint32_t nextPartition;  // GeometryBuffers::nextPartition
	};
#pragma pack(pop)
	static_assert(sizeof(GeometryDraw) == 40);

	// Only values fixed for a pass across executions. Per-execution values live in BuildDrawsLatch, so the recorded dispatch never changes.
	struct BuildDrawsConstants
	{
		std::uint32_t latchIndex;  // the latch block's SRV
		std::uint32_t sequencesIndex;  // ShadowViews: the views' table
		std::uint32_t countIndex;      // ShadowViews: the table's rows
		// Row strides. An input's y names a material row in its low 20 bits and a pipeline row in its high 12.
		std::uint32_t materialRowStride;
		std::uint32_t phaseBits;        // the culling phase in bits 4-7 (CullFlags' phase field)
		std::uint32_t visibilityIndex;  // RWByteAddressBuffer: one uint per object in the tables
		std::uint32_t latchOffset;      // set at record time
		std::uint32_t frustumIndex;     // per object, the last frame it was in the frustum (depth phase 1 only)
		std::uint32_t hzbIndex;         // 0 when there is no HZB to test against
		std::uint32_t hzbSizePacked;    // mip 0: width in the low 16 bits, height in the high 16
		std::uint32_t hzbMips;
		// The sort by pipeline. 0 when appending directly, as phase 2 does.
		std::uint32_t sortCountsIndex;
		std::uint32_t sortStagingIndex;
		std::uint32_t sortRanksIndex;
		// Sequence buffer ranges, in sequences.
		std::uint32_t phaseTwoBase;  // ShadowCasterRows: the caster rows' capacity
		std::uint32_t decalBase;
		std::uint32_t decalStride;
		std::uint32_t pipelineRowStride;
		// Depth phase 1 and shadow views only.
		std::uint32_t fadeRootsIndex;
		// Each object's strict-light gate, for shadow views and the colour segment.
		std::uint32_t objectGatesIndex;
		// Bucket counts for shadow views and the depth phases.
		std::uint32_t bucketCountsIndex;
		// Each geometry's first index in the index pool, for shadow views and the depth segment.
		std::uint32_t poolFirstsIndex;
		std::uint32_t rootProgramsIndex;
		// Colour segment only.
		std::uint32_t forwardBase;
		std::uint32_t forwardCapacity;
		// Colour segment only. The refraction range starts at forwardBase + 2 * forwardCapacity.
		std::uint32_t refractionCapacity;
		// Colour segment only.
		std::uint32_t classCountsIndex;
		std::uint32_t memberQueriesIndex = 0, memberResultsIndex = 0, memberClaimsIndex = 0;
		// The depth phases' retest list. Phase 1 appends to it and grows phase 2's indirect dispatch by a group per 64 entries.
		// With none, phase 2 runs over every input.
		std::uint32_t retestsIndex = 0, retestDispatchIndex = 0;
	};
	static_assert(sizeof(BuildDrawsConstants) == 128);  // Vulkan's least maxPushConstantsSize: no room for another word
	constexpr std::uint32_t kBuildDrawsConstantWords = sizeof(BuildDrawsConstants) / 4;

	/** @brief One BuildDrawsCS dispatch's values for one execution, written into a latch block by the render thread.
	 * The dispatch reads its group count from the first three words. */
	struct BuildDrawsLatch
	{
		std::uint32_t dispatch[3];       // groups x, y, z
		std::uint32_t drawCount;         // inputs to cull
		std::uint32_t cullFlags;         // mode in bits 0-3, NoNearPlane at bit 9
		std::uint32_t visibilityStamp;   // marks the verdicts as this frame's
		std::uint32_t hzbUvScalePacked;  // rendered area over the area the HZB covers, 16-bit fixed point
		std::uint32_t cullPlaneMask;     // which of cullPlanes are tested; 0 for the main camera
		// Row-major for mul(M, v). Camera-relative, so a reader subtracts cullEye from a world position first.
		float viewProj[16];
		// A shadow view's caster volume as the engine culls it (NiCullingProcess::customCullPlanes), in world space.
		// Inside is where dot(normal, p) - constant >= 0.
		float cullPlanes[6][4];
		// A shadow view's pipeline map row, bytes into the latch block. 0 for the main camera.
		std::uint32_t pipelineMapOffset;
		// kSunTestOn makes the colour pass test kObjectSunTest inputs against the cascades.
		std::uint32_t sunState;
		// BSTreeNode::OnVisible's height test (base, limit). +infinity turns it off.
		float treeHeight[2];
		// The camera position and LOD factor (NiCamera +0x184), as BSFadeNode::OnVisible measures distance.
		float fadeEye[4];
		// The sun cascades, bytes into the latch block.
		std::uint32_t sunCascadeOffset;
		// A sun view's full-frustum processes. An input whose entry sphere is outside all of them is no sun caster. 0 skips the test.
		std::uint32_t sunEntryOffset;
		// The colour pass's local shadow lights. BuildDraws writes each input's Light Limit Fix shadow mask into its object word.
		std::uint32_t localShadowOffset;
		// A shadow view's bucket table. Each bucket is drawn by one plain indirect draw.
		std::uint32_t bucketTableOffset;
		// FadeStateCS's states from the frame before.
		std::uint32_t fadeStatesIndex;
		std::uint32_t phaseTwoBucketTableOffset;
		// Each pipeline slot's Z-prepass bucket, or each LOD slot's for a reflection face.
		std::uint32_t bucketMapOffset;
		// The frame's placement rows (bound, sun entry, fade node).
		std::uint32_t placementsIndex;
		// The installed payload's scene list, geometry table and row buffers. Shadow and occlusion views get the shadow rows.
		std::uint32_t inputsIndex;
		std::uint32_t geometriesIndex;
		// View types this dispatch draws. An input with none of them is skipped. 0 skips the test.
		std::uint32_t viewBits;
		// A focus view's target fade root slot + 1. 0 skips the test.
		std::uint32_t focusRoot;
		std::uint32_t materialRowsLo, materialRowsHi, pipelineRowsLo, pipelineRowsHi;
		// Fade seeds hold a root's values until its first FadeStateCS update.
		std::uint32_t fadeSeedsIndex;
		// The state class table and class map, bytes into the latch block. With 0, no colour draw is routed and each counts as unplanned.
		std::uint32_t classTableOffset;
		std::uint32_t classMapOffset;
		// The stage word of the feature pass before the build that asks for one, at byte 300. The shaders read it by byte offset,
		// so a second pass that needs one must grow this latch.
		std::uint32_t stageWord;
		// Terrain Blending's eye, with the near distance in w. Terrain group 1 draws within it go to the twin bucket. w = 0 turns it off.
		float terrainNear[4];
		// The frame's main-camera portal programs for the depth segment. 0 when there are none this frame.
		std::uint32_t portalProgramsIndex;
		// Strict-light gating for portal-strict light views and the colour segment. strictContext is the light's atlas entry + 1, or 0.
		std::uint32_t lightContextsIndex;
		std::uint32_t strictGatesIndex;
		std::uint32_t strictContext;
		// Each shadow key slot's vertex layout. BuildDraws stores it into the sequence, so one bucket can draw every layout.
		std::uint32_t slotLayoutsOffset;
		// Under kCullMinTexels, the least bound radius in world units a sun caster keeps.
		float minCasterRadius;
		std::uint32_t spare;  // 0
		// Address of the frame's copy of the animated material rows. A row whose header names one reads it. 0 for none.
		std::uint32_t animatedRowsLo, animatedRowsHi;
		// The frame's per-object rows, written from the latch by the feature pass that owns them. Null for none.
		RenderGraph::BufferRef frameRowsRef;
		std::uint32_t frameRowsCount;
		// The eye viewProj is relative to. Unlike fadeEye it is this view's own. w holds the CullPlaneMargin.
		float cullEye[4];
		// Buffer view refs for the material rows and the frame's animated rows.
		RenderGraph::BufferRef materialRowsRef;
		RenderGraph::BufferRef animatedRowsRef;
	};
	// focusRoot for a target that is not a listed root, so no input matches.
	constexpr std::uint32_t kFocusRootNone = 0xFFFFFFFFu;
	// Byte offset that BuildDrawsCS, FadeStateCS and PortalBatchCS read as kLatchPortalPrograms.
	constexpr std::uint32_t kLatchPortalProgramsOffset = 320;
	// Byte offset that CullEye.hlsli reads as kLatchCullEye.
	constexpr std::uint32_t kLatchCullEyeOffset = 368;
	static_assert(sizeof(BuildDrawsLatch) == 400 && offsetof(BuildDrawsLatch, materialRowsRef) == 384 && offsetof(BuildDrawsLatch, animatedRowsRef) == 392 &&
				  offsetof(BuildDrawsLatch, cullEye) == kLatchCullEyeOffset && offsetof(BuildDrawsLatch, animatedRowsLo) == 348 && offsetof(BuildDrawsLatch, frameRowsRef) == 356 && offsetof(BuildDrawsLatch, spare) == 344 && offsetof(BuildDrawsLatch, slotLayoutsOffset) == 336 && offsetof(BuildDrawsLatch, minCasterRadius) == 340 && offsetof(BuildDrawsLatch, portalProgramsIndex) == kLatchPortalProgramsOffset &&
				  offsetof(BuildDrawsLatch, lightContextsIndex) == 324 && offsetof(BuildDrawsLatch, strictContext) == 332 && offsetof(BuildDrawsLatch, terrainNear) == 304 && offsetof(BuildDrawsLatch, classTableOffset) == 292 &&
				  offsetof(BuildDrawsLatch, classMapOffset) == 296 && offsetof(BuildDrawsLatch, focusRoot) == 268 && offsetof(BuildDrawsLatch, fadeSeedsIndex) == 288 && offsetof(BuildDrawsLatch, inputsIndex) == 256 &&
				  offsetof(BuildDrawsLatch, viewBits) == 264 && offsetof(BuildDrawsLatch, materialRowsLo) == 272 &&
				  offsetof(BuildDrawsLatch, viewProj) == 32 && offsetof(BuildDrawsLatch, cullPlanes) == 96 &&
				  offsetof(BuildDrawsLatch, pipelineMapOffset) == 192 && offsetof(BuildDrawsLatch, sunState) == 196 &&
				  offsetof(BuildDrawsLatch, treeHeight) == 200 && offsetof(BuildDrawsLatch, fadeEye) == 208 &&
				  offsetof(BuildDrawsLatch, sunCascadeOffset) == 224 && offsetof(BuildDrawsLatch, sunEntryOffset) == 228 &&
				  offsetof(BuildDrawsLatch, localShadowOffset) == 232 && offsetof(BuildDrawsLatch, bucketTableOffset) == 236 &&
				  offsetof(BuildDrawsLatch, fadeStatesIndex) == 240 && offsetof(BuildDrawsLatch, phaseTwoBucketTableOffset) == 244 &&
				  offsetof(BuildDrawsLatch, bucketMapOffset) == 248 && offsetof(BuildDrawsLatch, placementsIndex) == 252 &&
				  offsetof(BuildDrawsLatch, materialRowsHi) == 276 && offsetof(BuildDrawsLatch, stageWord) == 300);
	/** @brief Radius margin of 4 to 8 ulps of the eye's largest component, so rounding never culls in absolute tests.
	 * The tests happen near the eye, so its size sets the rounding. */
	inline float CullPlaneMargin(float a_x, float a_y, float a_z)
	{
		return std::max({ std::abs(a_x), std::abs(a_y), std::abs(a_z) }) * 0x1p-21f;
	}

	/** @brief Folds the eye into a camera-relative view-projection so it takes absolute positions. Only column 3 changes. */
	template <class T>
	inline void FoldCullEye(const float* a_viewProj, const float* a_eye, T (&a_out)[16])
	{
		for (std::uint32_t row = 0; row < 4; ++row) {
			const float* m = a_viewProj + row * 4;
			for (std::uint32_t column = 0; column < 3; ++column)
				a_out[row * 4 + column] = static_cast<T>(m[column]);
			a_out[row * 4 + 3] = static_cast<T>(double(m[3]) - (double(m[0]) * double(a_eye[0]) + double(m[1]) * double(a_eye[1]) + double(m[2]) * double(a_eye[2])));
		}
	}

	// A pipeline slot with no bucket (BuildDrawsCS's kNoPipeline).
	constexpr std::uint32_t kNoBucket = 0xFFFFFFFFu;
	// A first-person forward slot's map word. The low bits hold its range's table entry.
	constexpr std::uint32_t kFirstPersonForwardBucket = 0x40000000u;
	constexpr std::uint32_t kSunTestOn = 1u << 31;

	/** @brief One view's row in the table read by BuildDrawsCS's single ShadowViews dispatch. */
	struct ShadowBuildView
	{
		std::uint32_t latchAt = 0;          // the view slot's BuildDrawsLatch, bytes into the frame slot's region
		std::uint32_t drawArgsIndex = 0;    // draw arguments, count words and bucket counts (UAVs)
		std::uint32_t countIndex = 0;
		std::uint32_t bucketCountsIndex = 0;
		std::uint32_t sequenceDraws = 0;    // the slot's draw capacity
		std::uint32_t casterRowsIndex = 0;  // shared caster rows (UAV), then each object's first row (SRV)
		std::uint32_t casterFirstsIndex = 0;
	};
	static_assert(sizeof(ShadowBuildView) == 28);
	/** @brief A shadow draw is its first instance, then DrawIndexedIndirect's arguments. The first instance names its caster row.
	 * D3D12's SV_InstanceID ignores the first instance, so the D3D12 signature also writes it into the draw push word. */
	constexpr std::uint32_t kShadowDrawBytes = 24;
	constexpr std::uint32_t kShadowDrawArgumentsOffset = 4;
	// Half again a draw (ShadowSlotSequences), rounded to a word.
	constexpr std::uint32_t kShadowDrawUnitBytes = 36;

	/** @brief One full-frustum sun process. The engine does not bound how many there are, so the region grows. */
	struct SunEntryProcess
	{
		std::uint32_t mask = 0;  // the active planes
		std::uint32_t pad[3]{};
		float planes[6][4]{};  // (normal, constant), inside where dot(normal, p) - constant >= 0

		bool operator==(const SunEntryProcess&) const = default;
	};
	/** @brief One sun cascade as BuildDraws tests it: plane masks (custom 0: none), 6 planes and 6 custom planes. */
	struct GpuCascade
	{
		std::uint32_t masks[2]{};
		std::uint32_t pad[2]{};
		float planes[12][4]{};
	};
	static_assert(sizeof(SunEntryProcess) == 112 && sizeof(GpuCascade) == 208);
	constexpr std::uint32_t kSunRegionHeader = 16;
	template <class Record>
	constexpr std::uint32_t SunRegionBytes(std::uint32_t a_records)
	{
		return kSunRegionHeader + a_records * static_cast<std::uint32_t>(sizeof(Record));
	}

	/** @brief The sun's entry rule as BuildDrawsCS applies it: outside every process. */
	inline bool OutsideSunEntryProcesses(std::span<const SunEntryProcess> a_processes, const float a_entry[4])
	{
		if (a_processes.empty())
			return false;
		for (const auto& process : a_processes) {
			bool outside = false;
			for (std::uint32_t p = 0; p < 6 && !outside; ++p) {
				if (!(process.mask & (1u << p)))
					continue;
				const float* plane = process.planes[p];
				outside = plane[0] * a_entry[0] + plane[1] * a_entry[1] + plane[2] * a_entry[2] - plane[3] < -a_entry[3];
			}
			if (!outside)
				return false;
		}
		return true;
	}

	/** @brief The sun's sphere test against one cascade (as BuildDrawsCS does it). */
	inline bool InSunCascade(const GpuCascade& a_cascade, const float a_centre[3], float a_radius)
	{
		for (std::uint32_t set = 0; set < 2; ++set) {
			const std::uint32_t mask = a_cascade.masks[set];
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(mask & (1u << p)))
					continue;
				const float* plane = a_cascade.planes[set * 6 + p];
				if (plane[0] * a_centre[0] + plane[1] * a_centre[1] + plane[2] * a_centre[2] - plane[3] < -a_radius)
					return false;
			}
		}
		return true;
	}
	/** @brief A shadow key slot's vertex layout and caster technique. BuildDrawsCS copies them into each draw's sequence. */
	struct ShadowSlotEntry
	{
		std::uint64_t vertexLayout = 0;
		std::uint32_t technique = 0;
		std::uint32_t reserved = 0;
	};
	static_assert(sizeof(ShadowSlotEntry) == 16);
	constexpr std::uint32_t kShadowSlotTechniqueKnown = 0x80000000u;

	/** @brief The shadow latch block's region per frame slot. Each dimension grows before the epoch. */
	struct ShadowLatchLayout
	{
		std::uint32_t viewSlots = 0, keySlots = 0, rasterStates = 0, sunProcesses = 0;
		std::uint32_t MapOffset() const { return viewSlots * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t MapRowBytes() const { return keySlots * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t MapRows() const { return kShadowModeCount * rasterStates; }
		/** @brief A view's map row: its mode's rows, then its state's (1 to rasterStates). */
		std::uint32_t MapRowOf(std::uint32_t a_mode, std::uint32_t a_state) const { return a_mode * rasterStates + (a_state - 1); }
		// Shared by every view and written once per frame slot.
		std::uint32_t SlotLayoutsOffset() const { return MapOffset() + MapRows() * MapRowBytes(); }
		std::uint32_t SunEntryOffset() const { return SlotLayoutsOffset() + keySlots * static_cast<std::uint32_t>(sizeof(ShadowSlotEntry)); }
		// (first, capacity) per bucket, at most one bucket per key slot.
		std::uint32_t BucketTableBytes() const { return keySlots * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t BucketOffset(std::uint32_t a_slot) const
		{
			return SunEntryOffset() + SunRegionBytes<SunEntryProcess>(sunProcesses) + a_slot * BucketTableBytes();
		}
		// The index pool copies' dispatch groups, then their count.
		std::uint32_t PoolOffset() const { return BucketOffset(viewSlots); }
		// The epoch's one culling dispatch for all its views. Written by each commit.
		std::uint32_t BuildDispatchOffset() const { return PoolOffset() + 4u * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		// The caster rows' dispatch, then the inputs' descriptor and count.
		std::uint32_t RowsDispatchOffset() const { return BuildDispatchOffset() + 4u * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t RowsInputsOffset() const { return RowsDispatchOffset() + 4u * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		// Copied to the view-blocks buffer by LatchedCopiesPass.
		std::uint32_t ViewBlockOffset(std::uint32_t a_slot) const
		{
			const std::uint32_t first = (RowsInputsOffset() + 4 * static_cast<std::uint32_t>(sizeof(std::uint32_t)) + 255u) & ~255u;
			return first + a_slot * static_cast<std::uint32_t>(kShadowViewSlotBytes);
		}
		std::uint32_t Bytes() const { return ViewBlockOffset(viewSlots); }
		bool operator==(const ShadowLatchLayout&) const = default;
	};
	/** @brief One cull volume of a local shadow light as BuildDraws tests it.
	 * A light with several shadowmap descriptors has one per descriptor, with the same mask bit. */
	struct GpuShadowVolume
	{
		std::uint32_t masks[2];     // the active planes: frustum (planes 0-5), custom (6-11)
		std::uint32_t maskBit;      // Light Limit Fix's ShadowBitMask bit (1 << maskIndex)
		std::uint32_t affectsLand;  // 1 when landscape takes this light too
		float sphere[4];            // the light's centre and radius
		float planes[12][4];        // (normal, constant), inside where dot(normal, p) - constant >= 0
		// A portal-strict point light's atlas entry + 1. 0 means not gated.
		std::uint32_t portalContext;
		std::uint32_t padding[3];
	};
	static_assert(sizeof(GpuShadowVolume) == 240);

	/** @brief CPU reference for the shadow mask BuildDraws gives a bound. Landscape objects take only affectsLand volumes. */
	inline std::uint32_t ShadowVolumeMaskOf(std::span<const GpuShadowVolume> a_volumes, bool a_landscape, const float a_center[3], float a_radius)
	{
		std::uint32_t mask = 0;
		for (const auto& volume : a_volumes) {
			if (a_landscape && !volume.affectsLand)
				continue;
			const float dx = a_center[0] - volume.sphere[0], dy = a_center[1] - volume.sphere[1], dz = a_center[2] - volume.sphere[2];
			const float reach = volume.sphere[3] + a_radius;
			if (dx * dx + dy * dy + dz * dz > reach * reach)
				continue;
			bool inside = true;
			for (std::uint32_t p = 0; p < 12 && inside; ++p) {
				if (!((volume.masks[p / 6] >> (p % 6)) & 1))
					continue;
				const auto& plane = volume.planes[p];
				inside = plane[0] * a_center[0] + plane[1] * a_center[1] + plane[2] * a_center[2] - plane[3] >= -a_radius;
			}
			if (inside)
				mask |= volume.maskBit;
		}
		return mask;
	}

	/** @brief The main latch block's region per frame slot. */
	struct MainLatchLayout
	{
		std::uint32_t cascades = 0;
		std::uint32_t shadowVolumes = 0;
		// One Z-prepass bucket per group of pipeline slots that share a depth pipeline.
		std::uint32_t buckets = 0;
		// The state classes the class table holds, and the set indices the class map holds a word for (IndirectState::classOf).
		std::uint32_t classes = 0, classMapWords = 0;
		static constexpr std::uint32_t CascadeOffset() { return static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t ShadowVolumeOffset() const { return CascadeOffset() + SunRegionBytes<GpuCascade>(cascades); }
		std::uint32_t BucketTableOffset() const { return ShadowVolumeOffset() + SunRegionBytes<GpuShadowVolume>(shadowVolumes); }
		std::uint32_t PhaseTwoBucketTableOffset() const { return BucketTableOffset() + buckets * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t BucketMapOffset() const { return PhaseTwoBucketTableOffset() + buckets * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		// Same as the shadow latch, for the depth commit's pool update.
		std::uint32_t PoolOffset() const { return BucketMapOffset() + buckets * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		// 16-byte aligned for the headers' Load4.
		std::uint32_t ClassTableOffset() const { return (PoolOffset() + 4 * static_cast<std::uint32_t>(sizeof(std::uint32_t)) + 15u) & ~15u; }
		std::uint32_t ClassMapOffset() const { return ClassTableOffset() + (kClassHeaderWords + classes * kClassWords) * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t Bytes() const { return ClassMapOffset() + (kClassHeaderWords + classMapWords) * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
	};
	// A clamped view (0xE) pancakes casters in front of its near plane onto it, so the near plane rejects nothing.
	constexpr std::uint32_t kCullNoNearPlane = 0x200;
	// Which caster class a shadow view draws. The volumetric copy draws only volumetric-only casters, other class-split views the rest.
	constexpr std::uint32_t kCullCastersOnly = 0x400;
	constexpr std::uint32_t kCullVolumetricOnly = 0x800;
	// A sun view applies the sun's entry rule.
	constexpr std::uint32_t kCullSunEntry = 0x1000;
	// Skylighting's occlusion size test. A bound radius <= 32 draws nothing.
	constexpr std::uint32_t kCullMinRadius = 0x2000;
	// Occlusion maps drop occluders under a fade root that OnVisible stops at, matching Precipitation::SetupMask's cull.
	// Set while the engine's fades are on (0x142032dfd).
	constexpr std::uint32_t kCullFadeOnVisible = 0x4000;
	// A sun caster whose bound projects to fewer texels than the threshold draws nothing.
	// DCLF is the only source of those casters, so a culled one casts no shadow at all.
	constexpr std::uint32_t kCullMinTexels = 0x8000;
	// A point light hemisphere (BSParabolicCullingProcess), tested against the light's sphere and plane instead of frustum corners.
	// Follows FUN_14151a1e0's codes (ParabolicSphereMask).
	constexpr std::uint32_t kCullParabolic = 0x80000;
	constexpr std::uint32_t kCullParabolicBack = 0x100000;
	constexpr std::uint32_t kCullParabolicBothSides = 0x200000;
	// Which key word a view reads, in bits 16-17. 0 is the pipeline word, 1 shadowKey, 2 and 3 occlusionKey[0] and [1].
	constexpr std::uint32_t kCullKeyShift = 16;
	constexpr std::uint32_t kCullKeyCaster = 1u << kCullKeyShift;
}
