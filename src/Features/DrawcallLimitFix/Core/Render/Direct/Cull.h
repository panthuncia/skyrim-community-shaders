#pragma once

// The CPU-driven pipeline's cull. CullBatch runs BuildDrawsCS's per-input decisions on the CPU over a ViewBatch's lanes.
// CullFrame is the frame's CPU state those decisions read, and the *CullView builders make views from the GPU latches.
// The main camera's verdicts are kept for kAccumulated.
//
// There is no HZB, so CPU mode has no occlusion. The frustum test is a sphere against world planes, where the shader tests the
// sphere's AABB corners. The sphere test rejects at least what the corners do, so the CPU never draws what the GPU would not.
//
// Threads: CullBatch runs on any pool thread, a CullFrame is immutable once published, and KickCpuFrameState runs on the render thread.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <ORGModuleServices/RecordingPools.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace DCLF
{
	struct BindlessPlacement;
	struct EntrySpace;
	struct SceneTables;
}

namespace DCLF::Draws
{
	struct BuildDrawsLatch;
	struct SceneBuffers;
}

namespace DCLF::Direct
{
	/**
	 * @brief A cull job's output memory for one frame slot (OMS's bump arena). A CulledBatch stays put until Reset at the slot's
	 * retirement. One thread at a time.
	 */
	using CullArena = org::services::BumpArena;

	/** @brief A portal-strict point light's gates on the CPU. hold keeps the gate words alive for the cull. */
	struct StrictGates
	{
		std::span<const std::uint32_t> gates, atlas, objectGates;
		bool (*rejects)(const StrictGates& a_gates, std::uint32_t a_context, std::uint32_t a_object, const float a_centre[3], float a_radius) = nullptr;
		std::shared_ptr<const void> hold;
	};

	/** @brief The frame's strict gates source, registered once at install. rejects is null when no portal-strict context is published. */
	using StrictGatesSource = StrictGates (*)(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);
	void SetStrictGatesSource(StrictGatesSource a_source);
	/** @brief Render thread: the frame's strict gates from the registered source. The caller holds them for its culls. */
	std::shared_ptr<const StrictGates> FrameStrictGates(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);

	/** @brief PortalPrograms.hlsli's PortalTest on the CPU. Returns 1 in view, 0 culled, -1 no program. */
	using PortalTestFn = int (*)(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags);

	/** @brief PortalTestFn over many bounds of one program. a_rejected[a_lanes[k]] is set to 1 where the verdict is not 1, else 0. */
	using PortalTestBatchFn = void (*)(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float* a_x, const float* a_y, const float* a_z,
		const float* a_r, const std::uint16_t* a_lanes, std::uint32_t a_count, std::uint8_t* a_rejected);

	/** @brief A root slot's verdicts as the frame's cull reads them, from the CPU fade stage. */
	enum RootWord : std::uint32_t
	{
		kRootListed = 1u << 0,  // a listed root of a seeded generation
		kRootFading = 1u << 1,  // not fully faded in, so shadow views drop its casters
		kRootFadedOutOfOcclusion = 1u << 2,  // the occlusion maps' kCullFadeOnVisible
		kRootServiced = 1u << 3,  // this frame's OnVisible ran
		kRootDrawn = 1u << 4,  // ... and went on into the children
		kRootInView = 1u << 5,  // the root's own cull test passed
		kRootUpdated = 1u << 6,  // a verdict exists this frame
		kRootLodCopy = 1u << 7,  // crossing levels with int(blend * 31) > 0, so main views draw row L+1 less row L as the copy
		kRootLodShift = 8,  // bits 8-9: the LOD level, min(+0x152 & 0xF, 3), 0 without state
	};

	/** @brief The CPU state every cull of the frame reads. Published once a frame by the CPU fade stage and immutable after that. */
	struct CullFrame
	{
		std::uint32_t sceneFrame = 0;
		std::uint32_t stamp = 0;  // sceneFrame's low 28 bits
		std::uint64_t frameValuesSeq = 0;
		std::shared_ptr<const ViewBatchSet> set;
		// By object slot, the sampled sun entry sphere (row 7) and fade node (row 8).
		std::span<const std::array<float, 4>> sunEntry, fadeNode;
		// By root slot: the verdicts, the states the readers take, and each root's portal program.
		std::span<const std::uint32_t> rootWords;
		std::span<const FadeNodeState> rootStates;
		std::span<const std::uint32_t> rootGenerations;
		std::span<const std::uint32_t> rootPrograms;
		// The main camera's portal programs. Empty means the frustum alone.
		std::span<const std::uint32_t> programs;
		PortalTestFn portalTest = nullptr;
		PortalTestBatchFn portalTestBatch = nullptr;  // null means portalTest per lane
		// The main camera's fade eye and tree height limits. Main camera stamps read these whichever view culls them.
		std::array<float, 4> fadeEye{};
		std::array<float, 2> treeHeight{};
		// The tables and plain entries the frame's visibility batch is judged against.
		std::uint64_t publication = 0;
		std::shared_ptr<const void> retirement;
		std::shared_ptr<const SceneTables> tables;
		std::shared_ptr<const EntrySpace> candidates;
		bool visibilityReset = false;
		// False on an observer's frame, where the GPU's events report visibility instead.
		bool reportsVisibility = true;
		// The CPU frame's colour commit opens water sites when the set has water lanes (not on an observer's frame).
		bool waterSites = false;
		std::shared_ptr<const void> storage;  // owns every span above
		// Main camera batches still to cull, plus one for the publication. The last to finish diffs the verdicts.
		mutable std::atomic<std::uint32_t> pendingStamps{ 0 };
	};

