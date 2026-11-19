#pragma once

// Decals (CS_DCLF_DECALS, CS_DCLF_LAYERS): the engine's decals (accumulation hints 2 and 3) and multi-index shapes' layers
// (hint 12). DCLF draws them in its decal groups after the opaque draws, each group in the engine's draw order.
// The order is rebuilt from the scene mirror, which records each decal node's array.
// See dclf-architecture.md, "Decals and pass layers".

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <string>

namespace DCLF::Features::Decals
{
	// Group g uses slot Fixed(g - 1), matching BuildDrawsCS. Features/Manifest.cpp checks them.
	inline constexpr Api::MainRangeSlot kOpaqueRange = Api::MainRangeSlot::Fixed0;
	inline constexpr Api::MainRangeSlot kBlendedRange = Api::MainRangeSlot::Fixed1;
	inline constexpr Api::MainRangeSlot kLayerRange = Api::MainRangeSlot::Fixed2;

	Api::Feature Feature();

	void InstallDecalHooks();
	void RegisterDecalCapture(Api::InstallContext& a_context);
	const void* DecalNodeRtti();

	Api::CoordinatorComponentId RegisterOrder(Api::InstallContext& a_context);
	void CandidateLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text);

	void SubscribeOrderParity();
	void OrderParityLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text);
}
