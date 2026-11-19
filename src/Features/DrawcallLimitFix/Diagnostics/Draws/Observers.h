#pragma once

// The draws area's observers, subscribed by InstallDraws to the hook points of Events.h.

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"

namespace DCLF::Draws
{
	struct CounterReadback;
}

namespace DCLF::Diagnostics
{
	struct MainEpochDone;
	struct CullLatchWritten;
	struct FrameTextureUnresolved;
	struct FrameBufferUnfilled;
	struct FadeInputsVaried;
	struct ShortBuffersDrawn;
	struct MainCullingSetup;
	struct ExtrasSending;
	struct ExtrasSent;
	template <class Store>
	struct StoreUpdated;
	struct ShadowBuilt;
	struct ExclusionRebuilt;
	struct DrawsBuiltAhead;
	struct SnapshotSealed;
	struct CommitTargetStaged;
	struct RevisionShapesMade;
	struct IndexPoolUpdated;
	struct ShadowTargetPredicted;
	struct OwnShadowViewport;
	struct OwnShadowCamera;
	struct ShadowViewLatched;
	struct MainCullViewProj;
	struct FoliageRecordMade;
	struct MainGraphPasses;
	struct SnapshotAdopted;
	struct EpochCoverageDecided;
	struct MainEpochOutcome;
	struct AdoptedBound;

	// Culling counters, every run.
	void NoteCullLatch(const CullLatchWritten& a_written);
	void ReadCullCounters(const MainEpochDone& a_epoch);
	// Described once per register, every run.
	void DescribeUnresolvedFrameTexture(const FrameTextureUnresolved& a_unresolved);
	// Commit warnings and setup log, every run.
	void LogUnfilledFrameBuffer(const FrameBufferUnfilled& a_unfilled);
	void LogFadeInputsVaried(const FadeInputsVaried& a_varied);
	void LogShortBuffers(const ShortBuffersDrawn& a_drawn);
	void LogCullingSetup(const MainCullingSetup& a_setup);
	// CS_DCLF_BUILD_PARITY (BuildParity.cpp).
	void CheckBuildParity(const MainEpochDone& a_epoch);
	// CS_DCLF_SET_PARITY (SetParity.cpp).
	void CheckSetParity(const MainEpochDone& a_epoch);
	// CS_DCLF_SET_PARITY. State class sub-ranges against the shape's class plan.
	void CheckSetClassParity(const MainEpochDone& a_epoch);
	// CS_DCLF_PERSISTENT_PARITY. Frame value rows and palettes against the engine's. Render thread, waits for the producer.
	// a_fresh is the plan the walk made this frame, whose rows are the next frame's.
	void CheckFrameValues(const SceneTables& a_tables, const PlacementPlan* a_fresh);
	// CS_DCLF_CAPTURE_POINT_PARITY, at the frame's first deferred lighting draw.
	void CheckCapturePoint();
	// CS_DCLF_PERSISTENT_PARITY. Kept stores against rebuilds, and view masks.
	void MirrorExtras(const ExtrasSending& a_sending);
	void CheckExtras(const ExtrasSent& a_sent);
	void CheckObjectRecords(const StoreUpdated<Draws::ObjectRecordStore>& a_updated);
	void CheckGeometryDraws(const StoreUpdated<Draws::GeometryStore>& a_updated);
	void CheckDrawBound(const StoreUpdated<Draws::DrawBoundStore>& a_updated);
	void CheckKeptShadow(const ShadowBuilt& a_built);
	void CheckExclusionReuse(const ExclusionRebuilt& a_rebuilt);
	void CheckViewMasks(const DrawsBuiltAhead& a_built);
	// CS_DCLF_PERSISTENT_PARITY. Sealed set checks of embedded addresses, commit targets, recording resolves and shape ranges.
	void CheckSnapshotAddresses(const SnapshotSealed& a_sealed);
	void CheckCommitTarget(const CommitTargetStaged& a_staged);
	void CheckShapeCapacities(const RevisionShapesMade& a_made);
	void CheckNamedResolves();
	// Shadow epoch checks of the index pool, the views against the engine's, and the sun entry.
	void CheckIndexPool(const IndexPoolUpdated& a_updated);
	void CheckShadowTarget(const ShadowTargetPredicted& a_predicted);
	void CheckOwnViewport(const OwnShadowViewport& a_own);
	void CheckOwnCamera(const OwnShadowCamera& a_own);
	void CheckSunEntry(const ShadowViewLatched& a_latched);
	// CS_DCLF_TARGET_PROBE. The objects whose bound covers the pixel.
	void ProbeTarget(const MainCullViewProj& a_cull);
	// CS_DCLF_FOLIAGE_PARITY. The compare pass and its readbacks.
	void MakeFoliageCompare(const FoliageRecordMade& a_made);
	void AppendFoliagePasses(const MainGraphPasses& a_passes);
	// CS_DCLF_FRAME_TRACE. Every frame's adoption, coverage, main epochs, retirement point and the F10 marker.
	void TraceSnapshotAdopted(const SnapshotAdopted& a_adopted);
	void TraceEpochCoverage(const EpochCoverageDecided& a_decided);
	void TraceMainEpoch(const MainEpochOutcome& a_outcome);
	void TraceAdoptedBound(const AdoptedBound& a_bound);
	void TraceColourCounts(const Draws::CounterReadback& a_readback, std::uint32_t a_slot);
	// CS_DCLF_SET_PARITY. Every resident slot's shading row sampled, every 60 colour epochs.
	void CheckShadingSampled(const SceneTables& a_tables);
	// CS_DCLF_PERSISTENT_PARITY. The catalog's constant tables against ShaderCache's.
	void CheckConstantTables(const RE::BSShader& a_lighting, const Lookups& a_lookups);
}
