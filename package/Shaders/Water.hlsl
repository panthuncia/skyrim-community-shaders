// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

#if defined(HORIZON_FIX)
namespace HorizonFix
{
	// Depth (z/w) that water folded back from beyond the far clip plane lands at: eight
	// depth quanta inside the far plane, exactly representable in both D24 and D32F
	// buffers - behind everything the scene rendered, in front of the depth clear.
	static const float FoldedDepth = 1.0 - 8.0 / 16777216.0;

	// Scene depth at or beyond this counts as "nothing rendered behind the water": the
	// clear value, folded far water (FoldedDepth), or an external plugin's depth-clamped
	// far-water backdrop a few quanta inside that. The margin is a few world units at the
	// far plane, where no real surface can render.
	static const float EmptyDepthThreshold = 1.0 - 64.0 / 16777216.0;
}
#endif

#if defined(UNDERWATERMASK)

#	if defined(CS_ORG_BINDLESS)
// The ORG build of ISUnderwaterMask (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE). It draws the engine's own geometry: the vertex
// stage pulls it by device address, which the port writes into the latch's PerFrame words (this form reads no PerFrame). The pixel
// stage writes 1 and reads nothing. One push block serves both stages.
#		define ORG_SHARED_CONSTANTS
#		include "Common/DeviceRef.hlsli"
#		include "Common/OrgEngineConstants.hlsli"
#		include "Common/OrgResources.hlsli"
#		define UNDERWATERMASK_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)
ORG_PUSH_BLOCK(UNDERWATERMASK_ORG_RESOURCES(ORG_HANDLE))
UNDERWATERMASK_ORG_RESOURCES(ORG_RESOURCE)
#	endif

#	if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION0;
};
#	endif

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
};

#	ifdef VSHADER

VS_OUTPUT UnderwaterMaskVertex(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float z = min(1, 1e-4 * max(0, input.Position.z - 70000)) * 0.5 + input.Position.z;
	vsout.Position = float4(input.Position.xy, z, 1);

	return vsout;
}

#		if defined(CS_ORG_BINDLESS)
#			if defined(__spirv__)
// The draw's geometry (ImageSpace.cpp, TakeDraw): FrameSrv and FrameOffset the vertex buffer's view ref, FrameSize and Source the
// 16-bit index buffer's, Padding the vertex stride (low 16 bits) and the first index (high 16 bits). The position is the vertex's
// first attribute, four floats.
VS_INPUT PullUnderwaterMaskVertex(uint a_vertexID)
{
	const DeviceRef vertices = DeviceRef(PerGeometryAt.FrameSrv, PerGeometryAt.FrameOffset);
	const DeviceRef indices = DeviceRef(PerGeometryAt.FrameSize, PerGeometryAt.Source);
	const uint stride = PerGeometryAt.Padding & 0xFFFF;
	const uint indexNumber = (PerGeometryAt.Padding >> 16) + a_vertexID;
	const uint indexWord = DeviceLoad(uint, DeviceRefAdd(indices, (indexNumber * 2) & ~3u));
	const uint index = (indexNumber & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	VS_INPUT input;
	input.Position = asfloat(DeviceLoad(uint4, DeviceRefAdd(vertices, index * stride)));
	return input;
}
#			else
// The D3D12 build has no vertex pulling yet: it compiles, and draws nothing.
VS_INPUT PullUnderwaterMaskVertex(uint a_vertexID)
{
	VS_INPUT input = (VS_INPUT)0;
	return input;
}
#			endif

VS_OUTPUT main(uint a_vertexID: SV_VertexID)
{
	return UnderwaterMaskVertex(PullUnderwaterMaskVertex(a_vertexID));
}
#		else
VS_OUTPUT main(VS_INPUT input)
{
	return UnderwaterMaskVertex(input);
}
#		endif
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#	ifdef PSHADER
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	psout.Color = 1;

	return psout;
}
#	endif

#else

#	if defined(CS_ORG_BINDLESS)
// The ORG build (Drawcall Limit Fix's water family, Features/DrawcallLimitFix/Features/Water). One push block serves both stages.
// A draw's engine constants (both stages' PerTechnique, PerMaterial and PerGeometry), its descriptors, its textures' and samplers'
// heap indices and its geometry are one latch region; Light Limit Fix's strict lights (PS b3) and the directional shadow record (t98)
// follow it. The shared constants and the headers' other resources come from the heap through the push block.
#		define ORG_SHARED_CONSTANTS
#		define ORG_HEADER_RESOURCES
// The permutation block is the draw's (WaterOrgPermutation), not a shared buffer: the pixel stage branches on its descriptor.
#		define DCLF_PULLED_ROWS
#		define SKYLIGHTING_PROBE_REGISTER t50
#		include "Common/OrgResources.hlsli"
#		include "Common/PhysicalSkyOrgHandles.hlsli"

// A draw's latched values, as the engine's SetupTechnique, SetupMaterial and SetupGeometry wrote its constant buffers (c rows in
// the D3D11 shaders' packoffsets). Matrices are rows: a latch is loaded with Load<T>, which does not keep row_major.
struct WaterOrgDraw
{
	float4 VSTechnique[1];  // VS b0: QPosAdjust
	float4 VSMaterial[6];   // VS b1: VSFogParam, VSFogNearColor, VSFogFarColor, NormalsScroll0, NormalsScroll1, NormalsScale
	float4 VSGeometry[14];  // VS b2: World, PreviousWorld, WorldViewProj (rows), ObjectUV, CellTexCoordOffset
	float4 PSTechnique[5];  // PS b0
	float4 PSMaterial[14];  // PS b1
	float4 PSGeometry[22];  // PS b2: TextureProj (its columns), ReflectPlane, ProjData, LightPos[8], LightColor[8]
	uint4 Descriptors;      // CS's permutation block: vertex, pixel, extra and extra feature descriptors
	float EffectRadius;
	uint VertexStride;
	uint FirstIndex;
	uint Padding;
	uint4 Textures[3];  // t0-t11 heap indices
	uint4 Samplers[3];  // s0-s11 sampler heap indices
	uint2 VertexAddress;
	uint2 IndexAddress;
	uint2 VertexLayout;
	uint2 Padding2;
	PhysSkyOrgHandles PhysSky;  // Physical Sky's resources and samplers, the same for every draw of a batch
};

#		define WATER_ORG_RESOURCES(X)                                                         \
			X(SHARED, ConstantBuffer, Shared, 0)                                                 \
			X(LATCH, WaterOrgDraw, WaterDraw, 2)                                                 \
			X(EXTERN_LATCH, StrictLightDataValues, StrictLightData, 3)                           \
			X(EXTERN, Texture2D<float4>, DepthTexture, 17)                                       \
			X(EXTERN, Texture2D<float2>, SharedShadowMap, 18)                                    \
			X(EXTERN, Texture3D<float4>, ExponentialHeightFogIntegratedLightScattering, 19)      \
			X(EXTERN, Texture3D<float4>, ExponentialHeightFogIntegratedLightScatteringFar, 22)   \
			X(EXTERN, TextureCube<float>, CloudShadowsTexture, 25)                               \
			X(EXTERN, TextureCube<float>, CloudSelfShadowTexture, 26)                            \
			X(EXTERN, TextureCube<float3>, EnvReflectionsTexture, 30)                            \
			X(EXTERN, TextureCube<float3>, EnvTexture, 31)                                       \
			X(EXTERN, StructuredBuffer<Light>, lights, 35)                                       \
			X(EXTERN, StructuredBuffer<uint>, lightList, 36)                                     \
			X(EXTERN, StructuredBuffer<LightGrid>, lightGrid, 37)                                \
			X(EXTERN, Texture3D<sh2>, SkylightingProbeArray, 50)                                 \
			X(EXTERN, Texture2D<float2>, ShadowHeightTexture, 60)                                \
			X(EXTERN, Texture2D<float4>, TexPrecipOcclusion, 70)                                 \
			X(EXTERN, Texture2D<sh2>, EnvIBLTexture, 76)                                         \
			X(EXTERN, Texture2D<sh2>, SkyIBLTexture, 77)                                         \
			X(EXTERN_LATCH, DirectionalShadowLightData, DirectionalShadowLight, 98)              \
			X(EXTERN, StructuredBuffer<LocalShadowData>, LocalShadows, 102)                      \
			X(EXTERN, Texture2DArray<float>, LocalShadowMaps, 103)
ORG_PUSH_BLOCK(WATER_ORG_RESOURCES(ORG_HANDLE))
WATER_ORG_RESOURCES(ORG_RESOURCE)
// Physical Sky's header reads its resources through these.
PHYS_SKY_ORG_HANDLES(WaterDraw.PhysSky)
#	endif

#	include "Common/FrameBuffer.hlsli"
#	include "Common/MotionBlur.hlsli"
#	include "Common/Permutation.hlsli"
#	include "Common/Random.hlsli"
#	include "Common/Shading.hlsli"
#	include "Common/ColorManagement.hlsli"
#	include "Common/BRDF.hlsli"
#	include "Common/LightDisk.hlsli"
#	include "Common/Game.hlsli"

#	define WATER

#	if defined(SPECULAR) || defined(UNDERWATER)
// WADING is undefined in pixel builds (read from the descriptor): `defined(WADING)` there is always false.
#		define WADING_RT
#		if defined(PSHADER)
#			undef WADING
#		endif
#	endif

#	include "Common/SharedData.hlsli"

#	if defined(CS_ORG_BINDLESS)
/** @brief The draw's permutation block (Permutation.hlsli, DCLF_PULLED_ROWS), set by each stage's main before anything reads it. */
void WaterOrgPermutation()
{
	Permutation::VertexShaderDescriptor = WaterDraw.Descriptors.x;
	Permutation::PixelShaderDescriptor = WaterDraw.Descriptors.y;
	Permutation::ExtraShaderDescriptor = WaterDraw.Descriptors.z;
	Permutation::ExtraFeatureDescriptor = WaterDraw.Descriptors.w;
	Permutation::EffectRadius = WaterDraw.EffectRadius;
}
#	elif defined(DCLF_BINDLESS)
// Drawcall Limit Fix's water draws (Features/DrawcallLimitFix/Features/Water/Draw; docs/development/plans/dclf-water-indirect.md).
// The colour segment culls water into its deferred ranges: the mask (the Stencil technique) in range 0, the surface (techniques
// 0, 9 and 11) in range 1. Each range draws plain indexed indirect draws (DCLF_PULLED), so a stage finds its draw's sequence by its
// instance, as Effect.hlsl's and Lighting.hlsl's pulled stages do.
// - World and PreviousWorld come from the placement row, made relative to the eye here.
// - The material values come from the water material pool's row, which the shading row names (WaterMaterial). They move every
//   frame (fog, scroll, colours times the water lighting), so a material row's block cannot hold them.
// - The water object row holds what SetupGeometry reads of the property. ObjectUV and CellTexCoordOffset are computed from it here,
//   with the frame row's flowmap values and Unified Water's rewrite, as Port::EvaluateObject computes them.
// - The camera comes from DCLF's own frame constants (FrameBuffer), which are this frame's. The water frame row is captured at the
//   frame's start, so its camera rows may be a frame old; only its other rows are read.
// - The headers, t98 and Physical Sky's resources are the frame record's, as Lighting's are. Light Limit Fix's room index is the
//   object record's (LightLimitFix.hlsli, RoomIndexOf).
// The three row buffers are frame bindings. The vertex stage's layout maps no register there, so both stages read them through the
// frame record's descriptor indices. Features/Water/Scene/WaterScene.h and Features/Water/Draw/WaterDraw.h must match the constants below.
#		include "Common/DCLFObjects.hlsli"

// The water material pool's row (WaterScene::WaterMaterialRow): VS b1 c0-c5, then PS b1 c0-c13. Row 0 is none.
struct DCLFWaterMaterial
{
	float4 VS[6];
	float4 PS[14];
};

// The water object row (WaterScene::WaterObjectRow): the property's cell and flow offsets, its plane, its flags and stencil bit.
struct DCLFWaterObject
{
	int4 Offsets;  // cellX, cellY, flowX, flowY, as the property holds them
	float4 Plane;  // world space, normal and constant
	uint WaterFlags;
	uint StencilBit;
	uint SurfaceDescriptor;
	uint Generation;
};

