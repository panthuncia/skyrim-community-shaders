// BSLensFlare: one sprite of a lens flare, transcribed from the vanilla DXBC. The engine has no source file name for this
// shader (its originalShaderName is null), so ShaderCache maps it to this file. The constant names are the engine's
// (the BSLensFlare constructor) and it fills them by name; the registers are vanilla's.

struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float4 TexCoord: TEXCOORD0;  // xy: sprite UV, z: a tenth of the sprite's distance from the screen center
	float3 LightPos: TEXCOORD1;  // the light's screen position, passed through
};

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(VSHADER)
cbuffer PerGeometry : register(b2)
{
	float4 UV : packoffset(c1);                                 // xy and zw: the sprite's axes for the vertex x and y
	float4 ScreenSpaceLightPos_PositionScale : packoffset(c2);  // xy: light position in UV, w: how far along the light axis
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float2 position = ScreenSpaceLightPos_PositionScale.xy * 2.0 - 1.0;
	position = position * ScreenSpaceLightPos_PositionScale.w + input.Position.x * UV.xy;
	position = input.Position.y * UV.zw + position;

	vsout.Position = float4(position, 0, 1);
	vsout.TexCoord = float4(input.TexCoord.xy, length(position) * 0.1, 0);
	vsout.LightPos = ScreenSpaceLightPos_PositionScale.xyz;

	return vsout;
}
#endif

#if defined(PSHADER)
SamplerState LensFlareSampler : register(s0);

Texture2D<float4> LensFlareTex : register(t0);
Texture2D<float4> VisTex : register(t1);  // one texel per light: the visibility the occlusion queries measured

cbuffer PerGeometry : register(b2)
{
	float4 Color : packoffset(c0);        // xyz: tint, w > 0: scale the sprite by its visibility
	float4 VisScale : packoffset(c1);     // weights of the visibility texel's channels
	float LightIndex : packoffset(c2.x);  // which visibility texel
	float GlobalIntensity : packoffset(c2.y);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	const float visibility = dot(VisScale, VisTex.Load(int3((int)LightIndex, 0, 0)));
	const float3 tint = (visibility * Color.xyz) * GlobalIntensity;

	float2 texCoord = input.TexCoord.xy;
	if (Color.w > 0) {
		texCoord = (texCoord - 0.5) / (visibility + 0.001) + 0.5;
	}

	psout.Color = float4(tint * LensFlareTex.Sample(LensFlareSampler, texCoord).xyz, 0);

	return psout;
}
#endif
