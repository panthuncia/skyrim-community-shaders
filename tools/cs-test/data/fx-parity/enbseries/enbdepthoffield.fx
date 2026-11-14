// FX parity test: depth of field with aperture and focus history, and a two-technique chain (HDRTemp, HDRTemp2).
#include "parity_common.fxh"

float4 DofParameters;

Texture2D TextureColor;
Texture2D TextureOriginal;
Texture2D TextureDepth;
Texture2D TextureCurrent;
Texture2D TexturePrevious;
Texture2D TextureFocus;
Texture2D TextureAperture;

float4 PS_Aperture(VS_OUTPUT_POST IN) : SV_Target
{
	const float previous = TexturePrevious.Sample(Smp, float2(0.5, 0.5)).x;
	return lerp(previous, 0.75, DofParameters.z);
}

// 16x16: the depth around the centre.
float4 PS_ReadFocus(VS_OUTPUT_POST IN) : SV_Target
{
	return TextureDepth.SampleLevel(Smp, 0.4 + IN.txcoord * 0.2, 0).x;
}

float4 PS_Focus(VS_OUTPUT_POST IN) : SV_Target
{
	float focus = 0;
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 4; ++x)
			focus += TextureCurrent.SampleLevel(Smp, (float2(x, y) * 4.0 + 2.0) / 16.0, 0).x;
	return lerp(TexturePrevious.Sample(Smp, float2(0.5, 0.5)).x, focus / 16.0, DofParameters.w);
}

float4 Blur(float2 uv, float radius)
{
	const float2 px = ScreenSize.y * float2(1.0, ScreenSize.z) * radius;
	float4 c = 0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
			c += TextureColor.Sample(Smp, uv + float2(x, y) * px);
	return c / 9.0;
}

float4 PS_Dof(VS_OUTPUT_POST IN) : SV_Target
{
	const float depth = TextureDepth.SampleLevel(Smp, IN.txcoord, 0).x;
	const float focus = TextureFocus.Sample(Smp, float2(0.5, 0.5)).x;
	const float amount = saturate(abs(depth - focus) * 40.0) * TextureAperture.Sample(Smp, float2(0.5, 0.5)).x;
	return lerp(TextureOriginal.Sample(Smp, IN.txcoord), Blur(IN.txcoord, 3.0), amount);
}

float4 PS_Dof1(VS_OUTPUT_POST IN) : SV_Target
{
	return lerp(TextureColor.Sample(Smp, IN.txcoord), Blur(IN.txcoord, 1.5), 0.5);
}

technique11 Aperture
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Aperture()));
	}
}

technique11 ReadFocus
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_ReadFocus()));
	}
}

technique11 Focus
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Focus()));
	}
}

technique11 Dof < string UIName = "Parity DoF"; >
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Dof()));
	}
}

technique11 Dof1
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Dof1()));
	}
}
