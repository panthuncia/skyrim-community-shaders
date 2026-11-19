// Policy's answers, derived from the run's Observation. Any thread, since Observe() is immutable.
#include "Policy.h"

#include "Observation.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Policy
{
	bool ScenePassInline()
	{
		// Observers that read scene state from the engine's hooks while the scene work would run.
		const auto& o = Diagnostics::Observe();
		return o.scene || o.walk || o.capture;
	}

	bool WalkEveryEntry()
	{
		return Diagnostics::Observe().scene;
	}

	bool FilterLightCulls(std::uint32_t a_frame)
	{
		// The strict light parity needs the engine's own cull of every candidate on its frames.
		static const bool strictLights = RegisteredSwitchValue("CS_DCLF_STRICT_LIGHT_PARITY") == "1";
		return !((Diagnostics::Observe().scene || strictLights) && Diagnostics::ParityDue(a_frame));
	}

	bool FilterSceneLists(std::uint32_t a_frame)
	{
		// The scene list check compares the engine's whole lists with DCLF's on its frames.
		return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
	}

	bool CaptureEngineShadowBlock()
	{
		// The shadow view check compares the engine's block with DCLF's camera.
		return Diagnostics::Observe().claimsOrScene;
	}

	bool WithholdFromEngine()
	{
		// The capture check compares DCLF's draws with the engine's draws of the same objects.
		return !Diagnostics::Observe().engineDrawsAll;
	}

	bool RecomputeCachedVerdicts(Verdicts a_verdicts, std::uint64_t a_pass)
	{
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_pass, a_verdicts == Verdicts::Classification ? 17 : 13);
	}

	bool SerialFanout()
	{
		// The mirror check's live reads note their leases and differences on the pass's thread.
		return Diagnostics::Observe().mirror;
	}

	bool HiddenWitness(std::uint64_t a_pass)
	{
		return Diagnostics::Observe().walkOrScene && Diagnostics::ParityDue(a_pass);
	}

	bool ReuseResidentRegion()
	{
		return !Diagnostics::Observe().builds;
	}

	bool ReuseShadowExclusion(std::uint32_t a_frame)
	{
		return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
	}

	bool RecordFoliagePixels()
	{
		return Diagnostics::Observe().foliage;
	}

	bool FreezeTreeWind()
	{
		return Diagnostics::Observe().foliageFrozen;
	}

	bool RebuildShadowSets(std::uint64_t a_pass)
	{
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_pass, 15);
	}

	bool ValidateTrackedSlice()
	{
		// Safety net for missed detaches.
		return Diagnostics::Observe().scene;
	}

	bool SampleEveryMover(std::uint64_t a_serial)
	{
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_serial, 30);
	}

	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass)
	{
		// The normal path trusts the reference counts.
		return Diagnostics::Observe().scene && (a_slotsFreed || Diagnostics::ParityDue(a_pass));
	}

	// ---- The snapshot builder's main build.
	bool CheckBuilderPairs(std::uint64_t a_build)
	{
		// The build parity builds no region.
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_build, 29);
	}

	bool CheckRowPacking(std::uint64_t a_build)
	{
		return Diagnostics::Observe().builds || (Diagnostics::Observe().scene && Diagnostics::ParityDue(a_build, 31));
	}

	bool CheckRegionResync()
	{
		return Diagnostics::Observe().scene;
	}

	// ---- Publication pacing.
	bool CheckPacedPublication(std::uint64_t a_publication)
	{
		// Set parity passes run on the lane. Inline passes always publish, so the persistent parity does not need this.
		return Diagnostics::Observe().claims && Diagnostics::ParityDue(a_publication, 7);
	}

	// ---- Placement deltas and walk scheduling.
	bool WitnessRootSchedule(std::uint64_t)
	{
		// Every pass with a dirty root. Dirty roots come with attaches and detaches, so a cadence would miss most.
		return Diagnostics::Observe().scene;
	}

	bool CheckGeometryScan()
	{
		// Stale and refreshed slots are rare, so every scan with any.
		return Diagnostics::Observe().scene;
	}

	// ---- The attach burst.
	bool CheckMirrorUses(std::uint64_t a_pass)
	{
		return (Diagnostics::Observe().scene || Diagnostics::Observe().mirror) && Diagnostics::ParityDue(a_pass, 41);
	}

	bool CheckCategoryMembers(bool a_removal, std::uint64_t a_pass)
	{
		// Removals are rare so each is checked. The whole index is checked on a schedule.
		return Diagnostics::Observe().scene && (a_removal || Diagnostics::ParityDue(a_pass, 43));
	}

	bool CheckAncestorKeys(std::uint64_t a_pass)
	{
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_pass, 47);
	}

	bool CheckMirrorApply()
	{
		// Every batch, since a skipped replay or dropped update is a one-time loss.
		return Diagnostics::Observe().mirror || Diagnostics::Observe().scene;
	}
}
