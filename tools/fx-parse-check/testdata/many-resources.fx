// More textures and samplers than one push block holds (34 + 2). Each pass gets the ones its shaders reach:
// pass p0 reads TexA0-TexA19 through a helper function, p1 reads TexB0-TexB13 through a class's static member.

Texture2D TexA0;
Texture2D TexA1;
Texture2D TexA2;
Texture2D TexA3;
Texture2D TexA4;
Texture2D TexA5;
Texture2D TexA6;
Texture2D TexA7;
Texture2D TexA8;
Texture2D TexA9;
Texture2D TexA10;
Texture2D TexA11;
Texture2D TexA12;
Texture2D TexA13;
Texture2D TexA14;
Texture2D TexA15;
Texture2D TexA16;
Texture2D TexA17;
Texture2D TexA18;
Texture2D TexA19;
Texture2D TexB0;
Texture2D TexB1;
Texture2D TexB2;
Texture2D TexB3;
Texture2D TexB4;
Texture2D TexB5;
Texture2D TexB6;
Texture2D TexB7;
Texture2D TexB8;
Texture2D TexB9;
Texture2D TexB10;
Texture2D TexB11;
Texture2D TexB12;
Texture2D TexB13;
Texture2D TextureColor;
SamplerState Linear { Filter = MIN_MAG_MIP_LINEAR; };
SamplerState Unused { Filter = MIN_MAG_MIP_POINT; };

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

float4 SumA(float2 uv)
{
	float4 c = 0;
	c += TexA0.Sample(Linear, uv);
	c += TexA1.Sample(Linear, uv);
	c += TexA2.Sample(Linear, uv);
	c += TexA3.Sample(Linear, uv);
	c += TexA4.Sample(Linear, uv);
	c += TexA5.Sample(Linear, uv);
	c += TexA6.Sample(Linear, uv);
	c += TexA7.Sample(Linear, uv);
	c += TexA8.Sample(Linear, uv);
	c += TexA9.Sample(Linear, uv);
	c += TexA10.Sample(Linear, uv);
	c += TexA11.Sample(Linear, uv);
	c += TexA12.Sample(Linear, uv);
	c += TexA13.Sample(Linear, uv);
	c += TexA14.Sample(Linear, uv);
	c += TexA15.Sample(Linear, uv);
	c += TexA16.Sample(Linear, uv);
	c += TexA17.Sample(Linear, uv);
	c += TexA18.Sample(Linear, uv);
	c += TexA19.Sample(Linear, uv);
	return c;
}

class Bank
{
	static float4 Sum(float2 uv)
	{
		float4 c = 0;
		c += TexB0.SampleLevel(Linear, uv, 0);
		c += TexB1.SampleLevel(Linear, uv, 0);
		c += TexB2.SampleLevel(Linear, uv, 0);
		c += TexB3.SampleLevel(Linear, uv, 0);
		c += TexB4.SampleLevel(Linear, uv, 0);
		c += TexB5.SampleLevel(Linear, uv, 0);
		c += TexB6.SampleLevel(Linear, uv, 0);
		c += TexB7.SampleLevel(Linear, uv, 0);
		c += TexB8.SampleLevel(Linear, uv, 0);
		c += TexB9.SampleLevel(Linear, uv, 0);
		c += TexB10.SampleLevel(Linear, uv, 0);
		c += TexB11.SampleLevel(Linear, uv, 0);
		c += TexB12.SampleLevel(Linear, uv, 0);
		c += TexB13.SampleLevel(Linear, uv, 0);
		return c;
	}
};

float4 PS_A(VS_OUTPUT i) : SV_Target { return SumA(i.uv) + TextureColor.Sample(Linear, i.uv); }
float4 PS_B(VS_OUTPUT i) : SV_Target { return Bank::Sum(i.uv); }

technique11 Many
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS()));
		SetPixelShader(CompileShader(ps_5_0, PS_A()));
	}
	pass p1
	{
		SetVertexShader(CompileShader(vs_5_0, VS()));
		SetPixelShader(CompileShader(ps_5_0, PS_B()));
	}
}
