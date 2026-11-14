// FX parity test: blend and rasterizer states, state inheritance between passes, a sample mask, the blend factor.
#include "parity_common.fxh"

Texture2D TextureColor;

BlendState FactorBlend
{
	BlendEnable[0] = TRUE;
	SrcBlend = BLEND_FACTOR;
	DestBlend = INV_BLEND_FACTOR;
	BlendOp = ADD;
	SrcBlendAlpha = ONE;
	DestBlendAlpha = ZERO;
};

BlendState AddBlend
{
	BlendEnable[0] = TRUE;
	SrcBlend = ONE;
	DestBlend = ONE;
};

RasterizerState NoCull
{
	CullMode = NONE;
};

RasterizerState CullFront
{
	CullMode = FRONT;
};

float4 PS_Copy(VS_OUTPUT_POST IN) : SV_Target
{
	return TextureColor.Sample(Smp, IN.txcoord);
}

// A dark vignette, blended in at a quarter through the blend factor.
float4 PS_Vignette(VS_OUTPUT_POST IN) : SV_Target
{
	const float2 d = IN.txcoord - 0.5;
	return float4((1.0 - saturate(dot(d, d) * 2.5)).xxx * TextureColor.Sample(Smp, IN.txcoord).rgb, 1.0);
}

// Never visible: culled, masked, or culled through the state it inherits.
float4 PS_Red(VS_OUTPUT_POST IN) : SV_Target
{
	return float4(1.0, 0.0, 0.0, 1.0);
}

technique11 PostPass < string UIName = "Parity post"; >
{
	pass copy
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Copy()));
	}
	pass vignette
	{
		SetBlendState(FactorBlend, float4(0.25, 0.25, 0.25, 0.25), 0xFFFFFFFF);
		SetRasterizerState(NoCull);
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Vignette()));
	}
	pass culled
	{
		SetBlendState(AddBlend, float4(0.0, 0.0, 0.0, 0.0), 0xFFFFFFFF);
		SetRasterizerState(CullFront);
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Red()));
	}
	pass inherits
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Red()));
	}
	pass masked
	{
		SetBlendState(NULL, float4(0.0, 0.0, 0.0, 0.0), 0xFFFFFFFE);
		SetRasterizerState(NULL);
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Red()));
	}
}
