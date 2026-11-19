#pragma once

// Billboards: effect-shader meshes under an NiBillboardNode, which the vertex stage turns to each view's camera
// (Effect.hlsl, DCLFBillboardWorld). Particle systems under billboard nodes go through DCLF's particle path.
// Orient is a CPU copy of the engine's turn, used only by the parity check.
// Core keeps the placement row and face mode (Core/Gpu/PlacementRows/BillboardRow.h).
// See dclf-architecture.md, "Billboards and aligned particles".

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::Billboards
{
	/** @brief A camera as the engine's billboard code reads it: its world rotation's columns (forward, up, right) and translation. */
	struct Camera
	{
		float forward[3]{}, up[3]{}, right[3]{}, eye[3]{};
	};

	/**
	 * @brief The world rotation NiBillboardNode::OnVisible (0x140d3e650) makes, row-major.
	 * Must match Effect.hlsl's DCLFBillboardRotation.
	 */
	void Orient(const float (&a_base)[3][3], const float (&a_pivot)[3], std::uint32_t a_mode, const Camera& a_camera, float (&a_out)[3][3]);

	Api::Feature Feature();
	Api::ToggleId Toggle();

	/**
	 * @brief CS_DCLF_SET_PARITY. After NiBillboardNode::OnVisible, compares the engine's turn of each effect leaf with
	 * DCLF's row and Orient. Main thread, at the early hooks.
	 */
	void InstallTurns();
	bool TurnsInstalled();
	/** @brief Written from any thread. The report takes them on the render thread. */
	struct TurnCounts
	{
		std::uint64_t turns = 0, checked = 0, differ = 0;
		float largestRotation = 0.0f, largestTranslation = 0.0f;
		std::string first;  // the first differing leaf
	};
	TurnCounts TakeTurns();

	void RegisterReport(Api::InstallContext& a_context);
}
