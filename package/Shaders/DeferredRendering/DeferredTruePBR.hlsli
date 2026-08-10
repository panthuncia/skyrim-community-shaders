#ifndef CS_DEFERRED_TRUE_PBR_ABI_HLSLI
#define CS_DEFERRED_TRUE_PBR_ABI_HLSLI

static const uint CS_PBR_VISUAL_ABI = 7u;
static const uint CS_PBR_INVALID_TEXTURE_DESCRIPTOR = 0xFFFFFFFFu;

struct PBRLandscapeLayerRecord
{
	uint baseColorTexture;
	uint normalTexture;
	uint rmaosTexture;
	uint displacementTexture;
	float4 materialParameters;
	float4 glintParameters;
};

struct PBRMaterialRecord
{
	uint abiVersion;
	uint flags;
	uint samplerPolicy;
	uint generation;
	uint4 objectTextures0;
	uint4 objectTextures1;
	float4 materialParameters[6];
	PBRLandscapeLayerRecord landscapeLayers[6];
};

#endif
