#ifndef __PHYSICAL_SKY_ORG_HANDLES_DEPENDENCY_HLSL__
#define __PHYSICAL_SKY_ORG_HANDLES_DEPENDENCY_HLSL__

// Physical Sky's pixel-stage resources for an ORG engine draw family (Drawcall Limit Fix's sky and water). Physical Sky's
// header declares them with ORG_HEADER_SRV and ORG_HEADER_SAMPLER. They are too many for the push block, so the draw's latch
// holds their heap indices. The order is DCLF::Integrations::PhysicalSky's kOrgTextureRegisters (PhysicalSky.h).
// In core, not in the feature, so a build without Physical Sky installed still finds it.
struct PhysSkyOrgHandles
{
	uint4 Textures[4];  // t61, t62, t63, t64, t66, t67, t69, t106, t109, t110, t111, t112, t113, t114, t115, unused
	uint4 Samplers;     // SampTr, SampSv, unused, unused
};

#if defined(CS_ORG_BINDLESS)
// Declares OrgHandle_<name> for each of Physical Sky's header resources from H, a PhysSkyOrgHandles.
#	define PHYS_SKY_ORG_HANDLES(H)                                   \
		static uint OrgHandle_TexTrLut = H.Textures[0].x;              \
		static uint OrgHandle_TexSvLut = H.Textures[0].y;              \
		static uint OrgHandle_TexApLut = H.Textures[0].z;              \
		static uint OrgHandle_TexApShadow = H.Textures[0].w;           \
		static uint OrgHandle_TexSkyTail = H.Textures[1].x;            \
		static uint OrgHandle_TexVolCubeDepth = H.Textures[1].y;       \
		static uint OrgHandle_MeanProfile = H.Textures[1].z;           \
		static uint OrgHandle_TexDiskPhaseLut = H.Textures[1].w;       \
		static uint OrgHandle_TexLightningReflection = H.Textures[2].x; \
		static uint OrgHandle_TexVolTr = H.Textures[2].y;              \
		static uint OrgHandle_TexVolLum = H.Textures[2].z;             \
		static uint OrgHandle_TexShadowVolume = H.Textures[2].w;       \
		static uint OrgHandle_TexLightningChannels = H.Textures[3].x;  \
		static uint OrgHandle_TexVolCubeTr = H.Textures[3].y;          \
		static uint OrgHandle_TexVolCubeLum = H.Textures[3].z;         \
		static uint OrgHandle_SampTr = H.Samplers.x;                   \
		static uint OrgHandle_SampSv = H.Samplers.y;
#endif

#endif  // __PHYSICAL_SKY_ORG_HANDLES_DEPENDENCY_HLSL__
