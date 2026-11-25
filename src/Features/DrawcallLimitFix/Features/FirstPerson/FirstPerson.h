#pragma once

// FirstPerson: DCLF draws the player's first-person skeleton
// (arms, weapons, shield, magic hands) as a view of the one scene. Its members are found under the first-person root,
// withheld from the engine's accumulators, and drawn by two epochs, one at the depth commit and one at the colour render.
// Core keeps what every bucketed view shares, keyed by the view's slot (Core/Render/Views/ViewSlots.h, Core/Extensions/Views.h).
// See dclf-architecture.md, "First person as a view bit".

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::FirstPerson
{
	// BuildDrawsCS.hlsl mirrors kViewFirstPerson. Features/Manifest.cpp checks both against Core's values.
	inline constexpr std::uint8_t kSetFirstPerson = 1u << 6;
	inline constexpr std::uint32_t kViewFirstPerson = 1u << 11;
	// Sits above Core's ForwardPipelineKey bits. Its value is part of the keys' recipe ids.
	inline constexpr std::uint32_t kForwardKeyFirstPerson = 4;

	Api::Feature Feature();

	void BeginFrame(const Api::ViewPointContext& a_context);
	bool Covered(const Api::ViewCoverage& a_coverage);
	void CaptureRoot(Api::ViewRootCapture& a_capture);
	void Accumulators(Api::ViewAccumulators& a_accumulators);
	void CullHiddenSkeleton(Api::CullHiddenNodes& a_nodes);

	void TakeDepthRange(bool a_controlDriven);
	void ExecuteDepth(const Api::ViewPointContext& a_context);
	void ExecuteColour();
	std::string Report();

	void InstallHooks(Api::InstallContext& a_context);
	// Must run before any hook is written. Both sites are CALLs, so the thunks chain.
	void PreflightHooks();

	/** @brief The first-person subtree's logs, subscribed to Core's root view events. */
	void SubscribeRootViewChecks();
}