// The frame bindings' registers (WaterScene.h: kMaterialPoolRegister, kFrameRowRegister; WaterDraw.h: kObjectRowsRegister).
static const uint kDCLFWaterMaterialsRegister = 58;
static const uint kDCLFWaterFrameRegister = 59;
static const uint kDCLFWaterObjectsRegister = 42;
// The frame row's float4s (WaterScene::FrameRowEntry).
static const uint kDCLFWaterFrameQPosAdjust = 0;
static const uint kDCLFWaterFrameVposOffset = 1;
static const uint kDCLFWaterFramePosAdjust = 2;
static const uint kDCLFWaterFrameCameraData = 3;
static const uint kDCLFWaterFrameSunDir = 4;
static const uint kDCLFWaterFrameSunColor = 5;
static const uint kDCLFWaterFrameFlowmap = 9;
static const uint kDCLFWaterFrameUnifiedWaterFlow = 10;
static const uint kDCLFWaterFrameDisplacementOffset = 11;
// The water flags and descriptor bits ObjectUV and CellTexCoordOffset depend on (Port::EvaluateObject).
static const uint kDCLFWaterFlagVertexColors = 0x100;
static const uint kDCLFWaterFlagFlowmap = 0x8000;
static const uint kDCLFWaterDescriptorWading = 1u << 6;
static const uint kDCLFWaterDescriptorFlowmap = 1u << 9;

// The descriptor index the frame record holds for register a_register (DrawBindings::textures).
uint DCLFWaterFrameIndex(uint a_register) { return DCLFFrameRecordWord(kDCLFRecordTextures + 4 * a_register); }

DCLFWaterMaterial DCLFWaterMaterialOf(uint a_object)
{
	StructuredBuffer<DCLFWaterMaterial> materials = ResourceDescriptorHeap[DCLFWaterFrameIndex(kDCLFWaterMaterialsRegister)];
	return materials[DCLFShading[a_object].WaterMaterial];
}

// The water row is the object record's word 4 (WaterScene::kWaterRecordWord: DCLFTreeParams.x), its tag in word 5.
DCLFWaterObject DCLFWaterObjectOf(uint a_object)
{
	StructuredBuffer<DCLFWaterObject> objects = ResourceDescriptorHeap[DCLFWaterFrameIndex(kDCLFWaterObjectsRegister)];
	return objects[asuint(DCLFObjects[a_object].DCLFTreeParams.x)];
}

float4 DCLFWaterFrameRow(uint a_row)
{
	StructuredBuffer<float4> frame = ResourceDescriptorHeap[DCLFWaterFrameIndex(kDCLFWaterFrameRegister)];
	return frame[a_row];
}

// The placement's World as SetupGeometry writes it: the absolute row made relative to a_eye, with the fourth row of a 4x4.
// PreviousWorld is the same world against the previous eye, not the object's previous transform (the engine's).
float4x4 DCLFWaterWorld(uint a_object, float3 a_eye)
{
	const DCLFPlacement placement = DCLFPlacements[a_object];
	return float4x4(placement.World[0] - float4(0, 0, 0, a_eye.x), placement.World[1] - float4(0, 0, 0, a_eye.y), placement.World[2] - float4(0, 0, 0, a_eye.z),
		float4(0, 0, 0, 1));
}

// A frame row's position adjust with this frame's eye: the frame row's w (the framebuffer range) stays.
float4 DCLFWaterPosAdjust(uint a_row) { return float4(FrameBuffer::CameraPosAdjust.xyz, DCLFWaterFrameRow(a_row).w); }

/**
 * @brief ObjectUV (xyz) and CellTexCoordOffset of a_object for a_descriptor, as Port::EvaluateObject computes them: the property's
 * values after Unified Water's SetupGeometry rewrite, which takes the cell from the world translate (Util::WorldToCell).
 * Unified Water's local scale is the placement's world scale here: its tiles hang under the LOD water root without a scale of their own.
 */
void DCLFWaterObjectValues(uint a_object, uint a_descriptor, out float3 a_objectUV, out float4 a_cellTexCoordOffset)
{
	const DCLFWaterObject object = DCLFWaterObjectOf(a_object);
	const float4 flowmap = DCLFWaterFrameRow(kDCLFWaterFrameFlowmap);
	float flowmapSize = flowmap.x;
	float2 meshFlowCellOffset = flowmap.yz;
	int2 cell = object.Offsets.xy;
	int2 flow = object.Offsets.zw;
	if (flowmap.w != 0) {
		const float4 unified = DCLFWaterFrameRow(kDCLFWaterFrameUnifiedWaterFlow);  // width, height, offsets
		const DCLFPlacement placement = DCLFPlacements[a_object];
		const float scale = length(float3(placement.World[0].x, placement.World[1].x, placement.World[2].x));
		flowmapSize = unified.x;
		meshFlowCellOffset = float2(unified.y, 1.0 - scale);
		cell = int2(floor(float2(placement.World[0].w, placement.World[1].w) / 4096.0));
		flow = int2(cell.x + int(unified.z), cell.y + int(unified.w) + int(unified.x) - int(unified.y));
	}
	a_objectUV = 0;
	if (object.WaterFlags & kDCLFWaterFlagVertexColors)
		a_objectUV.x = 1;
	else if (object.WaterFlags & kDCLFWaterFlagFlowmap)
		a_objectUV = float3(flowmapSize, meshFlowCellOffset.x, 1.0 - meshFlowCellOffset.y);
	a_cellTexCoordOffset = 0;
	if (a_descriptor & kDCLFWaterDescriptorFlowmap) {
		if (a_descriptor & kDCLFWaterDescriptorWading)
			a_cellTexCoordOffset = DCLFWaterFrameRow(kDCLFWaterFrameDisplacementOffset);
		else
			a_cellTexCoordOffset = float4(flow.x, (flowmapSize - flow.y) - 1, cell.x, -cell.y);
	}
}

#		if defined(DCLF_PULLED)
// The permutation block (b4) as the draw's pipeline row holds it (Permutation.hlsli, DCLF_PULLED_ROWS; Effect.hlsl's).
void DCLFWaterPermutation()
{
	Permutation::VertexShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock);
	Permutation::PixelShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 4);
	Permutation::ExtraShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 8);
	Permutation::ExtraFeatureDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 12);
	Permutation::EffectRadius = DCLFRowFloat(DCLFPipelineRowAddress, kDCLFPermutationBlock + 16);
}

// A plain draw's object word and rows, from its sequence (BuildDrawsCS: DrawSequence), before anything reads them.
DeviceRef DCLFWaterSequence(uint a_instance)
{
	const DeviceRef sequence = DCLFDrawSequence(a_instance);
	const uint4 rows = DCLFSequenceRows(sequence);  // the pipeline row's ref, the material row's
	DCLFObjectWord = DCLFSequenceObjectWord(sequence);
	DCLFPipelineRowAddress = rows.xy;
	DCLFMaterialRowAddress = rows.zw;
	DCLFWaterPermutation();
	return sequence;
}
#		endif
#	endif

#	if defined(VSHADER)
struct VS_INPUT
{
#		if defined(SPECULAR) || defined(UNDERWATER) || defined(STENCIL) || defined(SIMPLE)
	float4 Position: POSITION0;
#			if defined(NORMAL_TEXCOORD)
	float2 TexCoord0: TEXCOORD0;
#			endif
#			if defined(VC)
	float4 Color: COLOR0;
#			endif
#		endif

#		if defined(LOD)
	float4 Position: POSITION0;
#			if defined(VC)
	float4 Color: COLOR0;
#			endif
#		endif
};
#	endif

struct VS_OUTPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER)
	float4 HPosition: SV_POSITION0;
#		if !defined(UNIFIED_WATER)
	float4 FogParam: COLOR0;
#		endif
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
#		if defined(WADING_RT) || defined(WADING) || (defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS))) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)) || ((defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0) && defined(FLOWMAP) /*!defined(NORMAL_TEXCOORD) && !defined(BLEND_NORMALS) && !defined(VC)*/)
	float4 TexCoord3: TEXCOORD3;
#		endif
#		if defined(FLOWMAP)
	nointerpolation float2 TexCoord4: TEXCOORD4;
#		endif
#		if NUM_SPECULAR_LIGHTS == 0
	float4 MPosition: TEXCOORD5;
#		endif
#	endif

#	if defined(SIMPLE)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
	float4 MPosition: TEXCOORD5;
#	endif

#	if defined(LOD)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
#	endif

#	if defined(STENCIL)
	float4 HPosition: SV_POSITION0;
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
#	endif

	float4 NormalsScale: TEXCOORD8;
#	if defined(DCLF_PULLED)
	// The draw's instance, by which the pixel stage reads its sequence (Effect.hlsl's).
	nointerpolation uint DCLFDrawInstance: TEXCOORD15;
#	endif
};

#	ifdef VSHADER

#		if defined(CS_ORG_BINDLESS)
static float4 QPosAdjust = WaterDraw.VSTechnique[0];
static float4 VSFogParam = WaterDraw.VSMaterial[0];
static float4 VSFogNearColor = WaterDraw.VSMaterial[1];
static float4 VSFogFarColor = WaterDraw.VSMaterial[2];
static float4 NormalsScroll0 = WaterDraw.VSMaterial[3];
static float4 NormalsScroll1 = WaterDraw.VSMaterial[4];
static float4 NormalsScale = WaterDraw.VSMaterial[5];
static float4x4 World = float4x4(WaterDraw.VSGeometry[0], WaterDraw.VSGeometry[1], WaterDraw.VSGeometry[2], WaterDraw.VSGeometry[3]);
static float4x4 PreviousWorld = float4x4(WaterDraw.VSGeometry[4], WaterDraw.VSGeometry[5], WaterDraw.VSGeometry[6], WaterDraw.VSGeometry[7]);
static float4x4 WorldViewProj = float4x4(WaterDraw.VSGeometry[8], WaterDraw.VSGeometry[9], WaterDraw.VSGeometry[10], WaterDraw.VSGeometry[11]);
static float3 ObjectUV = WaterDraw.VSGeometry[12].xyz;
static float4 CellTexCoordOffset = WaterDraw.VSGeometry[13];
#		elif defined(DCLF_BINDLESS)
// Assigned by DCLFWaterVertexStatics, which main calls once the draw's object word is known.
static float4 QPosAdjust;
static float4 VSFogParam;
static float4 VSFogNearColor;
static float4 VSFogFarColor;
static float4 NormalsScroll0;
static float4 NormalsScroll1;
static float4 NormalsScale;
static float4x4 World;
static float4x4 PreviousWorld;
static float4x4 WorldViewProj;
static float3 ObjectUV;
static float4 CellTexCoordOffset;

/** @brief The vertex stage's constants for the draw's object (DCLFObjectIndex), as SetupTechnique, SetupMaterial and SetupGeometry write them. */
void DCLFWaterVertexStatics()
{
	const DCLFWaterMaterial material = DCLFWaterMaterialOf(DCLFObjectIndex);
	QPosAdjust = DCLFWaterPosAdjust(kDCLFWaterFrameQPosAdjust);
	VSFogParam = material.VS[0];
	VSFogNearColor = material.VS[1];
	VSFogFarColor = material.VS[2];
	NormalsScroll0 = material.VS[3];
	NormalsScroll1 = material.VS[4];
	NormalsScale = material.VS[5];
	World = DCLFWaterWorld(DCLFObjectIndex, FrameBuffer::CameraPosAdjust.xyz);
	PreviousWorld = DCLFWaterWorld(DCLFObjectIndex, FrameBuffer::CameraPreviousPosAdjust.xyz);
	// The engine's WorldViewProj takes the jittered view-projection while TAA jitters (SE 513713), as the main pass's CameraViewProj.
	WorldViewProj = mul(FrameBuffer::CameraViewProj, World);
	DCLFWaterObjectValues(DCLFObjectIndex, Permutation::VertexShaderDescriptor, ObjectUV, CellTexCoordOffset);
}
#		else
cbuffer PerTechnique : register(b0)
{
	float4 QPosAdjust : packoffset(c0);
};

