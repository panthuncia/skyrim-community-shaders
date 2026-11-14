// Shared by the FX parity test preset (docs/development/parity/fx.md). Effects11's quad: POSITION and TEXCOORD0.
struct VS_OUTPUT_POST
{
	float4 pos : SV_POSITION;
	float2 txcoord : TEXCOORD0;
};

VS_OUTPUT_POST VS_Quad(float3 pos : POSITION, float2 txcoord : TEXCOORD0)
{
	VS_OUTPUT_POST o;
	o.pos = float4(pos, 1.0);
	o.txcoord = txcoord;
	return o;
}

SamplerState Smp
{
	Filter = MIN_MAG_MIP_LINEAR;
	AddressU = Clamp;
	AddressV = Clamp;
};

float4 ScreenSize;
