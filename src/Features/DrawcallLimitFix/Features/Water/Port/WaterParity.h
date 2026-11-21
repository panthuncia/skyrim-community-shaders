#pragma once

// Phase 0's checks on the latch path (docs/development/plans/dclf-water-indirect.md, "Phase 0"), behind CS_DCLF_WATER_PARITY=1.
// The parity observer recomputes each latched water draw's six constant groups with the port (WaterPort.h) and compares them
// bit for bit. The census logs where each drawn water shape hangs in the scene graph. Both only read. Render thread.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

namespace DCLF::Features::Water::Port
{
	/** @brief Install time, main thread. Registers CS_DCLF_WATER_PARITY and reads it once. */
	void RegisterSwitches(Api::InstallContext& a_context);

	/** @brief Whether the observer and the census run. Read once at install. */
	bool ParityEnabled();

	/**
	 * @brief After a water draw was latched: checks its constants against the port and adds it to the census.
	 * a_pass is the pass whose SetupGeometry ran last, a_shape the TriShape DrawTriShape drew.
	 */
	void ObserveDraw(const DrawConstants& a_latched, const RE::BSRenderPass* a_pass, const Family::TriShape* a_shape, View a_view);

	namespace Census
	{
		/** @brief Adds a latched draw. Logs its shape's parent chain once, and every few hundred frames the roots that held water. */
		void Note(const RE::BSRenderPass& a_pass, std::uint32_t a_frame, bool a_unifiedWaterTile);
	}
}