cbuffer PerMaterial : register(b1)
{
	float4 VSFogParam : packoffset(c0);
	float4 VSFogNearColor : packoffset(c1);
	float4 VSFogFarColor : packoffset(c2);
	float4 NormalsScroll0 : packoffset(c3);
	float4 NormalsScroll1 : packoffset(c4);
	float4 NormalsScale : packoffset(c5);
};

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 World : packoffset(c0);
	row_major float4x4 PreviousWorld : packoffset(c4);
	row_major float4x4 WorldViewProj : packoffset(c8);
	float3 ObjectUV : packoffset(c12);
	float4 CellTexCoordOffset : packoffset(c13);
};
#		endif

#		if defined(CS_ORG_BINDLESS) || defined(DCLF_BINDLESS)
VS_OUTPUT WaterVertex(VS_INPUT input)
#		else
VS_OUTPUT main(VS_INPUT input)
#		endif
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;

	vsout.NormalsScale = NormalsScale;

	float4 inputPosition = float4(input.Position.xyz, 1.0);
	float4 worldPos = mul(World, inputPosition);
	float4 worldViewPos = mul(WorldViewProj, inputPosition);

	float heightMult = min((1.0 / 10000.0) * max(worldViewPos.z - 70000, 0), 1);

	vsout.HPosition.xy = worldViewPos.xy;
	vsout.HPosition.z = heightMult * 0.5 + worldViewPos.z;
	vsout.HPosition.w = worldViewPos.w;

#		if defined(HORIZON_FIX)
	vsout.HPosition.z = min(vsout.HPosition.z, vsout.HPosition.w * HorizonFix::FoldedDepth);
#		endif

#		if defined(STENCIL)
	vsout.WorldPosition = worldPos;
	vsout.PreviousWorldPosition = mul(PreviousWorld, inputPosition);
#		else

#			if !defined(UNIFIED_WATER)
	float fogDistanceFactor = min(VSFogFarColor.w, pow(saturate(length(worldViewPos.xyz) * VSFogParam.y - VSFogParam.x), NormalsScale.w));
	vsout.FogParam.xyz = lerp(ColorManagement::SRGBToWorking(VSFogNearColor.xyz), ColorManagement::SRGBToWorking(VSFogFarColor.xyz), fogDistanceFactor);
	vsout.FogParam.w = fogDistanceFactor;
#			endif

	vsout.WPosition.xyz = worldPos.xyz;
	vsout.WPosition.w = length(worldPos.xyz);

#			if defined(LOD)
	float4 posAdjust =
		ObjectUV.x ? 0.0 : (QPosAdjust.xyxy + worldPos.xyxy) / NormalsScale.xxyy;

	vsout.TexCoord1.xyzw = NormalsScroll0 + posAdjust;
#			else
#				if !defined(SPECULAR) || (NUM_SPECULAR_LIGHTS == 0)
	vsout.MPosition.xyzw = inputPosition.xyzw;
#				endif

	float2 posAdjust = worldPos.xy + QPosAdjust.xy;

	float2 scrollAdjust1 = posAdjust / NormalsScale.xx;
	float2 scrollAdjust2 = posAdjust / NormalsScale.yy;
	float2 scrollAdjust3 = posAdjust / NormalsScale.zz;

#				if defined(UNIFIED_WATER) && defined(NORMAL_TEXCOORD)
	float2 cellShift = float2(floor(ObjectUV.z * 0.5), floor((ObjectUV.z - 1.0) * 0.5));
	float2 scaledUV = input.TexCoord0.xy * ObjectUV.z - cellShift;
#				endif

#				if !(defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS) || defined(DEPTH) || NUM_SPECULAR_LIGHTS == 0))
#					if defined(NORMAL_TEXCOORD)
	float3 normalsScale = 0.001 * NormalsScale.xyz;
	if (ObjectUV.x) {
		scrollAdjust1 = input.TexCoord0.xy / normalsScale.xx;
		scrollAdjust2 = input.TexCoord0.xy / normalsScale.yy;
		scrollAdjust3 = input.TexCoord0.xy / normalsScale.zz;
	}
#					else
	if (ObjectUV.x) {
		scrollAdjust1 = 0.0;
		scrollAdjust2 = 0.0;
		scrollAdjust3 = 0.0;
	}
#					endif
#				endif

	vsout.TexCoord1 = 0.0;
	vsout.TexCoord2 = 0.0;
#				if defined(WADING_RT)
	vsout.TexCoord3 = 0.0;
#				endif
#				if defined(FLOWMAP)
#					if !(((defined(SPECULAR) || NUM_SPECULAR_LIGHTS == 0) || (defined(UNDERWATER) && defined(REFRACTIONS))) && !defined(NORMAL_TEXCOORD))
#						if defined(BLEND_NORMALS)
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
#						else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = 0.0;
	vsout.TexCoord2.xy = 0.0;
#						endif
#					endif
#					if !defined(NORMAL_TEXCOORD)
	vsout.TexCoord3 = 0.0;
#					elif defined(WADING)
#						if defined(UNIFIED_WATER)
	float2 wadingUV = (input.TexCoord0.xy - 0.5f) * 0.5f;
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + wadingUV) / ObjectUV.xy;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + wadingUV;
#						else
	vsout.TexCoord2.zw = ((-0.5 + input.TexCoord0.xy) * 0.1 + CellTexCoordOffset.xy) +
	                     float2(CellTexCoordOffset.z, -CellTexCoordOffset.w + ObjectUV.x) / ObjectUV.xx;
	vsout.TexCoord3.xy = -0.25 + (input.TexCoord0.xy * 0.5 + ObjectUV.yz);
#						endif
	vsout.TexCoord3.zw = input.TexCoord0.xy;
#					elif (defined(REFRACTIONS) || NUM_SPECULAR_LIGHTS == 0 || defined(BLEND_NORMALS))
#						if defined(UNIFIED_WATER)
	float2 dims = float2(ObjectUV.x, ObjectUV.y);
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + scaledUV) / dims;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + scaledUV;
	vsout.TexCoord3.zw = scaledUV;
#						else
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + input.TexCoord0.xy) / ObjectUV.xx;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + input.TexCoord0.xy;
	vsout.TexCoord3.zw = input.TexCoord0.xy;
#						endif
#					endif
	vsout.TexCoord4 = ObjectUV.xy;
#				else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
	vsout.TexCoord2.z = worldViewPos.w;
	vsout.TexCoord2.w = 0;
#					if (defined(WADING) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)))
	vsout.TexCoord3 = 0.0;
#						if (defined(NORMAL_TEXCOORD) && ((!defined(BLEND_NORMALS) && !defined(VERTEX_ALPHA_DEPTH)) || defined(WADING)))
	vsout.TexCoord3.xy = input.TexCoord0;
#						endif
#						if defined(VERTEX_ALPHA_DEPTH) && defined(VC)
	vsout.TexCoord3.z = input.Color.w;
#						endif
#					endif
#				endif
#			endif
#		endif

	return vsout;
}

#		if defined(CS_ORG_BINDLESS) || defined(DCLF_BINDLESS)
#			if defined(CS_ORG_BINDLESS)
#				define DCLF_PULLED_VERTICES
#			endif
#			include "Common/DCLFVertexPull.hlsli"

/**
 * @brief Vertex a_index's attributes, as the engine's input layout for the draw's vertex layout reads them (DCLFDrawVertexLayout).
 * They must match the mask the DCLF pipelines check the geometry's layout against (ShaderPrograms.cpp, VertexAttributesOf: Water's).
 */
VS_INPUT WaterPulledInput(uint a_index, DCLFStream a_first, DCLFStream a_second)
{
	DCLFStreamsAt(a_index, a_first, a_second);
	VS_INPUT input = (VS_INPUT)0;
	input.Position = DCLFPullPosition(a_first, a_second);
#			if (defined(SPECULAR) || defined(UNDERWATER) || defined(STENCIL) || defined(SIMPLE)) && defined(NORMAL_TEXCOORD)
	input.TexCoord0 = DCLFPullHalf2(1, a_first, a_second);
#			endif
#			if defined(VC)
	input.Color = DCLFPullUnorm4(5, 0, a_first, a_second);
#			endif
	return input;
}
#		endif

#		if defined(CS_ORG_BINDLESS)
/** @brief The draw's vertex a_vertexID, pulled as the input assembler reads it: its 16-bit index, then its attributes. */
VS_INPUT WaterPullVertex(uint a_vertexID)
{
	DCLFDrawVertexLayout = WaterDraw.VertexLayout;
	const uint indexNumber = WaterDraw.FirstIndex + a_vertexID;
	const uint indexWord = DeviceLoad(uint, DeviceRefAdd(WaterDraw.IndexAddress, (indexNumber * 2) & ~3u));
	const uint index = (indexNumber & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	DCLFStream first = { WaterDraw.VertexAddress, WaterDraw.VertexStride };
	DCLFStream second = { DeviceRef(0, 0), 0u };
	return WaterPulledInput(index, first, second);
}

VS_OUTPUT main(uint a_vertexID: SV_VertexID)
{
	WaterOrgPermutation();
	return WaterVertex(WaterPullVertex(a_vertexID));
}
#		elif defined(DCLF_BINDLESS) && defined(DCLF_PULLED)
// The deferred ranges' plain draws: SV_VertexID is the geometry's vertex index (the index pool's indices, vertex offset 0), and the
// draw's sequence holds its geometry's two vertex buffer views (ref, size, stride) at bytes 24 and 40.
VS_OUTPUT DCLFWaterPulledVertex(uint a_index, uint a_instance)
{
	const DeviceRef sequence = DCLFWaterSequence(a_instance);
	DCLFWaterVertexStatics();
	DCLFDrawVertexLayout = DCLFPipelineVertexLayout();
	const uint4 vertexBuffer = DeviceLoad(uint4, DeviceRefAdd(sequence, 24));
	const uint4 streamBuffer = DeviceLoad(uint4, DeviceRefAdd(sequence, 40));
	DCLFStream first, second;
	first.address = vertexBuffer.xy;
	first.stride = vertexBuffer.w;
	second.address = streamBuffer.xy;
	second.stride = streamBuffer.w;
	VS_OUTPUT vsout = WaterVertex(WaterPulledInput(a_index, first, second));
	vsout.DCLFDrawInstance = a_instance;
	return vsout;
}

// The draw's lookups keyed on a wave-uniform instance (Effect.hlsl's pulled main): its rows and statics are the draw's, not each lane's.
VS_OUTPUT main(uint a_index: SV_VertexID, uint a_instance: SV_InstanceID DCLF_START_INSTANCE_INPUT)
{
	DCLF_ADD_START_INSTANCE(a_instance);
	VS_OUTPUT vsout;
	[loop] for (;;) {
		const uint drawInstance = WaveReadLaneFirst(a_instance);
		[branch] if (drawInstance == a_instance) {
			vsout = DCLFWaterPulledVertex(a_index, drawInstance);
			break;
		}
	}
	return vsout;
}
#		elif defined(DCLF_BINDLESS)
// A device-generated draw's form: the geometry's buffers come in the draw's push data. The deferred ranges never draw it, since their
// stencil reference cannot change per sequence, but a family program's stages are built in both forms.
VS_OUTPUT main(uint a_index: SV_VertexID)
{
	DCLFWaterVertexStatics();
	DCLFDrawVertexLayout = DCLFPushedVertexLayout();
	DCLFStream first, second;
	first.address = DCLFVertexBuffer.xy;
	first.stride = DCLFVertexStride;
	second.address = DCLFStreamBuffer.xy;
	second.stride = DCLFStreamStride;
	return WaterVertex(WaterPulledInput(a_index, first, second));
}
#		endif

#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#	if defined(UNDERWATER) || defined(SIMPLE) || defined(LOD) || defined(SPECULAR)
	float4 Lighting: SV_Target0;
#	endif

#	if defined(STENCIL)
	float4 WaterMask: SV_Target0;
	float2 MotionVector: SV_Target1;
#	endif
};

#	ifdef PSHADER

