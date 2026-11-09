#include "Upscaling/UpscaleVS.hlsl"

#ifdef PSHADER
#	include "Common/OrgResources.hlsli"

#	define COPY_HUDLESS_RESOURCES(X) 		X(SRV, Texture2D<float4>, ColorInput, 0)
ORG_PUSH_BLOCK(COPY_HUDLESS_RESOURCES(ORG_HANDLE))
COPY_HUDLESS_RESOURCES(ORG_RESOURCE)

float4 main(VS_OUTPUT input) : SV_Target
{
	return ColorInput.Load(int3(input.Position.xy, 0));
}

#endif
