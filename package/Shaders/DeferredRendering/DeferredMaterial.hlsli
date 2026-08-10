#ifndef CS_DEFERRED_MATERIAL_HLSLI
#define CS_DEFERRED_MATERIAL_HLSLI

static const uint CS_INVALID_CONTEXT = 0xFFFFu;
static const uint CS_LEGACY_MATERIAL = 0u;
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
static const uint CS_MATERIAL_TruePBR = CS_MATERIAL_CSRegTruePbr;
static const uint CS_MATERIAL_TruePBRTerrain = CS_MATERIAL_CSRegTruePbrTerrain;
static const uint CS_MATERIAL_Grass = CS_MATERIAL_CSRegGrass;
static const uint CS_MATERIAL_DistantTree = CS_MATERIAL_CSRegDistantTree;
static const uint CS_MATERIAL_FoliageSpecial = CS_MATERIAL_CSRegFoliageSpecial;
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
static const uint CS_EVALUATOR_COUNT = 8u;

uint CSDeferredContextIndex(uint packedSurface)
{
	return packedSurface & 0xFFFFu;
}

uint CSDeferredMaterialClass(uint packedSurface)
{
	return (packedSurface >> 16u) & 0xFFu;
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