#		if defined(CS_ORG_BINDLESS)
static SamplerState ReflectionSampler = SamplerDescriptorHeap[WaterDraw.Samplers[0].x];
static SamplerState RefractionSampler = SamplerDescriptorHeap[WaterDraw.Samplers[0].y];
static SamplerState DisplacementSampler = SamplerDescriptorHeap[WaterDraw.Samplers[0].z];
static SamplerState CubeMapSampler = SamplerDescriptorHeap[WaterDraw.Samplers[0].w];
static SamplerState Normals01Sampler = SamplerDescriptorHeap[WaterDraw.Samplers[1].x];
static SamplerState Normals02Sampler = SamplerDescriptorHeap[WaterDraw.Samplers[1].y];
static SamplerState Normals03Sampler = SamplerDescriptorHeap[WaterDraw.Samplers[1].z];
static SamplerState DepthSampler = SamplerDescriptorHeap[WaterDraw.Samplers[1].w];
static SamplerState FlowMapSampler = SamplerDescriptorHeap[WaterDraw.Samplers[2].x];
static SamplerState FlowMapNormalsSampler = SamplerDescriptorHeap[WaterDraw.Samplers[2].y];
static SamplerState SSRReflectionSampler = SamplerDescriptorHeap[WaterDraw.Samplers[2].z];
static SamplerState RawSSRReflectionSampler = SamplerDescriptorHeap[WaterDraw.Samplers[2].w];
#			if defined(PHYSICAL_SKY)
// D3D11 binds Physical Sky's sky-view sampler at s13 for the aerial perspective.
static SamplerState ApSampler = SamplerDescriptorHeap[WaterDraw.PhysSky.Samplers.y];
#			endif

static Texture2D<float4> ReflectionTex = ResourceDescriptorHeap[WaterDraw.Textures[0].x];
static Texture2D<float4> RefractionTex = ResourceDescriptorHeap[WaterDraw.Textures[0].y];
static Texture2D<float4> DisplacementTex = ResourceDescriptorHeap[WaterDraw.Textures[0].z];
static TextureCube<float4> CubeMapTex = ResourceDescriptorHeap[WaterDraw.Textures[0].w];
static Texture2D<float4> Normals01Tex = ResourceDescriptorHeap[WaterDraw.Textures[1].x];
static Texture2D<float4> Normals02Tex = ResourceDescriptorHeap[WaterDraw.Textures[1].y];
static Texture2D<float4> Normals03Tex = ResourceDescriptorHeap[WaterDraw.Textures[1].z];
static Texture2D<float4> DepthTex = ResourceDescriptorHeap[WaterDraw.Textures[1].w];
static Texture2D<float4> FlowMapTex = ResourceDescriptorHeap[WaterDraw.Textures[2].x];
static Texture2D<float4> FlowMapNormalsTex = ResourceDescriptorHeap[WaterDraw.Textures[2].y];
static Texture2D<float4> SSRReflectionTex = ResourceDescriptorHeap[WaterDraw.Textures[2].z];
static Texture2D<float4> RawSSRReflectionTex = ResourceDescriptorHeap[WaterDraw.Textures[2].w];

static float4 VPOSOffset = WaterDraw.PSTechnique[0];
static float4 PosAdjust = WaterDraw.PSTechnique[1];
static float4 CameraDataWater = WaterDraw.PSTechnique[2];
static float4 SunDir = WaterDraw.PSTechnique[3];
static float4 SunColor = WaterDraw.PSTechnique[4];

static float4 ShallowColor = WaterDraw.PSMaterial[0];
static float4 DeepColor = WaterDraw.PSMaterial[1];
static float4 ReflectionColor = WaterDraw.PSMaterial[2];
static float4 FresnelRI = WaterDraw.PSMaterial[3];
static float4 BlendRadius = WaterDraw.PSMaterial[4];
static float4 VarAmounts = WaterDraw.PSMaterial[5];
static float4 NormalsAmplitude = WaterDraw.PSMaterial[6];
static float4 WaterParams = WaterDraw.PSMaterial[7];
static float4 FogNearColor = WaterDraw.PSMaterial[8];
static float4 FogFarColor = WaterDraw.PSMaterial[9];
static float4 FogParam = WaterDraw.PSMaterial[10];
static float4 DepthControl = WaterDraw.PSMaterial[11];
static float4 SSRParams = WaterDraw.PSMaterial[12];
static float4 SSRParams2 = WaterDraw.PSMaterial[13];

// TextureProj is column-major in the D3D11 cbuffer: its four rows there are the matrix's columns.
static float4x4 TextureProj = transpose(float4x4(WaterDraw.PSGeometry[0], WaterDraw.PSGeometry[1], WaterDraw.PSGeometry[2], WaterDraw.PSGeometry[3]));
static float4 ReflectPlane[1] = { WaterDraw.PSGeometry[4] };
static float4 ProjData = WaterDraw.PSGeometry[5];
static float4 LightPos[8] = { WaterDraw.PSGeometry[6], WaterDraw.PSGeometry[7], WaterDraw.PSGeometry[8], WaterDraw.PSGeometry[9],
	WaterDraw.PSGeometry[10], WaterDraw.PSGeometry[11], WaterDraw.PSGeometry[12], WaterDraw.PSGeometry[13] };
static float4 LightColor[8] = { WaterDraw.PSGeometry[14], WaterDraw.PSGeometry[15], WaterDraw.PSGeometry[16], WaterDraw.PSGeometry[17],
	WaterDraw.PSGeometry[18], WaterDraw.PSGeometry[19], WaterDraw.PSGeometry[20], WaterDraw.PSGeometry[21] };
#		else
SamplerState ReflectionSampler : register(s0);
SamplerState RefractionSampler : register(s1);
SamplerState DisplacementSampler : register(s2);
SamplerState CubeMapSampler : register(s3);
SamplerState Normals01Sampler : register(s4);
SamplerState Normals02Sampler : register(s5);
SamplerState Normals03Sampler : register(s6);
SamplerState DepthSampler : register(s7);
SamplerState FlowMapSampler : register(s8);
SamplerState FlowMapNormalsSampler : register(s9);
SamplerState SSRReflectionSampler : register(s10);
SamplerState RawSSRReflectionSampler : register(s11);
#		if defined(PHYSICAL_SKY)
SamplerState ApSampler : register(s13);
#		endif

#			if defined(DCLF_BINDLESS)
// The samplers and the material's textures (t0, t3-t6, t9) are the material row's. The frame's textures are frame textures, the same
// for every water draw of a frame (Features/Water/Draw/WaterDraw.h, kFrameTextureRegisters: kMAIN's copy, the displacement, the depth
// copy, the flow map, the SSR targets).
Texture2D<float4> ReflectionTex : register(t0);
Texture2D<float4> RefractionTex : register(t46);
Texture2D<float4> DisplacementTex : register(t47);
TextureCube<float4> CubeMapTex : register(t3);
Texture2D<float4> Normals01Tex : register(t4);
Texture2D<float4> Normals02Tex : register(t5);
Texture2D<float4> Normals03Tex : register(t6);
Texture2D<float4> DepthTex : register(t48);
Texture2D<float4> FlowMapTex : register(t49);
Texture2D<float4> FlowMapNormalsTex : register(t9);
Texture2D<float4> SSRReflectionTex : register(t56);
Texture2D<float4> RawSSRReflectionTex : register(t75);
#			else
Texture2D<float4> ReflectionTex : register(t0);
Texture2D<float4> RefractionTex : register(t1);
Texture2D<float4> DisplacementTex : register(t2);
TextureCube<float4> CubeMapTex : register(t3);
Texture2D<float4> Normals01Tex : register(t4);
Texture2D<float4> Normals02Tex : register(t5);
Texture2D<float4> Normals03Tex : register(t6);
Texture2D<float4> DepthTex : register(t7);
Texture2D<float4> FlowMapTex : register(t8);
Texture2D<float4> FlowMapNormalsTex : register(t9);
Texture2D<float4> SSRReflectionTex : register(t10);
Texture2D<float4> RawSSRReflectionTex : register(t11);
#			endif

#			if defined(DCLF_BINDLESS)
// Assigned by DCLFWaterPixelStatics, which main calls once the draw's object word is known.
static float4 VPOSOffset;
static float4 PosAdjust;
static float4 CameraDataWater;
static float4 SunDir;
static float4 SunColor;

static float4 ShallowColor;
static float4 DeepColor;
static float4 ReflectionColor;
static float4 FresnelRI;
static float4 BlendRadius;
static float4 VarAmounts;
static float4 NormalsAmplitude;
static float4 WaterParams;
static float4 FogNearColor;
static float4 FogFarColor;
static float4 FogParam;
static float4 DepthControl;
static float4 SSRParams;
static float4 SSRParams2;

static float4x4 TextureProj;
static float4 ReflectPlane[1];
static float4 ProjData;
// DCLF draws no light technique: under Light Limit Fix they add nothing. The surface's stages read none of these.
static float4 LightPos[8];
static float4 LightColor[8];

/** @brief The pixel stage's constants for the draw's object (DCLFObjectIndex), as SetupTechnique, SetupMaterial and SetupGeometry write them. */
void DCLFWaterPixelStatics()
{
	const uint object = DCLFObjectIndex;
	VPOSOffset = DCLFWaterFrameRow(kDCLFWaterFrameVposOffset);
	PosAdjust = DCLFWaterPosAdjust(kDCLFWaterFramePosAdjust);
	CameraDataWater = DCLFWaterFrameRow(kDCLFWaterFrameCameraData);
	SunDir = DCLFWaterFrameRow(kDCLFWaterFrameSunDir);
	SunColor = DCLFWaterFrameRow(kDCLFWaterFrameSunColor);

	const DCLFWaterMaterial material = DCLFWaterMaterialOf(object);
	ShallowColor = material.PS[0];
	DeepColor = material.PS[1];
	ReflectionColor = material.PS[2];
	FresnelRI = material.PS[3];
	BlendRadius = material.PS[4];
	VarAmounts = material.PS[5];
	NormalsAmplitude = material.PS[6];
	WaterParams = material.PS[7];
	FogNearColor = material.PS[8];
	FogFarColor = material.PS[9];
	FogParam = material.PS[10];
	DepthControl = material.PS[11];
	SSRParams = material.PS[12];
	SSRParams2 = material.PS[13];

	// SetupGeometry's TextureProj: the texture space times the world-view-projection, the world relative to the eye (FUN_1412f83b0).
	// The D3D11 block holds its columns, so the matrix here is the transpose of that product.
	const float4x4 textureSpace = float4x4(0.5, 0, 0, 0.5, 0, 0.5, 0, 0.5, 0, 0, 0.5, 0.5, 0, 0, 0, 1);
	const float3 eye = FrameBuffer::CameraPosAdjust.xyz;
	TextureProj = transpose(mul(textureSpace, mul(FrameBuffer::CameraViewProj, DCLFWaterWorld(object, eye))));
	// The property's plane as stored, with w its distance to the eye (this frame's, as the camera above).
	const float4 plane = DCLFWaterObjectOf(object).Plane;
	ReflectPlane[0] = float4(plane.xyz, dot(plane.xyz, eye) - plane.w);
	ProjData = float4(FrameBuffer::CameraProj[0][0], FrameBuffer::CameraProj[1][1], 0, 0);
}
#			else
cbuffer PerTechnique : register(b0)
{
	float4 VPOSOffset : packoffset(c0);  // inverse main render target width and height in xy, 0 in zw
	float4 PosAdjust : packoffset(c1);   // inverse framebuffer range in w
	float4 CameraDataWater : packoffset(c2);
	float4 SunDir : packoffset(c3);
	float4 SunColor : packoffset(c4);
}

