#pragma once

#include <cstdint>

namespace DCLF::Policy
{
	/**
	 * @brief Questions where an observer changes what the main path does, such as a parity that needs the engine's own result.
	 * With every CS_DCLF_* parity and probe switch off, each answer is the normal path's. Policy.cpp derives them from Observation.
	 */

	/** @brief Run the scene pass inline at frame start on the render thread, where hook-reading observers keep their placement. False on the normal path. */
	bool ScenePassInline();

	/** @brief The stand-in walks every entry each frame, not only those it refreshes. False on the normal path. */
	bool WalkEveryEntry();

	/** @brief Filter the local light culls by DCLF's exclusion on a_frame, else the engine culls every candidate. True on the normal path. */
	bool FilterLightCulls(std::uint32_t a_frame);

	/** @brief Keep the engine's scene lists as DCLF's filter leaves them on a_frame, else the engine builds them whole. True on the normal path. */
	bool FilterSceneLists(std::uint32_t a_frame);

	/** @brief Shadow views copy the engine's per-frame block even where DCLF owns the camera. False on the normal path. */
	bool CaptureEngineShadowBlock();

	/** @brief Withhold the set's members from the engine's views, else both draw them. True on the normal path. */
	bool WithholdFromEngine();

	/** @brief Which cached verdict RecomputeCachedVerdicts asks about. */
	enum class Verdicts : std::uint8_t
	{
		Classification,  // the walk's cached classification
		Derivation,      // the accumulate phase's cached derivation
	};

	/** @brief Pass a_pass recomputes the cached verdicts as well as serving them, and compares the two. False on the normal path. */
	bool RecomputeCachedVerdicts(Verdicts a_verdicts, std::uint64_t a_pass);

	/** @brief Run the scene pass's evaluations on the pass's own thread whatever CS_DCLF_FANOUT asks, so mirror live checks stay there. False on the normal path. */
	bool SerialFanout();

	/** @brief The hidden witness reads every gated entry's hidden chain on a_pass. False on the normal path. */
	bool HiddenWitness(std::uint64_t a_pass);

	/** @brief Main builds reuse the resident region's cache, else every build is whole. True on the normal path. */
	bool ReuseResidentRegion();

	/** @brief A shadow build reuses its last exclusion when nothing moved on a_frame, else it builds in full. True on the normal path. */
	bool ReuseShadowExclusion(std::uint32_t a_frame);

	/** @brief The colour pass's alpha-tested draws record each pixel they shade. False on the normal path. */
	bool RecordFoliagePixels();

	/** @brief Freeze the trees' wind clocks. False on the normal path. */
	bool FreezeTreeWind();

	/** @brief Pass a_pass rebuilds the shadow sets whole after their incremental update and compares. False on the normal path. */
	bool RebuildShadowSets(std::uint64_t a_pass);

	/** @brief Check a slice of the tracked set against the live scene graph and drop entries whose detach was missed. False on the normal path. */
	bool ValidateTrackedSlice();

	/** @brief Producer a_serial also samples every mover and root of its plan, to check its moved-only sampling. False on the normal path. */
	bool SampleEveryMover(std::uint64_t a_serial);

	/** @brief Check bound records' slots after a pass that freed one or on a_pass, and neutralise a record naming a slot no member holds. False on the normal path. */
	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass);

	// ---- The snapshot builder's main build.
	/** @brief Build a_build checks its narrowed pair checks against a serial resolve, and a parallel region resync against a serial one. False on the normal path. */
	bool CheckBuilderPairs(std::uint64_t a_build);

	/** @brief Build a_build checks that every pipeline of a material packs its one row alike. False on the normal path. */
	bool CheckRowPacking(std::uint64_t a_build);

	/** @brief Check each parallel region resync slot by slot against the serial path. Resyncs are rare. False on the normal path. */
	bool CheckRegionResync();

	// ---- Publication pacing.
	/** @brief A publication made after deferred passes checks its tables and claims against the coordinator's. False on the normal path. */
	bool CheckPacedPublication(std::uint64_t a_publication);

	// ---- Placement deltas and walk scheduling.
	/** @brief Pass a_pass also schedules a dirty root's dependents that kept its motion verdict and checks each merge changes nothing. False on the normal path. */
	bool WitnessRootSchedule(std::uint64_t a_pass);

	/** @brief A geometry scan with stale or refreshed slots also scans every object slot and compares. False on the normal path. */
	bool CheckGeometryScan();

	// ---- The attach burst.
	/** @brief Pass a_pass counts mirror property and alpha records no geometry counts. False on the normal path. */
	bool CheckMirrorUses(std::uint64_t a_pass);

	/** @brief Check the category members' index against tracked entries on every removal of a cell or portal root, and on a_pass. False on the normal path. */
	bool CheckCategoryMembers(bool a_removal, std::uint64_t a_pass);
	/** @brief The tracked entries' ancestor keys against the mirror's chains. */
	bool CheckAncestorKeys(std::uint64_t a_pass);

	/** @brief The mirror checks its own apply every batch. False on the normal path. */
	bool CheckMirrorApply();
}
