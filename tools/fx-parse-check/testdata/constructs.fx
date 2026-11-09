// Constructs the shipped presets do not use, so the parser's other paths have something to read. Not a working effect.
// Expected: no errors; warnings for the blend/raster states, the DX9 sampler_state, the FX9 render state and the
// geometry shader, which the ORG runtime would not run.

#define UI_FLOAT(name, label, lo, hi, def) float name < string UIName = label; float UIMin = lo; float UIMax = hi; > = { def };

UI_FLOAT(Exposure, "Exposure", 0.0, 4.0, 1.25)
float3 Tint < string UIName = "Tint"; string UIWidget = "Color"; > = float3(1.0, 0.5, 0.25);
int Mode < string UIName = "Mode"; string UIWidget = "Dropdown"; string UIList = "A, B, C"; int UIMin = 0; int UIMax = 2; > = 2;
bool Enabled < string UIName = "Enabled"; > = true;
string Label < string UIName = "Some label"; > = "text";
float2 Pair[3] = { float2(1, 2), float2(3, 4), float2(5, 6) };
row_major float3x2 RowMatrix = { 1, 2, 3, 4, 5, 6 };
float2x3 ColumnMatrix = float2x3(1, 2, 3, 4, 5, 6);
float Half = 1.0 / 2.0;
uint Mask = 0xFF;
static const float Scale = 2.0;
float Scaled = Scale * 3.0;

struct Light
{
	float3 color;
	float intensity;
	float2 direction;
};
Light Sun;
float AfterStruct;

cbuffer Extra : register(b3)
{
	float4 ExtraA;
	float ExtraB;
	float3 ExtraC;
};

Texture2D<float4> TextureColor;
Texture2D TextureFile < string ResourceName = "file.dds"; >;
texture LegacyTexture;

SamplerState PointSampler
{
	Filter = MIN_MAG_MIP_POINT;
	AddressU = Wrap;
	AddressV = Mirror_Once;
	BorderColor = float4(0, 0, 0, 1);
	MaxLOD = FLT_MAX;
};

SamplerComparisonState ShadowSampler
{
	Filter = COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	ComparisonFunc = LESS_EQUAL;
};

sampler2D LegacySampler = sampler_state
{
	Texture = <LegacyTexture>;
	MinFilter = Linear;
};

BlendState Additive
{
	BlendEnable[0] = TRUE;
	SrcBlend = ONE;
	DestBlend = ONE;
	BlendOp = ADD;
	RenderTargetWriteMask[0] = 0x0F;
};

DepthStencilState NoDepth
{
	DepthEnable = FALSE;
	DepthWriteMask = ZERO;
};

RasterizerState NoCull
{
	CullMode = NONE;
};

struct VS_INPUT
{
	float3 pos : POSITION;
	float2 uv : TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
	nointerpolation float2 extra : TEXCOORD1;
};

class Helper
{
	float value;
	static float Twice(float x) { return x * 2.0; }
	float Get() { return value; }
};

[maxvertexcount(3)]
void GS_Pass(triangle VS_OUTPUT input[3], inout TriangleStream<VS_OUTPUT> stream)
{
	for (int i = 0; i < 3; ++i)
		stream.Append(input[i]);
}

VS_OUTPUT VS_Quad(VS_INPUT IN, uniform float offset)
{
	VS_OUTPUT OUT;
	OUT.pos = float4(IN.pos.xy + offset, IN.pos.z, 1.0);
	OUT.uv = IN.uv;
	OUT.extra = IN.uv * Pair[1];
	return OUT;
}

float4 PS_Draw(VS_OUTPUT IN, float4 v0 : SV_Position0, uniform Texture2D source, uniform float gain) : SV_Target
{
	return source.Sample(PointSampler, IN.uv) * gain * Exposure + float4(Tint, 0) + ExtraA * Sun.intensity + mul(RowMatrix, float2(Half, AfterStruct)).xyzz +
	       mul(ColumnMatrix, float3(Scaled, 0, 0)).xyxy + Mode + (Enabled ? 1 : 0) + Mask;
}

float4 PS_Draw(VS_OUTPUT IN) : SV_Target
{
	return TextureColor.Sample(PointSampler, IN.uv) * Exposure + float4(Tint, 0) + ExtraA * Sun.intensity + mul(RowMatrix, float2(Half, AfterStruct)).xyzz +
	       mul(ColumnMatrix, float3(Scaled, 0, 0)).xyxy + Mode + (Enabled ? 1 : 0) + Mask + Pair[2].xyxy;
}

VertexShader QuadShader = CompileShader(vs_5_0, VS_Quad(0.0));

technique11 Main < string UIName = "Main"; string RenderTarget = "RenderTargetRGBA64F"; >
{
	pass Draw
	{
		SetVertexShader(QuadShader);
		SetGeometryShader(NULL);
		SetPixelShader(CompileShader(ps_5_0, PS_Draw(TextureFile, 2.0)));
		SetBlendState(Additive, float4(0, 0, 0, 0), 0xFFFFFFFF);
		SetDepthStencilState(NoDepth, 0);
		SetRasterizerState(NoCull);
	}
	pass WithGeometry
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad(0.5)));
		SetGeometryShader(CompileShader(gs_5_0, GS_Pass()));
		SetPixelShader(CompileShader(ps_5_0, PS_Draw()));
	}
}

technique11 Main1
{
	pass
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad(0.0)));
		SetPixelShader(CompileShader(ps_5_0, PS_Draw()));
	}
}

technique10 Legacy
{
	pass P0
	{
		VertexShader = compile vs_4_0 VS_Quad(0.0);
		PixelShader = compile ps_4_0 PS_Draw();
		ZEnable = false;
	}
}

fxgroup Grouped < string UIName = "Grouped"; >
{
	technique11 First
	{
		pass P0
		{
			SetVertexShader(QuadShader);
			SetPixelShader(CompileShader(ps_5_0, PS_Draw()));
		}
	}
	technique11 Second
	{
		pass P0
		{
			SetVertexShader(QuadShader);
			SetPixelShader(CompileShader(ps_5_0, PS_Draw(TextureColor, 0.5)));
		}
	}
}
