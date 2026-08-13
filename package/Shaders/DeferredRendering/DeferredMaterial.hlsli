#ifndef CS_DEFERRED_MATERIAL_HLSLI
#define CS_DEFERRED_MATERIAL_HLSLI

static const uint CS_INVALID_CONTEXT = 0xFFFFu;
static const uint CS_LEGACY_MATERIAL = 0u;
static const uint CS_INVALID_PBR_MATERIAL_RECORD = 0xFFFFu;
static const uint CS_PBR_PAYLOAD_CORE = 0u;
static const uint CS_PBR_PAYLOAD_SUBSURFACE_FUZZ = 1u;
static const uint CS_PBR_PAYLOAD_COAT = 2u;
static const uint CS_PBR_PAYLOAD_GLINT = 3u;
static const uint CS_PBR_PAYLOAD_PARALLAX = 4u;
static const uint CS_PBR_PAYLOAD_TERRAIN_ADVANCED = 5u;
static const uint CS_PBR_PAYLOAD_LOD_BLEND = 6u;
#define CS_DEFERRED_MATERIAL(name, value, cppEvaluator, hlslEvaluator) \
	static const uint CS_MATERIAL_##name = value;
#include "DeferredRendering/DeferredMaterialRegistry.def"
#undef CS_DEFERRED_MATERIAL
static const uint CS_MATERIAL_Legacy = CS_MATERIAL_CSRegLegacy;
static const uint CS_MATERIAL_StandardOpaque = CS_MATERIAL_CSRegStandardOpaque;
static const uint CS_MATERIAL_AlphaTestedOpaque = CS_MATERIAL_CSRegAlphaTestedOpaque;
static const uint CS_MATERIAL_StandardSpecular = CS_MATERIAL_CSRegStandardSpecular;
static const uint CS_MATERIAL_AlphaTestedSpecular = CS_MATERIAL_CSRegAlphaTestedSpecular;
static const uint CS_MATERIAL_Foliage = CS_MATERIAL_CSRegFoliage;
static const uint CS_MATERIAL_Terrain = CS_MATERIAL_CSRegTerrain;
static const uint CS_MATERIAL_TerrainSpecular = CS_MATERIAL_CSRegTerrainSpecular;
static const uint CS_MATERIAL_LodObject = CS_MATERIAL_CSRegLodObject;
static const uint CS_MATERIAL_FoliageSpecular = CS_MATERIAL_CSRegFoliageSpecular;
static const uint CS_MATERIAL_BlendedGeneric = CS_MATERIAL_CSRegBlendedGeneric;
static const uint CS_MATERIAL_BlendedGenericSpecular = CS_MATERIAL_CSRegBlendedGenericSpecular;
static const uint CS_MATERIAL_TruePBR = CS_MATERIAL_CSRegTruePbr;
static const uint CS_MATERIAL_TruePBRTerrain = CS_MATERIAL_CSRegTruePbrTerrain;
static const uint CS_MATERIAL_TruePBRSubsurfaceFuzz = CS_MATERIAL_CSRegTruePbrSubsurfaceFuzz;
static const uint CS_MATERIAL_TruePBRCoat = CS_MATERIAL_CSRegTruePbrCoat;
static const uint CS_MATERIAL_TruePBRGlint = CS_MATERIAL_CSRegTruePbrGlint;
static const uint CS_MATERIAL_TruePBRParallax = CS_MATERIAL_CSRegTruePbrParallax;
static const uint CS_MATERIAL_TruePBRTerrainAdvanced = CS_MATERIAL_CSRegTruePbrTerrainAdvanced;
static const uint CS_MATERIAL_TruePBRLodBlend = CS_MATERIAL_CSRegTruePbrLodBlend;
static const uint CS_MATERIAL_BlendedPBR = CS_MATERIAL_CSRegBlendedPbr;
static const uint CS_MATERIAL_Grass = CS_MATERIAL_CSRegGrass;
static const uint CS_MATERIAL_DistantTree = CS_MATERIAL_CSRegDistantTree;
static const uint CS_MATERIAL_FoliageSpecial = CS_MATERIAL_CSRegFoliageSpecial;
static const uint CS_MATERIAL_LodLand = CS_MATERIAL_CSRegLodLand;
static const uint CS_MATERIAL_Skin = CS_MATERIAL_CSRegSkin;
static const uint CS_MATERIAL_Hair = CS_MATERIAL_CSRegHair;
static const uint CS_MATERIAL_EyeEnvmap = CS_MATERIAL_CSRegEyeEnvmap;
#define CS_DEFERRED_EVALUATOR(cppName, hlslName, value, red, green, blue) \
	static const uint CS_EVALUATOR_##hlslName = value;
