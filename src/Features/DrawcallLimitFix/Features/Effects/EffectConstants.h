#pragma once

// Constants for the Effect shader port (dclf-architecture.md, "The Effect shader in the forward epoch").

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace DCLF::Features::Effects
{
	// Variable indices in the engine's own order. BSEffectShader's Setup functions write variable i through the shader's
	// constant table at +0x50 + i (VS) or +0x40 + i (PS). Community Shaders reflects Effect.hlsl in the same order.
	inline constexpr std::uint32_t kEffectVSVariables = 14;
	inline constexpr std::uint32_t kEffectPSVariables = 19;

	inline constexpr std::uint32_t kEffectVSWorld = 0;
	inline constexpr std::uint32_t kEffectVSPreviousWorld = 1;
	inline constexpr std::uint32_t kEffectVSBones = 2;
	inline constexpr std::uint32_t kEffectVSEyePosition = 3;
	inline constexpr std::uint32_t kEffectVSFogParam = 4;  // first PerTechnique VS variable
	inline constexpr std::uint32_t kEffectVSFogNearColor = 5;
	inline constexpr std::uint32_t kEffectVSFogFarColor = 6;
	inline constexpr std::uint32_t kEffectVSFalloffData = 7;
	inline constexpr std::uint32_t kEffectVSSoftMaterialParams = 8;
	inline constexpr std::uint32_t kEffectVSTexcoordOffset = 9;  // first PerMaterial VS variable
	inline constexpr std::uint32_t kEffectVSTexcoordOffsetMembrane = 10;
	inline constexpr std::uint32_t kEffectVSSubTexOffset = 11;
	inline constexpr std::uint32_t kEffectVSPosAdjust = 12;
	inline constexpr std::uint32_t kEffectVSMatProj = 13;

	inline constexpr std::uint32_t kEffectPSPropertyColor = 0;
	inline constexpr std::uint32_t kEffectPSAlphaTestRef = 1;
	inline constexpr std::uint32_t kEffectPSPLightPositionX = 4;  // first PerGeometry PS variable
	inline constexpr std::uint32_t kEffectPSDLightColor = 11;
	inline constexpr std::uint32_t kEffectPSVPOSOffset = 12;
	inline constexpr std::uint32_t kEffectPSCameraData = 13;  // first PerTechnique PS variable
	inline constexpr std::uint32_t kEffectPSFilteringParam = 14;
	inline constexpr std::uint32_t kEffectPSBaseColor = 15;  // first PerMaterial PS variable
	inline constexpr std::uint32_t kEffectPSBaseColorScale = 16;
	inline constexpr std::uint32_t kEffectPSLightingInfluence = 17;

	/** @brief Effect.hlsl's member names, by variable index. */
	inline constexpr std::array<std::string_view, kEffectVSVariables> kEffectVSNames{ "World", "PreviousWorld", "Bones", "EyePosition", "FogParam", "FogNearColor",
		"FogFarColor", "FalloffData", "SoftMateralVSParams", "TexcoordOffset", "TexcoordOffsetMembrane", "SubTexOffset", "PosAdjust", "MatProj" };
	inline constexpr std::array<std::string_view, kEffectPSVariables> kEffectPSNames{ "PropertyColor", "AlphaTestRef", "MembraneRimColor", "MembraneVars",
		"PLightPositionX", "PLightPositionY", "PLightPositionZ", "PLightingRadiusInverseSquared", "PLightColorR", "PLightColorG", "PLightColorB", "DLightColor",
		"VPOSOffset", "CameraDataEffect", "FilteringParam", "BaseColor", "BaseColorScale", "LightingInfluence", "ExtendedFlags" };

	/** @brief Variables wider than 4 floats, and Bones, which nothing reads and needs no real storage. */
	inline constexpr std::pair<std::uint32_t, std::uint32_t> kEffectVSSized[] = { { kEffectVSWorld, 12 }, { kEffectVSPreviousWorld, 12 }, { kEffectVSMatProj, 12 } };
	inline constexpr std::uint32_t kEffectVSScratch[] = { kEffectVSBones };

	/** @brief Variable masks for the PerTechnique, PerMaterial and PerGeometry groups (b0 to b2). */
	inline constexpr std::uint64_t kEffectVSGroups[3] = { 0x70ull, 0x380ull, 0x3C0Full };
	inline constexpr std::uint64_t kEffectPSGroups[3] = { 0x7000ull, 0x38000ull, 0x40FFFull };
	inline constexpr std::uint32_t kEffectVSFirstVariable[3] = { kEffectVSFogParam, kEffectVSTexcoordOffset, kEffectVSWorld };
	inline constexpr std::uint32_t kEffectPSFirstVariable[3] = { kEffectPSCameraData, kEffectPSBaseColor, kEffectPSPLightPositionX };

	/** @brief Must be the first port registered, after Core's Lighting at 0. RegisterPort checks this. */
	inline constexpr std::uint8_t kMaterialKindEffect = 1;

	// Pass descriptor bits, as built by FUN_1414bde90 and named in ShaderCache.h's EffectShaderFlags.
	namespace EffectFlag
	{
		inline constexpr std::uint32_t Vc = 1u << 0;
		inline constexpr std::uint32_t TexCoord = 1u << 1;
		inline constexpr std::uint32_t TexCoordIndex = 1u << 2;
		inline constexpr std::uint32_t Skinned = 1u << 3;
		inline constexpr std::uint32_t Normals = 1u << 4;
		inline constexpr std::uint32_t BinormalTangent = 1u << 5;
		inline constexpr std::uint32_t Texture = 1u << 6;
		inline constexpr std::uint32_t IndexedTexture = 1u << 7;
		inline constexpr std::uint32_t Falloff = 1u << 8;
		inline constexpr std::uint32_t AddBlend = 1u << 10;
		inline constexpr std::uint32_t MultBlend = 1u << 11;
		inline constexpr std::uint32_t Particles = 1u << 12;
		inline constexpr std::uint32_t StripParticles = 1u << 13;
		inline constexpr std::uint32_t Blood = 1u << 14;
		inline constexpr std::uint32_t Membrane = 1u << 15;
		inline constexpr std::uint32_t Lighting = 1u << 16;
		inline constexpr std::uint32_t ProjectedUv = 1u << 17;
		inline constexpr std::uint32_t Soft = 1u << 18;
		inline constexpr std::uint32_t GrayscaleToColor = 1u << 19;
		inline constexpr std::uint32_t GrayscaleToAlpha = 1u << 20;
		inline constexpr std::uint32_t IgnoreTexAlpha = 1u << 21;
		inline constexpr std::uint32_t MultBlendDecal = 1u << 22;
		inline constexpr std::uint32_t AlphaTest = 1u << 23;
		inline constexpr std::uint32_t SkyObject = 1u << 24;
		inline constexpr std::uint32_t MsnSpuSkinned = 1u << 25;
		inline constexpr std::uint32_t MotionVectorsNormals = 1u << 26;
		inline constexpr std::uint32_t Deferred = 1u << 27;
	}
	inline constexpr std::uint32_t kEffectPassEnumBase = 0x4000002Cu;
	inline constexpr std::uint32_t EffectPassEnum(std::uint32_t a_descriptor) { return a_descriptor + kEffectPassEnumBase; }

	// Frame constant rows in PS b13. Effect.hlsl reads them as DCLFEffectCamera, DCLFEffectDLightColor, DCLFEffectBlood and
	// DCLFEffectCloudLightColor.
	inline constexpr std::uint32_t kEffectCameraFrameRow = 17;
	inline constexpr std::uint32_t kEffectLightFrameRow = 18;
	inline constexpr std::uint32_t kEffectBloodFrameRow = 19;
	inline constexpr std::uint32_t kEffectCloudLightFrameRow = 20;
	/** @brief Depth copy for soft effects. The engine binds it at t3 for a Soft pass. */
	inline constexpr std::uint32_t kSoftDepthRegister = 118;

	// Shares bit 3 with refraction, which an effect record never has.
	inline constexpr std::uint32_t kRecordEffectSuppressEmittance = 1u << 3;
	// NiBillboardNode's mode goes in bits 5-7. Shares bits 4-7 with refraction.
	inline constexpr std::uint32_t kRecordEffectBillboard = 1u << 4;
	inline constexpr std::uint32_t kRecordEffectBillboardModeShift = 5;

	/** @brief Bits in PropertyRecord::feature for an effect material. */
	inline constexpr std::uint32_t kEffectHasSourcePath = 1u << 0;
	inline constexpr std::uint32_t kEffectHasGreyscalePath = 1u << 1;

	// Address Library ids.
	namespace Address
	{
		inline constexpr Engine::Offset kEffectShader{ { 415166 }, { 528216 } };  // BSEffectShader*
		inline constexpr Engine::Offset kEffectCamera{ { 414580 }, { 527667 } };  // NiCamera*, near and far at +0x160 and +0x164
		inline constexpr Engine::Offset kEffectBlood{ { 415167 }, { 528217 } };   // float[2]: BSEffectShader's blood globals
	}
}