	// ---- The frame state ----

	/**
	 * @brief Render thread, never blocks: arms the frame's CPU state. The CPU fade stage then publishes the CullFrame.
	 * An observer frame only reads, and FadeStateCS stays the fades' authority.
	 */
	void KickCpuFrameState(std::shared_ptr<const ViewBatchSet> a_set, bool a_observer = false);
	/** @brief What KickCpuFrameState armed. A null set means not armed. */
	struct CpuFrameArm
	{
		std::shared_ptr<const ViewBatchSet> set;
		std::uint32_t sceneFrame = 0;
		// The frame before was not armed, so the reported verdicts and the visibility decoder start again.
		bool visibilityReset = false;
		bool observer = false;
	};
	CpuFrameArm TakeCpuFrameArm();
	/** @brief Any thread: whether the frame's root states come from the CPU stage. */
	bool CpuRootStatesFor(std::uint32_t a_sceneFrame);

	/** @brief Producer thread only: hands the frame's sampled rows to the CPU fade stage. */
	void OnFrameValuesSampled(std::uint64_t a_seq, std::span<const BindlessPlacement> a_rows);
	/** @brief The rows OnFrameValuesSampled was given for a_seq, empty for none. */
	std::span<const BindlessPlacement> SampledRows(std::uint64_t a_seq);

	/** @brief Publishes the frame's CullFrame and runs continuations waiting for it. Producer thread. */
	void PublishCullFrame(std::shared_ptr<CullFrame> a_frame);
	/** @brief Lets go of the newest frame when no CPU frame runs, so its tables do not hold back the scene's retirements. */
	void DropCullFrame();
	/** @brief Any thread: runs a_then once a_sceneFrame's CullFrame or a later one is published. Lock-free, each continuation runs once. */
	void AfterCullFrame(std::uint32_t a_sceneFrame, std::function<void(const std::shared_ptr<const CullFrame>&)> a_then);
	/** @brief Any thread: the newest published CullFrame, null before the first. */
	std::shared_ptr<const CullFrame> LatestCullFrame();

	// ---- The cull ----

	/**
	 * @brief Any pool thread: culls a_batch for a_view into a_arena with the live spheres of a_parity. The caller sets `last`.
	 */
	const CulledBatch& CullBatch(const ViewBatch& a_batch, std::uint32_t a_parity, const CullView& a_view, const CullFrame& a_frame, CullArena& a_arena);

	const CulledBatch& ColourObjectWords(const CulledBatch& a_depth, const CullView& a_colour, const CullFrame& a_frame, CullArena& a_arena);

	// ---- The views, from the latches the GPU path writes at the same point ----

	/** @brief Any view from its BuildDrawsLatch. */
	CullView CullViewOfLatch(ViewType a_type, std::uint32_t a_viewIndex, const Draws::BuildDrawsLatch& a_latch);
	/** @brief The main camera's per-frame view, from the depth segment's latch. */
	CullView MainCullView(const Draws::BuildDrawsLatch& a_depthLatch);
	/** @brief A colour segment view. The caller holds the latched cascades and shadow volumes. */
	CullView ColourCullView(ViewType a_type, const Draws::BuildDrawsLatch& a_colourLatch, std::span<const Draws::GpuCascade> a_cascades,
		std::span<const Draws::GpuShadowVolume> a_volumes);
	/** @brief A shadow view from its slot's latch, with the sun entry processes for a sun view. */
	CullView ShadowCullView(ViewType a_type, std::uint32_t a_slot, const Draws::BuildDrawsLatch& a_latch, std::span<const Draws::SunEntryProcess> a_sunEntry);
	/** @brief An occlusion map view. */
	CullView OcclusionCullView(ViewType a_type, std::uint32_t a_view, const Draws::BuildDrawsLatch& a_latch);
	/** @brief A water reflection face. */
	CullView ReflectionCullView(std::uint32_t a_face, const Draws::BuildDrawsLatch& a_latch);
	/** @brief The first-person view. */
	CullView FirstPersonCullView(const Draws::BuildDrawsLatch& a_latch);
}
