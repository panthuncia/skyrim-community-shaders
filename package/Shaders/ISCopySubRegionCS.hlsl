// ISCopySubRegionCS: copies a rectangle of one texture into another, transcribed from the vanilla DXBC. The snow and
// water passes use it. The engine uploads the constants whole (SetComputeShaderConstants), so only their layout matters.

#if defined(CS_ORG_BINDLESS)
// The ORG build (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0 and u0 from the heap, and the constants the engine uploaded
// for this dispatch (CS b0) from the engine-constants capture through the pass's latch. The water's flow copies read a different
// loaded texture each dispatch: its heap index is the latch's Source, which replaces t0 then.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISCOPYSUBREGIONCS_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, BoundInputTexture, 0) X(UAV, RWTexture2D<float4>, OutputTexture, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 0)
ORG_PUSH_BLOCK(ISCOPYSUBREGIONCS_ORG_RESOURCES(ORG_HANDLE))
ISCOPYSUBREGIONCS_ORG_RESOURCES(ORG_RESOURCE)
static Texture2D<float4> InputTexture = ResourceDescriptorHeap[PerGeometryAt.Source != ORG_ENGINE_NO_SOURCE ? PerGeometryAt.Source : OrgHandle_BoundInputTexture];
// The names are the engine's (the constructor's constant list).
static uint4 srcRect = asuint(OrgEngineRow(PerGeometryAt, 0));        // xy: the rectangle's corner in the input, zw: its size
static uint2 destOffset = asuint(OrgEngineRow(PerGeometryAt, 1)).xy;  // where the corner lands in the output
#else
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer PerTechnique : register(b0)
{
	// The names are the engine's (the constructor's constant list).
	uint4 srcRect;     // xy: the rectangle's corner in the input, zw: its size
	uint2 destOffset;  // where the corner lands in the output
	uint2 padding;
};
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (all(dispatchID.xy < srcRect.zw)) {
#if defined(CS_ORG_BINDLESS)
		// D3D drops stores outside the texture. Vulkan does not promise to, so test here.
		uint2 size;
		OutputTexture.GetDimensions(size.x, size.y);
		if (any(dispatchID.xy + destOffset >= size))
			return;
#endif
		OutputTexture[dispatchID.xy + destOffset] = InputTexture.Load(int3(dispatchID.xy + srcRect.xy, 0));
	}
}
