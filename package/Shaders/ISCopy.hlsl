// ISCopyScaleBias and ISCopyTextureMask move the texture coordinates in their vertex shader, as vanilla's do. The other
// copies pass them through.
#if defined(VSHADER) && (defined(SCALE_BIAS) || defined(MASK))
struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float2 TexCoord: TEXCOORD0;
};

cbuffer PerGeometry : register(b2)
{
#	if defined(MASK)
	float4 RotationMatrix : packoffset(c0);  // a 2x2 matrix that turns the coordinates about the center
#	else
	float4 ScaleBias : packoffset(c2);  // xy: scale, zw: bias
#	endif
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	vsout.Position = float4(input.Position.xyz, 1);
#	if defined(MASK)
	const float4 rotated = (input.TexCoord.xxyy - 0.5) * RotationMatrix;
	vsout.TexCoord = (rotated.xy + rotated.zw) + 0.5;
#	else
	vsout.TexCoord = input.TexCoord * ScaleBias.xy + ScaleBias.zw;
#	endif

	return vsout;
}
#else
#	include "Common/DummyVSTexCoord.hlsl"
#endif
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#if defined(DEPTHBUFFER_COPY)
	float Depth: SV_Depth;
#else
	float4 Color: SV_Target0;
#endif
};

#if defined(PSHADER)
SamplerState ImageSampler : register(s0);

Texture2D<float4> ImageTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 RotationMatrix : packoffset(c0);
	float4 ColorSelect : packoffset(c1);
	float4 ScaleBias : packoffset(c2);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if !defined(DISABLE_DYNAMIC) || (defined(DEPTHBUFFER_COPY) && defined(DEPTHBUFFER_4X_DOWNSAMPLE))
	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);
#	else
	float2 screenPosition = input.TexCoord;
#	endif

	float4 color = ImageTex.Sample(ImageSampler, screenPosition);

#	if defined(GREYSCALE)
	color = float4(dot(color, ColorSelect).xxx, color.w);
#	elif defined(MASK)
	color.w = 1 - color.x;
#	endif

#	if defined(DEPTHBUFFER_COPY)
	psout.Depth = color.x;
#	else
	psout.Color = color;
#	endif

	return psout;
}
#endif
