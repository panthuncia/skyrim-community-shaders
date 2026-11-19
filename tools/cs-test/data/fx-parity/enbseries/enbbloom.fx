// FX parity test: more textures than one push block holds (34 here). The pass reaches two of them.
#include "parity_common.fxh"

Texture2D TextureColor;
Texture2D TextureDownsampled;
Texture2D ParityUnused0;
Texture2D ParityUnused1;
Texture2D ParityUnused2;
Texture2D ParityUnused3;
Texture2D ParityUnused4;
Texture2D ParityUnused5;
Texture2D ParityUnused6;
Texture2D ParityUnused7;
Texture2D ParityUnused8;
Texture2D ParityUnused9;
Texture2D ParityUnused10;
Texture2D ParityUnused11;
Texture2D ParityUnused12;
Texture2D ParityUnused13;
Texture2D ParityUnused14;
Texture2D ParityUnused15;
Texture2D ParityUnused16;
Texture2D ParityUnused17;
Texture2D ParityUnused18;
Texture2D ParityUnused19;
Texture2D ParityUnused20;
Texture2D ParityUnused21;
Texture2D ParityUnused22;
Texture2D ParityUnused23;
Texture2D ParityUnused24;
Texture2D ParityUnused25;
Texture2D ParityUnused26;
Texture2D ParityUnused27;
Texture2D ParityUnused28;
Texture2D ParityUnused29;
Texture2D ParityUnused30;
Texture2D ParityUnused31;

float4 PS_Bloom(VS_OUTPUT_POST IN) : SV_Target
{
	const float2 px = ScreenSize.y * float2(1.0, ScreenSize.z) * 4.0;
	float3 c = 0;
	for (int y = -2; y <= 2; ++y)
		for (int x = -2; x <= 2; ++x)
			c += TextureColor.Sample(Smp, IN.txcoord + float2(x, y) * px).rgb;
	return float4(max(c / 25.0 - 0.8, 0.0) + ParityUnused0.Sample(Smp, IN.txcoord).rgb, 1.0);
}

technique11 Bloom < string UIName = "Parity bloom"; >
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad()));
		SetPixelShader(CompileShader(ps_5_0, PS_Bloom()));
	}
}