cbuffer PerMaterial : register(b1)
{
	float4 ShallowColor : packoffset(c0);
	float4 DeepColor : packoffset(c1);
	float4 ReflectionColor : packoffset(c2);
	float4 FresnelRI : packoffset(c3);    // Fresnel amount in x, specular power in z
	float4 BlendRadius : packoffset(c4);  // flowmap scale in y, specular radius in z
	float4 VarAmounts : packoffset(c5);   // Sun specular power in x, reflection amount in y, alpha in z, refraction magnitude in w
	float4 NormalsAmplitude : packoffset(c6);
	float4 WaterParams : packoffset(c7);   // noise falloff in x, reflection magnitude in y, sun sparkle power in z, framebuffer range in w
	float4 FogNearColor : packoffset(c8);  // above water fog amount in w
	float4 FogFarColor : packoffset(c9);
	float4 FogParam : packoffset(c10);      // above water fog distance far in z, above water fog range in w
	float4 DepthControl : packoffset(c11);  // depth reflections factor in x, depth refractions factor in y, depth normals factor in z, depth specular lighting factor in w
	float4 SSRParams : packoffset(c12);     // fWaterSSRIntensity in x, fWaterSSRBlurAmount in y, inverse main render target width and height in zw
	float4 SSRParams2 : packoffset(c13);    // fWaterSSRNormalPerturbationScale in x
}

cbuffer PerGeometry : register(b2)
{
	float4x4 TextureProj : packoffset(c0);
	float4 ReflectPlane[1] : packoffset(c4);
	float4 ProjData : packoffset(c5);
	float4 LightPos[8] : packoffset(c6);
	float4 LightColor[8] : packoffset(c14);
}
#			endif
#		endif

#		define SampColorSampler Normals01Sampler
#		define LinearSampler Normals01Sampler

static const float WATER_F0 = 0.02f;
static const float WATER_SUN_ANGULAR_RADIUS = 0.00465f;

float GetWaterProjectedSolidAngle(float cosAngularRadius)
{
	return max(Math::PI * (1.0f - cosAngularRadius * cosAngularRadius), EPSILON_DIVISION);
}

float GetWaterLightDiskCos()
{
	float cosAngularRadius = cos(WATER_SUN_ANGULAR_RADIUS);
#		if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		cosAngularRadius = cos(SharedData::physSkyData.diskAngularRadius.w);
#		endif
	return cosAngularRadius;
}

/** @brief Physical Sky switches water to a fixed-F0 fresnel and a sun disk highlight; otherwise the water form's values apply. */
bool UsePhysicalWaterReflections()
{
#		if defined(PHYSICAL_SKY)
	return SharedData::physSkyData.enabled != 0;
#		else
	return false;
#		endif
}

float GetWaterFresnel(float cosTheta)
{
	return BRDF::F_Schlick(WATER_F0.xxx, saturate(cosTheta)).x;
}

float GetWaterDirectSpecularScale()
{
	return Color::PBRLightingCompensation * Color::PBRLightingScale;
}

float GetWaterDeltaLightDistributionFromCos(float3 normal, float3 viewDirection, float3 lightDirection, float cosAngularRadius)
{
	float3 V = -viewDirection;
	float3 R = reflect(viewDirection, normal);

	float NdotV = saturate(dot(normal, V));
	float lightMask = step(cosAngularRadius, dot(R, lightDirection));

	return lightMask * GetWaterFresnel(NdotV) / GetWaterProjectedSolidAngle(cosAngularRadius);
}

#		if defined(SKYLIGHTING)
#			include "Skylighting/Skylighting.hlsli"
#		endif

#		if defined(EXP_HEIGHT_FOG)
#			include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#		endif

#		if defined(PHYSICAL_SKY)
#			include "PhysicalSky/Common.hlsli"
#		endif

#		include "Common/ShadowSampling.hlsli"

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
float GetWaterFogFade()
{
#			if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		return ExponentialHeightFog::GetVanillaFogFade(PosAdjust.w);
	}
#			endif
	return PosAdjust.w;
}

#			if defined(FLOWMAP)

/**
 * Structure containing complete flowmap information
 */
struct FlowmapData
{
	float4 color;       // Raw flowmap color (R=flow_x, G=flow_y, B=flow_strength, A=flow_mask)
	float2 flowVector;  // Flow vector (coordinate space depends on source function)
};

/**
 * Gets raw flowmap data before UV-space coordinate transformation
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for sampling the flowmap texture
 * @return FlowmapData with raw components:
 *         - color: Raw flowmap texture sample (RG=rotation, B=strength, A=mask)
 *         - flowVector: Base flow vector before any coordinate transformation
 *                      Ready for direct application of rotation matrix for world positioning
 *
 * @details This function provides flowmap data in its original coordinate space, suitable
 *          for world-space positioning effects (like ripple movement). The flowVector has
 *          NOT been transformed for UV-space normal sampling - that transformation is only
 *          applied in GetFlowmapDataUV() which uses transpose for UV coordinate perturbation.
 *
 *          Use this function when you need to apply the rotation matrix directly for
 *          world-space effects without needing to reverse any existing transformations.
 *
 * @see GetFlowmapDataUV() for UV-space normal sampling (applies transpose transformation)
 */
FlowmapData GetFlowmapDataTextureSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data;
	data.color = FlowMapTex.SampleLevel(FlowMapSampler, input.TexCoord2.zw + uvShift, 0);
	data.flowVector = (64 * input.TexCoord3.xy) * sqrt(1.01 - data.color.z);
	// NOTE: flowVector is NOT transformed yet - this is the raw vector before rotation matrix
	return data;
}
/**
 * Samples flowmap texture and calculates UV-space flow data for texture sampling
 *
 * @param input Pixel shader input containing texture coordinates and world position data
 * @param uvShift UV offset for sampling the flowmap texture (used for animation/variation)
 * @return FlowmapData Complete flowmap information with UV-space flow vector
 *
 * @details This function:
 *          - Samples the flowmap texture at the specified UV coordinates
 *          - Decodes flow direction from RG channels (remapped from [0,1] to [-1,1])
 *          - Calculates flow strength using the blue channel with sqrt falloff
 *          - Applies transpose rotation matrix to transform flow direction to UV space
 *          - Scales flow vector by world position and strength factors
 *
 * @note Flowmap format:
 *       - Red channel: Flow direction X component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Green channel: Flow direction Y component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Blue channel: Flow strength (0 = no flow, 1 = maximum flow)
 *       - Alpha channel: Flow mask/intensity multiplier
 */
FlowmapData GetFlowmapDataUV(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowSinCos = data.color.xy * 2 - 1;
	float2x2 flowRotationMatrix = float2x2(flowSinCos.x, flowSinCos.y, -flowSinCos.y, flowSinCos.x);
	data.flowVector = mul(transpose(flowRotationMatrix), data.flowVector);
	return data;
}
// ----------------------------------------------------------------
// Flowmap Parallax Functions
// ----------------------------------------------------------------

/**
 * Samples height from flowmap texture using the same 4-sample blend as flowmap normals
 * This ensures height transitions match the normal transitions exactly
 *
 * @param input PS_INPUT for flowmap coordinate access
 * @param normalMul The blend weights from the flowmap system (same as used for normals)
 * @param uvShift The UV shift value (1 / (128 * flowmapDimensions))
 * @param mipLevel Mip level for texture sampling
 */
float GetFlowmapHeightBlended(PS_INPUT input, float2 normalMul, float2 uvShift, float mipLevel)
{
	// Sample height using the EXACT same UV computation as GetFlowmapNormal
	// This ensures the height blending matches the normal blending perfectly

	// Sample 0: uvShift, multiplier=9.92, offset=0
	FlowmapData flowData0 = GetFlowmapDataUV(input, uvShift);
	float2 uv0 = 0 + (flowData0.flowVector - float2(9.92 * ((0.001 * ReflectionColor.w) * flowData0.color.w), 0));
	float height0 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv0, mipLevel).w;

	// Sample 1: float2(0, uvShift.y), multiplier=10.64, offset=0.27
	FlowmapData flowData1 = GetFlowmapDataUV(input, float2(0, uvShift.y));
	float2 uv1 = 0.27 + (flowData1.flowVector - float2(10.64 * ((0.001 * ReflectionColor.w) * flowData1.color.w), 0));
	float height1 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv1, mipLevel).w;

	// Sample 2: 0.0.xx, multiplier=8, offset=0
	FlowmapData flowData2 = GetFlowmapDataUV(input, 0.0.xx);
	float2 uv2 = 0 + (flowData2.flowVector - float2(8 * ((0.001 * ReflectionColor.w) * flowData2.color.w), 0));
	float height2 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv2, mipLevel).w;

	// Sample 3: float2(uvShift.x, 0), multiplier=8.48, offset=0.62
	FlowmapData flowData3 = GetFlowmapDataUV(input, float2(uvShift.x, 0));
	float2 uv3 = 0.62 + (flowData3.flowVector - float2(8.48 * ((0.001 * ReflectionColor.w) * flowData3.color.w), 0));
	float height3 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv3, mipLevel).w;

	// Use the EXACT same blending formula as flowmap normals
	float blendedHeight =
		normalMul.y * (normalMul.x * height2 + (1 - normalMul.x) * height3) +
		(1 - normalMul.y) * (normalMul.x * height1 + (1 - normalMul.x) * height0);

	return blendedHeight;
}

// Keep this for compatibility - just forwards to the proper function
float GetFlowmapHeightBarycentric(PS_INPUT input, float2 flowmapDimensions, float2 baseUV, float mipLevel)
{
	// This is now unused - we use GetFlowmapHeightBlended directly
	return FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, baseUV, mipLevel).w;
}

/**
 * Computes mip level from continuous (non-flowmapped) UVs.
 * Flow-rotated UVs have discontinuous ddx/ddy and must not be used for LOD.
 */
float GetFlowmapMipLevel(float2 baseUV)
{
	float2 textureDims;
	FlowMapNormalsTex.GetDimensions(textureDims.x, textureDims.y);

	float2 texCoordsPerSize = baseUV * textureDims;
	float2 dxSize = ddx(texCoordsPerSize);
	float2 dySize = ddy(texCoordsPerSize);
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;
	float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);
	return max(0.5 * log2(minTexCoordDelta), 0);
}

/**
 * Samples height from flowmap texture (riverflow.dds alpha channel)
 * Uses the same UV calculation as GetFlowmapNormal for consistency
 */

/**
 * Generates flowmap-based normal (no parallax - flowmap normals are not parallax-shifted).
 * Mip LOD uses non-flowmapped TexCoord3 derivatives — flow-rotated UVs break hardware/manual
 * mip selection (stuck mip 0 shimmer, or excessive LOD + UV shrink that flattens normals).
 */
float3 GetFlowmapNormal(PS_INPUT input, float2 uvShift, float multiplier, float offset, float2 dx, float2 dy)
{
	FlowmapData flowData = GetFlowmapDataUV(input, uvShift);
	float2 uv = offset + (flowData.flowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	// Match UV footprint: flowVector scales by this (see GetFlowmapDataTextureSpace)
	float flowStrength = sqrt(1.01 - flowData.color.z);
	float mipScale = exp2(SharedData::MipBias) * flowStrength;

	return float3(FlowMapNormalsTex.SampleGrad(FlowMapNormalsSampler, uv, dx * mipScale, dy * mipScale).xy, flowData.color.z);
}

/**
 * Gets flowmap data with world-space flow vector for positioning effects
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for flowmap sampling (used for animation phases)
 * @return FlowmapData Complete flowmap information with world-space flow vector
 *
 * @details This function:
 *          - Samples raw flowmap data (before UV-space transformations)
 *          - Decodes flow direction from flowmap RG channels
 *          - Applies component-wise directional transformation
 *          - Returns complete flowmap data with world-space flow vector
 *
 * @note Use this for effects that need to move with water current (ripples, debris, foam, etc.)
 *       For UV-space normal sampling, use GetFlowmapDataUV() instead
 */
FlowmapData GetFlowmapDataWorldSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}

/**
 * Converts existing texture-space flowmap data to world-space (avoids duplicate sampling)
 *
 * @param textureSpaceData FlowmapData from GetFlowmapDataTextureSpace()
 * @return FlowmapData Complete flowmap data with world-space flow vector
 *
 * @note Use this overload when you already have texture-space flowmap data to avoid duplicate texture sampling
 */
FlowmapData GetFlowmapDataWorldSpace(FlowmapData textureSpaceData)
{
	FlowmapData data = textureSpaceData;
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}
#			endif

