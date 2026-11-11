#ifndef __DCLF_POINT_LIGHTS_DEPENDENCY_HLSL__
#define __DCLF_POINT_LIGHTS_DEPENDENCY_HLSL__

// Drawcall Limit Fix without Light Limit Fix: a draw's point lights. The engine gives each pass its own lights in PerGeometry
// (SetupGeometry's PointLightPosition, PointLightColor, ShadowLightMaskSelect), but a DCLF pipeline's PerGeometry block is shared by
// every object it draws. So DCLF latches the frame's point lights into one table at PS b3 (Light Limit Fix's StrictLightData register,
// unused without it; Core/Render/Resources/FrameRecords.cpp, Api::PointLight), and each draw picks its own from the table, as the
// engine's light selection (FUN_1414fcf80) does for a pass: the shadow lights its object takes (the draw's shadow mask), then the plain
// lights whose sphere meets its bound, at most seven, in the table's order. Requires Common/DCLFObjects.hlsli.
#if defined(DCLF_BINDLESS_DRAW)

// The table's size (FrameRecords.cpp, kPointLightTableLights) and a light's flags.
#	define kDCLFMaxPointLights 128
#	define kDCLFPointLightShadowChannel 0xFFu   // bits 0-7: a shadow light's mask channel + 1; 0 for a plain light
#	define kDCLFPointLightAffectsLand 0x100u    // a plain light the landscape properties take (BSLight +0x61)

struct DCLFPointLight
{
	float4 PositionRadius;  // the light's world position (absolute, as the object rows), its radius in w
	float3 Color;           // its diffuse times its fade and lodDimmer, as SetupGeometry writes PointLightColor
	uint Flags;
};

cbuffer DCLFPointLights : register(b3)
{
	uint4 DCLFPointLightCounts;  // x: the lights in the table, the shadow lights first
	DCLFPointLight DCLFPointLightTable[kDCLFMaxPointLights];
};

// A draw's lights: how many, how many of them are shadow lights (the first ones), and their table indices, a byte each.
struct DCLFPointLightSelection
{
	uint Count;
	uint ShadowCount;
	uint2 Indices;
};

// The draw's lights from the table: a shadow light when its channel is in a_shadowMask, a plain light when its sphere meets a_bound
// (the object's world bound: centre, radius). The same for every pixel of the draw.
DCLFPointLightSelection DCLFSelectPointLights(uint a_shadowMask, float4 a_bound)
{
	DCLFPointLightSelection selection;
	selection.Count = 0;
	selection.ShadowCount = 0;
	selection.Indices = 0;
	const uint lights = min(DCLFPointLightCounts.x, kDCLFMaxPointLights);
	[loop] for (uint i = 0; i < lights && selection.Count < 7; i++)
	{
		const DCLFPointLight light = DCLFPointLightTable[i];
		const uint channel = light.Flags & kDCLFPointLightShadowChannel;
		bool taken;
		if (channel != 0) {
			taken = ((a_shadowMask >> (channel - 1)) & 1) != 0;
		} else {
#	if defined(LANDSCAPE)
			if ((light.Flags & kDCLFPointLightAffectsLand) == 0)
				continue;
#	endif
			const float3 offset = light.PositionRadius.xyz - a_bound.xyz;
			const float reach = light.PositionRadius.w + a_bound.w;
			taken = dot(offset, offset) < reach * reach;
		}
		if (taken) {
			const uint shift = (selection.Count & 3) * 8;
			if (selection.Count < 4)
				selection.Indices.x |= i << shift;
			else
				selection.Indices.y |= i << shift;
			selection.ShadowCount += channel != 0 ? 1 : 0;
			selection.Count++;
		}
	}
	return selection;
}

// The table index of a selection's light a_light.
uint DCLFPointLightIndex(DCLFPointLightSelection a_selection, uint a_light)
{
	const uint word = a_light < 4 ? a_selection.Indices.x : a_selection.Indices.y;
	return (word >> ((a_light & 3) * 8)) & 0xFFu;
}

#endif  // DCLF_BINDLESS_DRAW
#endif  // __DCLF_POINT_LIGHTS_DEPENDENCY_HLSL__
