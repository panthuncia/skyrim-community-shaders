#pragma once

// The registration census's question to the coordinator and its answer. Asked on the render thread, answered in the next scene pass.

#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	namespace Scene
	{
		class Coordinator;
	}
}

namespace DCLF::Diagnostics
{
	/**
	 * @brief The coordinator's verdict on a geometry the census saw the engine draw, with its record, residency and phases.
	 * Nothing of the geometry is read. The request's pins keep the address valid.
	 */
	struct CensusVerdict
	{
		bool tracked = false;
		bool record = false;    // FindObject
		bool resident = false;  // bound
		Ineligible reason = Ineligible::None;
		std::uint8_t phases = 0;  // PhasesIn of its record in the coordinator's tables
	};
	/**
	 * @brief The census's question and its answer. One is in flight at a time, carrying every geometry seen since the last.
	 * The pins travel back with the reply so they are released on the render thread.
	 */
	struct CensusRequest
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
	};
	struct CensusReply
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
		std::vector<CensusVerdict> verdicts;  // parallel to geometries
		std::uint32_t sceneFrame = 0;         // the pass's
	};
	/** @brief a_geometry's verdict. Coordinator thread, or render thread while passes run inline. */
	CensusVerdict CensusVerdictOf(const Scene::Coordinator& a_store, const RE::BSGeometry* a_geometry);
	/** @brief Posts the request for the next scene pass. No wake is needed since the frame's pass takes it. Render thread. */
	void PostCensusRequest(std::shared_ptr<CensusRequest> a_request);
	/** @brief The reply posted since the last call, or null. Render thread. */
	std::shared_ptr<CensusReply> TakeCensusReply();
}