#include "DeferredRendering/DeferredMaterialRegistry.def"
#undef CS_DEFERRED_EVALUATOR
static const uint CS_EVALUATOR_COMPATIBILITY = CS_EVALUATOR_CSREG_COMPATIBILITY;
static const uint CS_EVALUATOR_GENERIC = CS_EVALUATOR_CSREG_GENERIC;
static const uint CS_EVALUATOR_TRUE_PBR = CS_EVALUATOR_CSREG_TRUEPBR;
static const uint CS_EVALUATOR_GRASS = CS_EVALUATOR_CSREG_GRASS;
static const uint CS_EVALUATOR_DISTANT_TREE = CS_EVALUATOR_CSREG_DISTANT_TREE;
static const uint CS_EVALUATOR_SKIN = CS_EVALUATOR_CSREG_SKIN;
static const uint CS_EVALUATOR_HAIR = CS_EVALUATOR_CSREG_HAIR;
static const uint CS_EVALUATOR_EYE_ENVMAP = CS_EVALUATOR_CSREG_EYE_ENVMAP;
static const uint CS_EVALUATOR_FOLIAGE_SPECIAL = CS_EVALUATOR_CSREG_FOLIAGE_SPECIAL;
static const uint CS_EVALUATOR_TRUE_PBR_SUBSURFACE_FUZZ = CS_EVALUATOR_CSREG_TRUEPBR_SUBSURFACE_FUZZ;
static const uint CS_EVALUATOR_TRUE_PBR_COAT = CS_EVALUATOR_CSREG_TRUEPBR_COAT;
static const uint CS_EVALUATOR_TRUE_PBR_GLINT = CS_EVALUATOR_CSREG_TRUEPBR_GLINT;
static const uint CS_EVALUATOR_TRUE_PBR_PARALLAX = CS_EVALUATOR_CSREG_TRUEPBR_PARALLAX;
static const uint CS_EVALUATOR_TRUE_PBR_TERRAIN = CS_EVALUATOR_CSREG_TRUEPBR_TERRAIN;
static const uint CS_EVALUATOR_TRUE_PBR_TERRAIN_ADVANCED = CS_EVALUATOR_CSREG_TRUEPBR_TERRAIN_ADVANCED;
static const uint CS_EVALUATOR_TRUE_PBR_LOD_BLEND = CS_EVALUATOR_CSREG_TRUEPBR_LOD_BLEND;
static const uint CS_EVALUATOR_COUNT = 16u;

uint CSDeferredContextIndex(uint packedSurface)
{
	return packedSurface & 0xFFFFu;
}

uint CSDeferredMaterialClass(uint packedSurface)
{
	return (packedSurface >> 16u) & 0xFFu;
}

uint CSDeferredPBRMaterialRecord(uint4 packedSurface)
{
	return packedSurface.y & 0xFFFFu;
}

uint CSDeferredPBRPayloadProfile(uint4 packedSurface)
{
	return (packedSurface.y >> 16u) & 0xFFu;
}

uint CSDeferredPBRPayloadAux(uint4 packedSurface)
{
	return packedSurface.y >> 24u;
}

uint CSDeferredEvaluatorForMaterial(uint materialClass)
{
	switch (materialClass) {
#define CS_DEFERRED_MATERIAL(name, value, cppEvaluator, hlslEvaluator) \
	case value: return CS_EVALUATOR_##hlslEvaluator;
#include "DeferredRendering/DeferredMaterialRegistry.def"
#undef CS_DEFERRED_MATERIAL
	default: return CS_EVALUATOR_COMPATIBILITY;
	}
}

bool CSDeferredEvaluatorEnabled(uint evaluator, uint enabledEvaluatorMask)
{
	return evaluator != CS_EVALUATOR_COMPATIBILITY &&
		(enabledEvaluatorMask & (1u << evaluator)) != 0u;
}

float3 CSDeferredEvaluatorDebugColor(uint evaluator)
{
	switch (evaluator) {
#define CS_DEFERRED_EVALUATOR(cppName, hlslName, value, red, green, blue) \
	case value: return float3(red, green, blue);
#include "DeferredRendering/DeferredMaterialRegistry.def"
#undef CS_DEFERRED_EVALUATOR
	default: return 0.0f;
	}
}

#endif