#			if defined(LOD)
#				undef WATER_EFFECTS
#				undef WETNESS_EFFECTS
#			endif

#			if defined(WATER_EFFECTS) && !defined(VC)
#				define WATER_PARALLAX
#				include "WaterEffects/WaterParallax.hlsli"
#			endif

#			if defined(DYNAMIC_CUBEMAPS)
#				include "DynamicCubemaps/DynamicCubemaps.hlsli"
#			endif

#			if defined(WETNESS_EFFECTS)
#				include "WetnessEffects/WetnessEffects.hlsli"
#			endif

// Structure to return both normal and ripple/splash color information
struct WaterNormalData
{
	float3 normal;
	float4 rippleInfo;  // xyz = scaled ripple normal (normalized normal * intensity), w = splash effect intensity
};

WaterNormalData GetWaterNormal(PS_INPUT input, float distanceFactor, float normalsDepthFactor, float3 viewDirection, float depth, float wetnessOcclusion)
{
	WaterNormalData result;
	result.rippleInfo = float4(0, 0, 0, 0);

	float3 normalScalesRcp = rcp(input.NormalsScale.xyz);

	float4 normalsAmplitude = NormalsAmplitude;
	if (SharedData::enbSettings.EnableWater) {
		normalsAmplitude *= SharedData::enbSettings.WaterWavesAmplitude;
	}

#			if defined(WATER_PARALLAX)
	float2 parallaxOffset = WaterEffects::GetParallaxOffset(input, normalsAmplitude.xyz, normalScalesRcp);
#			endif

#			if defined(FLOWMAP)
#				if defined(UNIFIED_WATER)
	float2 flowmapDimensions = input.TexCoord4.xy;
#				else
	float2 flowmapDimensions = input.TexCoord4.xx;
#				endif
	float2 uvShift = 1 / (128 * flowmapDimensions);

	// Compute flowmap parallax and create parallaxed input for normal sampling
	PS_INPUT flowmapInput = input;
	float2 flowmapParallaxOffset = float2(0, 0);
#				if defined(WATER_PARALLAX) && !defined(LOD)
	float parallaxAmount = WaterEffects::GetFlowmapParallaxAmount(input, flowmapDimensions, viewDirection);
	float2 parallaxDir = viewDirection.xy / -viewDirection.z;
	parallaxDir.y = -parallaxDir.y;
	float viewDotUp = -viewDirection.z;
	parallaxDir *= 0.008 * saturate(viewDotUp * 2.0);
	flowmapInput.TexCoord3.xy = input.TexCoord3.xy + parallaxAmount * parallaxDir;
	flowmapParallaxOffset = WaterEffects::GetFlowmapParallaxOffset(input, flowmapDimensions, viewDirection, normalsAmplitude.x, normalScalesRcp);
#				endif

	// Calculate cell blend weights using parallaxed input
	float2 normalMul = 0.5 + -(-0.5 + abs(frac(flowmapInput.TexCoord2.zw * (64 * flowmapDimensions)) * 2 - 1));

	// Same UV scale as flowVector before rotation — shared so all 4 blend samples use matching LOD
	float2 baseUV = 64 * flowmapInput.TexCoord3.xy;
	float2 flowNormalDx = ddx(baseUV);
	float2 flowNormalDy = ddy(baseUV);

	// Sample flowmap normals with parallax applied
	float3 flowmapNormal0 = GetFlowmapNormal(flowmapInput, uvShift, 9.92, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal1 = GetFlowmapNormal(flowmapInput, float2(0, uvShift.y), 10.64, 0.27, flowNormalDx, flowNormalDy);
	float3 flowmapNormal2 = GetFlowmapNormal(flowmapInput, 0.0.xx, 8, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal3 = GetFlowmapNormal(flowmapInput, float2(uvShift.x, 0), 8.48, 0.62, flowNormalDx, flowNormalDy);

	float2 flowmapNormalWeighted =
		normalMul.y * (normalMul.x * flowmapNormal2.xy + (1 - normalMul.x) * flowmapNormal3.xy) +
		(1 - normalMul.y) *
			(normalMul.x * flowmapNormal1.xy + (1 - normalMul.x) * flowmapNormal0.xy);
	float2 flowmapDenominator = sqrt(normalMul * normalMul + (1 - normalMul) * (1 - normalMul));
	float3 flowmapNormal =
		float3(((-0.5 + flowmapNormalWeighted) / (flowmapDenominator.x * flowmapDenominator.y)) *
				   max(0.4, normalsDepthFactor),
			0);
	flowmapNormal.z =
		sqrt(1 - flowmapNormal.x * flowmapNormal.x - flowmapNormal.y * flowmapNormal.y);
	float2 baseNormalUv = input.TexCoord1.xy;
#				if defined(WATER_PARALLAX)
	// Use flowmap-derived parallax offset for base normals
	baseNormalUv += flowmapParallaxOffset.xy * normalScalesRcp.x;
#				endif
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, baseNormalUv, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#			endif  // End of FLOWMAP block

#			if !defined(FLOWMAP)
#				if defined(WATER_PARALLAX)
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy + parallaxOffset.xy * normalScalesRcp.x, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#				else
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#				endif
#			endif  // End of !FLOWMAP block
#			if defined(FLOWMAP) && !defined(BLEND_NORMALS)
#				ifdef DISABLE_FLOWMAP_NORMALS
	// FLOWMAP NORMALS DISABLED: Using only base normals (flow system still active for ripples/splashes)
	float3 finalNormal = normalize(normals1 + float3(0, 0, 1));
#				else
	// FLOWMAP NORMALS ENABLED: Blending flow-based normals with base normals
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor));
#				endif
#			elif !defined(LOD)

#				if defined(WATER_PARALLAX)
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw + parallaxOffset.xy * normalScalesRcp.y, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy + parallaxOffset.xy * normalScalesRcp.z, SharedData::MipBias).xyz * 2.0 - 1.0;
#				else
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy, SharedData::MipBias).xyz * 2.0 - 1.0;
#				endif

	float3 blendedNormal = normalize(float3(0, 0, 1) + normalsAmplitude.x * normals1 +
									 normalsAmplitude.y * normals2 + normalsAmplitude.z * normals3);
#				if defined(UNDERWATER)
	float3 finalNormal = blendedNormal;
#				else
	float3 finalNormal = normalize(lerp(float3(0, 0, 1), blendedNormal, normalsDepthFactor));
#				endif

#				if defined(FLOWMAP)
	float normalBlendFactor =
		normalMul.y * ((1 - normalMul.x) * flowmapNormal3.z + normalMul.x * flowmapNormal2.z) +
		(1 - normalMul.y) * (normalMul.x * flowmapNormal1.z + (1 - normalMul.x) * flowmapNormal0.z);
	finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), normalize(lerp(finalNormal, flowmapNormal, normalBlendFactor)), distanceFactor));
#				endif
#			else
	float3 finalNormal =
		normalize(float3(0, 0, 1) + normalsAmplitude.xxx * normals1);
#			endif

#			if defined(WADING_RT) || defined(WADING)
#				if defined(WADING_RT)
	const bool isWading = Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Wading;
#				else
	const bool isWading = true;
#				endif
	if (isWading) {
#				if defined(FLOWMAP)
		float2 displacementUv = input.TexCoord3.zw;
#				else
		float2 displacementUv = input.TexCoord3.xy;
#				endif
		float3 displacement = normalize(float3(normalsAmplitude.w * (-0.5 + DisplacementTex.Sample(DisplacementSampler, displacementUv).zw),
			0.04));
		finalNormal = lerp(displacement, finalNormal, displacement.z);
	}
#			endif

#			if defined(WETNESS_EFFECTS)
	// Wetness Effects Debug System:
	// DEBUG_WETNESS_EFFECTS Color Legend:
	// - BRIGHT MAGENTA: Ripples, BRIGHT GREEN: Splashes, CYAN: Both effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	float maxRainDropDistance = SharedData::wetnessEffectsSettings.RaindropFxRange * SharedData::wetnessEffectsSettings.RaindropFxRange * 3;
	float rainDropDistance = dot(input.WPosition.xyz, input.WPosition.xyz);
	float distanceFadeout = saturate((1 - saturate(rainDropDistance / maxRainDropDistance)) * 3);
	if (finalNormal.z > 0 && SharedData::wetnessEffectsSettings.Raining > 0.0f && SharedData::wetnessEffectsSettings.EnableRaindropFx &&
		(rainDropDistance < maxRainDropDistance) && wetnessOcclusion > 0.05) {
		float rippleStrengthModifier = (wetnessOcclusion * wetnessOcclusion) * distanceFadeout;
		float3 rippleWPosition = input.WPosition.xyz + finalNormal * 16;
#				if defined(WATER_PARALLAX)
		rippleWPosition.xy += parallaxOffset;
#				endif
#				if defined(FLOWMAP)
		// Flow-following ripple enhancement: Makes raindrops follow water current
		FlowmapData worldFlowData = GetFlowmapDataWorldSpace(input, float2(0, 0));

		// Calculate flow-aware ripple offset using centralized timing logic
		// Parameters: avgFlowmapMultiplier=9.26 (average of GetWaterNormal flowmap normal multipliers: 9.92, 10.64, 8, 8.48)
		// uvToWorldScale=0.125 (1/8 - relates to 64× texture coordinate scaling factor)
		float2 flowOffset = WetnessEffects::GetFlowAwareRippleOffset(
			worldFlowData.flowVector,
			worldFlowData.color.w,      // Flow strength from flowmap alpha
			0.001 * ReflectionColor.w,  // Reflection timing scale (matches GetFlowmapNormal)
			9.26,                       // Average flowmap normal multiplier
			0.125                       // UV-to-world scale factor (1/8)
		);

		rippleWPosition.xy += flowOffset;
#				endif
		raindropInfo = WetnessEffects::GetRainDrops(rippleWPosition + FrameBuffer::CameraPosAdjust.xyz, SharedData::wetnessEffectsSettings.Time, finalNormal, rippleStrengthModifier);

		// Calculate ripple and splash color intensities
		float rippleIntensity = length(raindropInfo.xy) * rippleStrengthModifier;
		float splashIntensity = raindropInfo.w * distanceFadeout;

		// Store ripple and splash information for color effects
		result.rippleInfo.xyz = raindropInfo.xyz * rippleIntensity;
		result.rippleInfo.w = splashIntensity;
	}
	float3 rippleNormal = normalize(raindropInfo.xyz);
	finalNormal = ReorientNormal(rippleNormal, finalNormal);
#			endif

	result.normal = finalNormal;
	return result;
}

float3 GetWaterSpecularColor(PS_INPUT input, float3 normal, float3 viewDirection, float distanceFactor, float skylightingSpecular)
{
#			if !defined(DYNAMIC_CUBEMAPS)
	if (!(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections))
		return ReflectionColor.xyz * VarAmounts.y;
#			endif

	float3 R = reflect(viewDirection, WaterParams.y * normal + float3(0, 0, 1 - WaterParams.y));

#			if defined(DYNAMIC_CUBEMAPS)
	if (SharedData::InInterior)
		return DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, 0).xyz;

	float3 workingSpecularIrradiance = 0.0;
	if (skylightingSpecular < 1.0)
		workingSpecularIrradiance = DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, 0).xyz;

	float3 workingSpecularIrradianceReflections = 0.0;
	if (skylightingSpecular > 0.0) {
		workingSpecularIrradianceReflections = DynamicCubemaps::EnvReflectionsTexture.SampleLevel(CubeMapSampler, R, 0).xyz;
		if ((Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections) && !SharedData::HideSky) {
			// Only the sky-visible lobe, without the specular floor, fades to the engine cube
			float minVisibility = SharedData::skylightingSettings.MinSpecularVisibility;
			float skyVisibility = minVisibility < 1.0 ? saturate((skylightingSpecular - minVisibility) / (1.0 - minVisibility)) : 1.0;
			float reflectionAmount = saturate(length(input.WPosition.xyz) / 1024.0) * skyVisibility;
			workingSpecularIrradianceReflections = lerp(workingSpecularIrradianceReflections, CubeMapTex.SampleLevel(CubeMapSampler, R, 0).xyz, reflectionAmount);
		}
	}

	float3 reflectionColor = ColorManagement::SceneColor::LerpInLinear(workingSpecularIrradiance, workingSpecularIrradianceReflections, skylightingSpecular);
