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
	struct FadeInputsVaried;
	struct MainCullingSetup;
	struct ExtrasSending;
	struct ExtrasSent;
	template <class Store>
	struct StoreUpdated;
	struct ShadowBuilt;
	struct MainGraphPasses;

	// Culling counters, every run.
	void NoteCullLatch(const CullLatchWritten& a_written);
	void ReadCullCounters(const MainEpochDone& a_epoch);
	// Commit warnings and setup log, every run.
	void LogFadeInputsVaried(const FadeInputsVaried& a_varied);
	void LogCullingSetup(const MainCullingSetup& a_setup);
	// CS_DCLF_PARITY: set parity (SetParity.cpp).
	void CheckSetParity(const MainEpochDone& a_epoch);
	// CS_DCLF_PARITY. State class sub-ranges against the shape's class plan.
	void CheckSetClassParity(const MainEpochDone& a_epoch);
	// CS_DCLF_PARITY. Kept stores against rebuilds.
	void MirrorExtras(const ExtrasSending& a_sending);
	void CheckExtras(const ExtrasSent& a_sent);
	void CheckObjectRecords(const StoreUpdated<Draws::ObjectRecordStore>& a_updated);
	void CheckGeometryDraws(const StoreUpdated<Draws::GeometryStore>& a_updated);
	void CheckDrawBound(const StoreUpdated<Draws::DrawBoundStore>& a_updated);
	void CheckKeptShadow(const ShadowBuilt& a_built);
	// CS_DCLF_PARITY. Every resident slot's shading row sampled, every 60 colour epochs.
	void CheckShadingSampled(const SceneTables& a_tables);
}
