// Matrix packing and cbuffer blocks: fx-parse-check compares the $Globals layout with FXC's reflection.
// #pragma pack_matrix(row_major) applies to the declarations after it; column_major switches back.

float4x4 BeforePragma;
#pragma pack_matrix(row_major)
float3x4 RowMajorByPragma;
float2 Gap;
column_major float4x3 ExplicitColumn;
#pragma pack_matrix(column_major)
float3x2 ColumnAgain;
row_major float2x3 ExplicitRow;
float4 ScreenSize;

cbuffer Extra
{
	float4 ExtraTint = { 1.0, 0.5, 0.25, 1.0 };
	float ExtraScale = 2.0;
};

Texture2D TextureColor;
SamplerState Point { Filter = MIN_MAG_MIP_POINT; };

struct VS_OUTPUT
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VS_OUTPUT VS(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
	VS_OUTPUT o;
	o.pos = float4(pos, 1.0);
	o.uv = uv;
	return o;
}

float4 PS(VS_OUTPUT i) : SV_Target
{
	float4 c = TextureColor.Sample(Point, i.uv);
	float4 m = mul(BeforePragma, c) + float4(mul(RowMajorByPragma, c), 0.0) + float4(mul(c, ExplicitColumn), 0.0);
	m.xy += mul(ColumnAgain, Gap) + mul(ExplicitRow, m.xyz);
	return m * ExtraTint * ExtraScale + ScreenSize;
}

technique11 Packing
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS()));
		SetPixelShader(CompileShader(ps_5_0, PS()));
	}
}