#			else
	float3 reflectionColor = CubeMapTex.SampleLevel(CubeMapSampler, R, 0).xyz;
#			endif

#			if !defined(LOD) && NUM_SPECULAR_LIGHTS == 0
	float pointingDirection = dot(viewDirection, R) * 0.5 + 0.5;
	float pointingAlignment = dot(reflect(viewDirection, float3(0, 0, 1)), R) * 0.5 + 0.5;
	float ssrAmount = sqrt(min(pointingAlignment, pointingDirection));
	float2 ssrReflectionUv = ((FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy) * SSRParams.zw) + 0.05 * normal.xy;
	float2 ssrReflectionUvDR = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(ssrReflectionUv);
	float4 ssrReflectionColorBlurred = SSRReflectionTex.Sample(SSRReflectionSampler, ssrReflectionUvDR);
	float4 ssrReflectionColorRaw = RawSSRReflectionTex.Sample(RawSSRReflectionSampler, ssrReflectionUvDR);
	float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7);
	float3 finalSsrReflectionColor = max(0, PreExposure::Decode(ssrReflectionColor.xyz));
	float ssrFraction = saturate(ssrReflectionColor.w * distanceFactor * ssrAmount);
	reflectionColor = lerp(reflectionColor, finalSsrReflectionColor, ssrFraction);
#			endif

	return reflectionColor;
}

// Effects 11 water: depth over which the surface reaches full opacity at the shore
static const float ShoreFadeDepth = 12.0;

float GetScreenDepthWater(float2 screenPosition)
{
	float depth = DepthTex.Load(float3(screenPosition, 0)).x;
	return (CameraDataWater.w / (-depth * CameraDataWater.z + CameraDataWater.x));
}

float3 GetLdotN(float3 normal)
{
#			if defined(UNDERWATER)
	return 1;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 1;
	return LightDisk::DiffuseCosine(dot(SunDir.xyz, normal), LightDisk::DirectionalSource());
#			endif
}

float GetFresnelValue(float3 normal, float3 viewDirection)
{
#			if defined(UNDERWATER)
	float3 actualNormal = -normal;
#			else
	float3 actualNormal = normal;
#			endif
	float viewAngle = 1 - saturate(dot(-viewDirection, actualNormal));

	if (SharedData::enbSettings.EnableWater) {
		float fresnelExponent = SharedData::enbSettings.WaterFresnelMultiplier;
		// pow(0, 0) is NaN on GPU
		float fresnelRI = fresnelExponent != 0.0 ? pow(abs(FresnelRI.x), fresnelExponent) : 1.0;
		float fresnel = (1 - fresnelRI) * pow(viewAngle, 5) + fresnelRI;
		fresnel = lerp(SharedData::enbSettings.WaterFresnelMin, SharedData::enbSettings.WaterFresnelMax, fresnel);
		return fresnel * SharedData::enbSettings.WaterReflectionAmount;
	}

	if (UsePhysicalWaterReflections())
		return GetWaterFresnel(dot(-viewDirection, actualNormal));

	return (1 - FresnelRI.x) * pow(viewAngle, 5) + FresnelRI.x;
}

struct DiffuseOutput
{
	float3 refractionColor;
	float3 refractionDiffuseColor;
	float depth;
	float refractionMul;
	float3 refractedViewDirection;
};

DiffuseOutput GetWaterDiffuseColor(PS_INPUT input, float3 normal, float3 viewDirection, inout float4 distanceMul, float refractionsDepthFactor, float fresnel, float3 viewPosition, float depth)
{
#			if defined(REFRACTIONS)
	float4 refractionNormal = mul(transpose(TextureProj), float4((VarAmounts.w * refractionsDepthFactor * normal.xy) + input.MPosition.xy, input.MPosition.z, 1));

	float2 refractionUvRaw = float2(refractionNormal.x, refractionNormal.w - refractionNormal.y) / refractionNormal.ww;
	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

	float2 refractionScreenPosition = FrameBuffer::DynamicResolutionParams1.xy * (refractionUvRaw / VPOSOffset.xy);
	float4 refractionWorldPosition = float4(input.WPosition.xyz * depth / viewPosition.z, 0);

#				if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	float refractionDepth = GetScreenDepthWater(refractionScreenPosition);
	depth = refractionDepth;
	float refractionDepthMul = length(float3((((VPOSOffset.zw + refractionUvRaw) * 2 - 1)) * refractionDepth / ProjData.xy, refractionDepth));

	float3 refractionDepthAdjustedViewDirection = -viewDirection * refractionDepthMul;
	float refractionViewSurfaceAngle = dot(refractionDepthAdjustedViewDirection, ReflectPlane[0].xyz);

	float refractionPlaneMul = (1 - ReflectPlane[0].w / refractionViewSurfaceAngle);

	if (refractionPlaneMul < 0.0) {
		refractionUvRaw = FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	} else {
		float muddiness = SharedData::enbSettings.EnableWater ? SharedData::enbSettings.WaterMuddiness : 1.0;
		distanceMul = saturate(float4(1, muddiness, 1, 1) * refractionPlaneMul * float4(length(refractionDepthAdjustedViewDirection).xx, abs(refractionViewSurfaceAngle).xx) / FogParam.z);

		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse, float4((refractionUvRaw * 2 - 1) * float2(1, -1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
		refractionWorldPosition.xyz /= refractionWorldPosition.w;
	}

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(refractionScreenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#				endif

	float2 refractionUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(refractionUvRaw);
	float3 refractionColor = PreExposure::Decode(RefractionTex.Sample(RefractionSampler, refractionUV).xyz);

#				if defined(UNDERWATER)
	float refractionMul = 0;
#				else
	float refractionMul = 1 - pow(saturate((-distanceMul.x * FogParam.z + FogParam.z) / FogParam.w), FogNearColor.w);
#				endif

	float3 refractionDiffuseColor;

	if (SharedData::enbSettings.EnableWater) {
		float3 shallowColor = ShallowColor.xyz;
		float maxValue = max(shallowColor.x, max(shallowColor.y, shallowColor.z));
		if (maxValue > 0.0)
			shallowColor /= maxValue;
		else
			shallowColor = 1.0;

		// Tinted refraction only reads near the surface; deeper water fades to the flat shallow color
		shallowColor = lerp(shallowColor.xyz * refractionColor, ShallowColor.xyz, lerp(SharedData::enbSettings.WaterMuddiness, 1.0, refractionMul));
		refractionDiffuseColor = lerp(shallowColor.xyz, DeepColor.xyz, distanceMul.y);
	} else {
		refractionDiffuseColor = lerp(ShallowColor.xyz, DeepColor.xyz, distanceMul.y);
	}

	DiffuseOutput output;
	output.refractionColor = refractionColor;
	output.refractionDiffuseColor = refractionDiffuseColor;
	output.depth = depth;
	output.refractionMul = refractionMul;
	output.refractedViewDirection = normalize(refractionWorldPosition.xyz - input.WPosition.xyz);
	return output;
#			else
	DiffuseOutput output;
	output.refractionColor = lerp(ShallowColor.xyz, DeepColor.xyz, fresnel) * GetLdotN(normal);
	output.refractionDiffuseColor = output.refractionColor;
	output.depth = 1;
	output.refractionMul = 1;
	output.refractedViewDirection = viewDirection;
	return output;
#			endif
}

float3 GetSunColor(float3 normal, float3 viewDirection, float3 worldPosition)
{
#			if defined(UNDERWATER)
	return 0.0.xxx;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 0.0.xxx;

	float lightDistribution;
	if (UsePhysicalWaterReflections()) {
		lightDistribution = GetWaterDeltaLightDistributionFromCos(normal, viewDirection, SunDir.xyz, GetWaterLightDiskCos()) * GetWaterDirectSpecularScale();
	} else {
		float3 reflectionDirection = reflect(viewDirection, normal);
		lightDistribution = VarAmounts.x > 0 ? exp2(VarAmounts.x * log2(saturate(dot(reflectionDirection, SunDir.xyz)))) : 1.0;
	}

	float3 sunColor = Color::DirectionalLight(ColorManagement::SRGBToWorking(SunColor.xyz)) * SunDir.w * (1.0 - exp(-DeepColor.w));
#				if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		sunColor *= PhysSky::SampleTr(normalize(reflect(viewDirection, normal)), DepthSampler);
		sunColor *= PhysSky::GetDirlightTransmittance(worldPosition + FrameBuffer::CameraPosAdjust.xyz, DepthSampler, true);
	}
#				endif

#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		sunColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#				endif
	return lightDistribution * sunColor;
#			endif
}
#		endif

#		if defined(LIGHT_LIMIT_FIX)
#			include "LightLimitFix/LightLimitFix.hlsli"
#		endif

#		if defined(IBL)
#			include "IBL/IBL.hlsli"
#		endif

#		if defined(CS_ORG_BINDLESS) || defined(DCLF_BINDLESS)
PS_OUTPUT WaterPixel(PS_INPUT input)
#		else
PS_OUTPUT main(PS_INPUT input)
#		endif
{
	PS_OUTPUT psout;

	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
	float3 viewDirection = normalize(input.WPosition.xyz);

	float distanceFactor = saturate(lerp(FrameBuffer::FrameParams.w, 1, (length(input.WPosition.xyz) - 8192) / (WaterParams.x - 8192)));
	float4 distanceMul = saturate(lerp(VarAmounts.z, 1, -(distanceFactor - 1))).xxxx;
	float distanceBlendFactor = distanceFactor;
#			if defined(UNIFIED_WATER)
	distanceBlendFactor = 1.0f;
#			endif

	bool isSpecular = false;

	float depth = 0;

#			if defined(DEPTH)
#				if defined(VERTEX_ALPHA_DEPTH)
#					if defined(VC)
	distanceMul = saturate(input.TexCoord3.z);
#					endif
#				else
	distanceMul = 0;

	depth = GetScreenDepthWater(screenPosition);
	float2 depthOffset =
		FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	float depthMul = length(float3((depthOffset * 2 - 1) * depth / ProjData.xy, depth));
	float3 depthAdjustedViewDirection = -viewDirection * depthMul;
	float viewSurfaceAngle = dot(depthAdjustedViewDirection, ReflectPlane[0].xyz);

	float planeMul = (1 - ReflectPlane[0].w / viewSurfaceAngle);
	distanceMul = saturate(
		planeMul * float4(length(depthAdjustedViewDirection).xx, abs(viewSurfaceAngle).xx) /
		FogParam.z);

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(screenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#				endif
#			endif

#			if defined(UNDERWATER)
	float4 depthControl = float4(0, 1, 1, 0);
#			elif defined(LOD)
	float4 depthControl = float4(1, 0, 0, 1);
#			elif defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float4 depthControl = float4(0, 0, 1, 0);
#			else
	float4 depthControl = DepthControl * (distanceMul - 1) + 1;
#			endif
	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

#			if defined(SKYLIGHTING)
	float wetnessOcclusion = 1.0;

	float3 positionMSSkylight = input.WPosition.xyz;

	sh2 skylightingSH = Skylighting::Sample(positionMSSkylight, float3(0, 0, 1));
	float skylighting = SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1));

	float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, float3(0, 0, 1), Skylighting::GetFadeOutFactor(input.WPosition.xyz));

	wetnessOcclusion = inWorld ? pow(saturate(skylighting), 2) : 0;
#			endif

#			if defined(SKYLIGHTING)
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, wetnessOcclusion);
#			else
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, inWorld);
#			endif

	float3 normal = waterData.normal;

#			if defined(SKYLIGHTING)
	sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, 0.0);
	float skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#			endif

	float fresnel = GetFresnelValue(normal, viewDirection);

#			if defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float3 finalColor = 0.0.xxx;

