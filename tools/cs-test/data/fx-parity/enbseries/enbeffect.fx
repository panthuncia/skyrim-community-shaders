// FX parity test: cbuffer values, #pragma pack_matrix(row_major), a RenderTarget annotation naming a common texture.
#include "parity_common.fxh"

#pragma pack_matrix(row_major)
// Asymmetric, so a transposed read changes the colours: red leaks into green.
float4x4 ParityMatrix = { 1.0, 0.0, 0.0, 0.0,
						  0.3, 0.8, 0.0, 0.0,
						  0.0, 0.0, 1.0, 0.0,
						  0.0, 0.0, 0.0, 1.0 };
#pragma pack_matrix(column_major)

cbuffer ParityValues
{
	float4 ParityTint < string UIName = "Parity tint"; string UIWidget = "Color"; > = { 1.0, 0.92, 0.85, 1.0 };
	float ParityExposure < string UIName = "Parity exposure"; float UIMin = 0.0; float UIMax = 4.0; > = { 1.2 };
};

Texture2D TextureColor;
Texture2D TextureBloom;
Texture2D TextureAperture;

float4 PS_Side(VS_OUTPUT_POST IN) : SV_Target
{
	return TextureColor.Sample(Smp, IN.txcoord) * 0.5;
}

float4 PS_Draw(VS_OUTPUT_POST IN) : SV_Target
{
	float3 c = TextureColor.Sample(Smp, IN.txcoord).rgb;
	c += TextureBloom.Sample(Smp, IN.txcoord).rgb * 0.25;
	c = mul(ParityMatrix, float4(c, 1.0)).rgb;
	c *= ParityTint.rgb * ParityExposure * (1.0 + 0.1 * TextureAperture.Sample(Smp, float2(0.5, 0.5)).x);
	c = c / (1.0 + c);
	return float4(c, 1.0);
}

// A side target by a common texture's name: the chain stays where it was.
technique11 Draw < string UIName = "Parity"; string RenderTarget = "TextureHDRTemp"; >
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Side()));
	}
}

technique11 Draw1
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Draw()));
	}
}