#				if !defined(LIGHT_LIMIT_FIX)
	[unroll] for (int lightIndex = 0; lightIndex < NUM_SPECULAR_LIGHTS; ++lightIndex)
	{
		float3 lightVector = LightPos[lightIndex].xyz - (PosAdjust.xyz + input.WPosition.xyz);
		float3 lightDirection = normalize(normalize(lightVector) - viewDirection);
		float lightFade = saturate(length(lightVector) / LightPos[lightIndex].w);
		float lightColorMul = (1 - lightFade * lightFade);
#					if defined(ENABLE_LL)
		lightColorMul = pow(lightColorMul, TransferFunctions::GAME_GAMMA);
#					endif
		float LdotN = saturate(dot(lightDirection, normal));
		float3 lightColor = (Color::PointLight(LightColor[lightIndex].xyz) * pow(LdotN, FresnelRI.z)) * lightColorMul;
		finalColor += lightColor;
	}
#				endif

	finalColor *= fresnel;
#				if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override specular color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorSpecular(waterData.rippleInfo, 2.5, 4.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#				endif

	isSpecular = true;
#			else

#				if defined(SKYLIGHTING)
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, skylightingSpecular);
#				else
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, 1.0);
#				endif

	DiffuseOutput diffuseOutput = GetWaterDiffuseColor(input, normal, viewDirection, distanceMul, depthControl.y, fresnel, viewPosition, depth);

	float surfaceShadow;
	float dirShadow = ShadowSampling::Get3DFilteredShadow(input.WPosition.xyz, diffuseOutput.refractedViewDirection, input.HPosition.xy, surfaceShadow);

	float3 dirColor;
	float3 ambientColor;
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, input.WPosition.xyz, dirColor, ambientColor);

	dirColor *= dirShadow;

#				if defined(SKYLIGHTING)
	ambientColor = ColorManagement::SceneColor::ScaleByLinear(ambientColor, skylightingDiffuse);
#				endif

	diffuseOutput.refractionDiffuseColor = dirColor + ambientColor;

#				if !defined(UNDERWATER)
	if (SharedData::enbSettings.EnableWater) {
		const float2 lightDisk = LightDisk::DirectionalSource();
		const float normalScatter = LightDisk::WrappedCosine(dot(normal.xyz, SharedData::DirLightDirection.xyz), 1.0, 0.5, lightDisk);
		const float viewScatter = LightDisk::WrappedCosine(dot(viewDirection.xyz, SharedData::DirLightDirection.xyz), 1.0, 0.5, lightDisk);
		float3 dirScatter = normalScatter * viewScatter * ShadowSampling::GetDirectionalLighting();
		diffuseOutput.refractionDiffuseColor += DeepColor.xyz * dirScatter * surfaceShadow * SharedData::enbSettings.WaterSunLightingMultiplier;
	}
#				endif

	float3 diffuseColor = lerp(diffuseOutput.refractionColor, diffuseOutput.refractionDiffuseColor, diffuseOutput.refractionMul);

	depthControl = DepthControl * (distanceMul - 1) + 1;

	float3 specularLighting = 0;

#				if defined(LIGHT_LIMIT_FIX)
	uint lightCount = 0;

	uint clusterIndex = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
			LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
			// Water has no engine shadow mask, so only shadow casters with a cached local shadow can light it.
			const bool localShadow = light.lightFlags & LightLimitFix::LightFlags::LocalShadow;
			if (LightLimitFix::IsLightIgnored(light) || ((light.lightFlags & LightLimitFix::LightFlags::ShadowCaster) && !localShadow)) {
				continue;
			}

			float3 lightDirection = light.positionWS.xyz - input.WPosition.xyz;
			float lightDist = length(lightDirection);

			float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);

			float3 normalizedLightDirection = normalize(lightDirection);

			float3 H = normalize(normalizedLightDirection - viewDirection);
			float HdotN = saturate(dot(H, normal));

			float3 lightColor = Color::PointLight(light.color.xyz) * pow(HdotN, FresnelRI.z) * light.fade * intensityMultiplier;

			// Water lights only with its narrow specular lobe, which a lamp's reflection fills on a few pixels: the local
			// shadow (a filtered cube map lookup) is only read where the lobe adds something.
			[branch] if (localShadow && max(lightColor.x, max(lightColor.y, lightColor.z)) > 1e-4)
				lightColor *= LightLimitFix::GetLocalShadowWithLightFade(DepthSampler, light.localShadowIndex, input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, normalizedLightDirection, false, Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount));

			specularLighting += lightColor;
		}
	}
	specularColor += specularLighting * 3;
#				endif

#				if defined(UNDERWATER)
	float3 finalSpecularColor = lerp(ShallowColor.xyz, specularColor, 0.5);
	float3 finalColor = saturate(1 - length(input.WPosition.xyz) * 0.002) * ((1 - fresnel) * (diffuseColor - finalSpecularColor)) + finalSpecularColor;
	// Add ripple and splash color effects for underwater
#					if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization (darker for underwater)
	float3 debugColor = WetnessEffects::GetDebugWetnessColorUnderwater(waterData.rippleInfo, 1.5, 2.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#					endif
#				else

	float3 sunColor = GetSunColor(normal, viewDirection, input.WPosition.xyz) * surfaceShadow;

	// Reflection and sun weights. On refracting water vanilla fades both with refractionMul, which only
	// reaches 1 once the water is deep enough to fog, so shallow water lost its reflections.
	float surfaceMul = diffuseOutput.refractionMul;
	float sunMul = depthControl.w;
	bool shoreFadedSurface = false;

	if (SharedData::enbSettings.EnableWater) {
		sunColor *= SharedData::enbSettings.WaterSunSpecularMultiplier;
#					if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH) && defined(REFRACTIONS)
		// distanceMul.w is the water depth below the surface over FogParam.z, saturated
		surfaceMul = saturate(distanceMul.w * FogParam.z / max(min(ShoreFadeDepth, FogParam.z), 1e-4));
		sunMul = max(sunMul, surfaceMul);
		shoreFadedSurface = true;
#					endif
	}

#					if defined(VC)
	float specularFraction = lerp(1, fresnel * surfaceMul, distanceBlendFactor);
	float3 finalColorPreFog = lerp(diffuseColor, specularColor, specularFraction) + sunColor * sunMul;

#						if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		const float3 waterViewDir = normalize(input.WPosition.xyz);
		const float waterDist = length(input.WPosition.xyz);
		const float4 apSample = PhysSky::SampleAp(waterViewDir, input.HPosition.xy, waterDist, ApSampler);
		finalColorPreFog = finalColorPreFog * apSample.w + apSample.xyz;
		if (!SharedData::PostWaterComposite)
			finalColorPreFog = PhysSky::CompositeVolumetricClouds(finalColorPreFog, input.HPosition.xy);
	}
#						endif

	if (!SharedData::PostWaterComposite) {
#						if !defined(UNIFIED_WATER)
		float fogDistanceFactor = input.FogParam.w;
		float3 fogColor = input.FogParam.xyz;
#						else
		float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
		float3 fogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogDistanceFactor);
#						endif

#						if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			fogColor = ImageBasedLighting::GetFogIBLColor(fogColor, input.WPosition.xyz);
		}
#						endif
#						if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled) {
			float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, fogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
			if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
				fogColor = exponentialHeightFog.xyz;
				fogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, fogColor, exponentialHeightFog.w);
			} else {
				fogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
				float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
			}
		} else {
			fogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
		}
#						else
		fogColor *= GetWaterFogFade();
		finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
#						endif
	}

	float3 finalColor = finalColorPreFog;

#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif

#					else
	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
	float waterOpacity = diffuseOutput.refractionMul;
	float3 finalColorPreFog;
	if (shoreFadedSurface) {
		// Reflection takes its share first; the rest splits between the water color and the see-through refraction.
		// The sun is a reflection too, so it fades in with the surface instead of rimming see-through water.
		// Divided by the opacity because the composite below lerps from the refraction by it; the floor keeps that
		// continuous and moves at most 0.1% of the weight off the refraction.
		float reflectionWeight = specularFraction * surfaceMul;
		waterOpacity = max(lerp(diffuseOutput.refractionMul, 1.0, reflectionWeight), 1e-3);
		finalColorPreFog = (diffuseOutput.refractionDiffuseColor * (diffuseOutput.refractionMul * (1.0 - reflectionWeight)) + specularColor * reflectionWeight + sunColor * (sunMul * surfaceMul)) / waterOpacity;
	} else {
		finalColorPreFog = lerp(diffuseOutput.refractionDiffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;
	}

#						if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		const float3 waterViewDir = normalize(input.WPosition.xyz);
		const float waterDist = length(input.WPosition.xyz);
		const float4 apSample = PhysSky::SampleAp(waterViewDir, input.HPosition.xy, waterDist, ApSampler);
		finalColorPreFog = finalColorPreFog * apSample.w + apSample.xyz;
		if (!SharedData::PostWaterComposite)
			finalColorPreFog = PhysSky::CompositeVolumetricClouds(finalColorPreFog, input.HPosition.xy);
	}
#						endif

	if (!SharedData::PostWaterComposite) {
#						if !defined(UNIFIED_WATER)
		float fogDistanceFactor = input.FogParam.w;
		float3 preFogColor = input.FogParam.xyz;
#						else
		float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
		float3 preFogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogDistanceFactor);
#						endif

#						if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			preFogColor = ImageBasedLighting::GetFogIBLColor(preFogColor, input.WPosition.xyz);
		}
#						endif
#						if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled) {
			float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, preFogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
			if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
				preFogColor = exponentialHeightFog.xyz;
				preFogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, preFogColor, exponentialHeightFog.w);
			} else {
				preFogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
				float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
			}
		} else {
			preFogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
		}
#						else
		preFogColor *= GetWaterFogFade();

		finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
#						endif
	}

	float3 refractionColor = diffuseOutput.refractionColor;

	if (!SharedData::PostWaterComposite) {
		float fogFactor = min(FogParam.w, pow(saturate(-diffuseOutput.depth * FogParam.y - FogParam.x), FogParam.z));
		float3 fogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogFactor);
#						if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled && ExponentialHeightFog::ShouldDisableVanillaFog()) {
			fogFactor = 0;
		}
#						endif
#						if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			fogColor = ImageBasedLighting::GetFogIBLColor(fogColor, input.WPosition.xyz);
		}
#						endif
		refractionColor = lerp(refractionColor, fogColor, fogFactor);
	}

	float3 finalColor = lerp(refractionColor, finalColorPreFog, waterOpacity);
#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif
#					endif

#				endif
#			endif
	psout.Lighting = float4(PreExposure::EncodeScene(finalColor), isSpecular);
#		endif

#		if defined(STENCIL)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	float3 normal =
		normalize(cross(ddx_coarse(input.WorldPosition.xyz), ddy_coarse(input.WorldPosition.xyz)));
	float VdotN = dot(viewDirection, normal);
	psout.WaterMask = float4(0, 0, VdotN, 0);

	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);
#		endif

	return psout;
}

#		if defined(CS_ORG_BINDLESS)
PS_OUTPUT main(PS_INPUT input)
{
	WaterOrgPermutation();
	return WaterPixel(input);
}
#		elif defined(DCLF_BINDLESS)
/** @brief The draw's constants. The mask's stage reads none of them. */
void DCLFWaterPixelInputs()
{
#			if !defined(STENCIL)
	DCLFWaterPixelStatics();
#			endif
}

#			if defined(DCLF_PULLED)
// A plain draw's rows and object word are its vertex stage's sequence's (Effect.hlsl's pulled pixel stage).
PS_OUTPUT DCLFWaterPulledPixel(PS_INPUT input, uint a_instance)
{
	DCLFWaterSequence(a_instance);
	DCLFWaterPixelInputs();
	return WaterPixel(input);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	[loop] for (;;) {
		const uint drawInstance = WaveReadLaneFirst(input.DCLFDrawInstance);
		[branch] if (drawInstance == input.DCLFDrawInstance) {
			psout = DCLFWaterPulledPixel(input, drawInstance);
			break;
		}
	}
	return psout;
}
#			else
PS_OUTPUT main(PS_INPUT input)
{
	DCLFWaterPixelInputs();
	return WaterPixel(input);
}
#			endif
#		endif

#	endif

#endif
