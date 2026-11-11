// ISFXAA: the engine's FXAA pass (BSImagespaceShaderFXAA). The vertex shader also passes the coordinates
// one texel (GeometryOffset.zw) up-left and down-right, which this pixel shader does not read.
//
// Transcribed mechanically from the vanilla DXBC (Skyrim - Shaders.bsa), so its arithmetic, registers and
// constant layout are vanilla's. Temporaries hold raw bits (uint4), as DXBC registers do; integer work goes
// through asint/asuint and float work through asfloat. Checked against the vanilla shader on random inputs.

#if defined(VSHADER)
// The engine fills these by name (the constructor's list) at vanilla's registers.
cbuffer PerGeometry : register(b2)
{
	float4 GeometryOffset : packoffset(c0);
};

void main(float4 v0 : POSITION0, float2 v1 : TEXCOORD0, out float4 o0 : SV_POSITION0, out float2 o1 : TEXCOORD0, out float4 o2 : TEXCOORD1)
{
	o0 = 0;
	o1 = 0;
	o2 = 0;
	o0.xy = -GeometryOffset.xy * float2(2.0, -2.0) + v0.xy;
	o0.z = v0.z;
	o0.w = 1.0;
	o1.xy = v1.xy;
	o2.xy = v1.xy + -GeometryOffset.zw;
	o2.zw = v1.xy + GeometryOffset.zw;
}
#endif

#if defined(PSHADER)
SamplerState Src0Sampler : register(s0);
Texture2D<float4> Src0Tex : register(t0);

// The engine fills these by name (the constructor's list) at vanilla's registers.
cbuffer PerGeometry : register(b2)
{
	float4 Offsets0 : packoffset(c0);
	float4 Offsets1 : packoffset(c1);
	float4 Offsets2 : packoffset(c2);
	float4 Offsets3 : packoffset(c3);
};

void main(float4 v0 : SV_POSITION0, float2 v1 : TEXCOORD0, float4 v2 : TEXCOORD1, out float4 o0 : SV_Target0)
{
	uint4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
	o0 = 0;
	{
		const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(v1.x, v1.y), 0.0);
		r0.xyzw = asuint(fetched.xyzw);
	}
	{
		const float4 fetched = Src0Tex.GatherGreen(Src0Sampler, float2(v1.x, v1.y));
		r1.xyz = asuint(fetched.xyz);
	}
	{
		const float4 fetched = Src0Tex.GatherGreen(Src0Sampler, float2(v1.x, v1.y), int2(-1, -1));
		r2.xyz = asuint(fetched.xzw);
	}
	r1.w = asuint(max(asfloat(r0.y), asfloat(r1.x)));
	r2.w = asuint(min(asfloat(r0.y), asfloat(r1.x)));
	r1.w = asuint(max(asfloat(r1.w), asfloat(r1.z)));
	r2.w = asuint(min(asfloat(r1.z), asfloat(r2.w)));
	r3.x = asuint(max(asfloat(r2.x), asfloat(r2.y)));
	r3.y = asuint(min(asfloat(r2.x), asfloat(r2.y)));
	r1.w = asuint(max(asfloat(r1.w), asfloat(r3.x)));
	r2.w = asuint(min(asfloat(r2.w), asfloat(r3.y)));
	r3.x = asuint(asfloat(r1.w) * 0.166);
	r1.w = asuint(asfloat(r1.w) + -asfloat(r2.w));
	r2.w = asuint(max(asfloat(r3.x), 0.0833));
	r2.w = asuint(-(int)((asfloat(r1.w)) >= (asfloat(r2.w))));
	if (r2.w != 0u) {
		{
			const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(v1.x, v1.y), 0.0, int2(1, -1));
			r2.w = asuint(fetched.y);
		}
		{
			const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(v1.x, v1.y), 0.0, int2(-1, 1));
			r3.x = asuint(fetched.y);
		}
		r3.yz = asuint(asfloat(r1.xz) + asfloat(r2.yx));
		r1.w = asuint(1.0 / asfloat(r1.w));
		r3.w = asuint(asfloat(r3.z) + asfloat(r3.y));
		r3.yz = asuint(asfloat(r0.yy) * float2(-2.0, -2.0) + asfloat(r3.yz));
		r4.x = asuint(asfloat(r1.y) + asfloat(r2.w));
		r2.w = asuint(asfloat(r2.w) + asfloat(r2.z));
		r4.y = asuint(asfloat(r1.z) * -2.0 + asfloat(r4.x));
		r2.w = asuint(asfloat(r2.y) * -2.0 + asfloat(r2.w));
		r2.z = asuint(asfloat(r2.z) + asfloat(r3.x));
		r1.y = asuint(asfloat(r1.y) + asfloat(r3.x));
		r3.x = asuint(abs(asfloat(r3.y)) * 2.0 + abs(asfloat(r4.y)));
		r2.w = asuint(abs(asfloat(r3.z)) * 2.0 + abs(asfloat(r2.w)));
		r3.y = asuint(asfloat(r2.x) * -2.0 + asfloat(r2.z));
		r1.y = asuint(asfloat(r1.x) * -2.0 + asfloat(r1.y));
		r3.x = asuint(asfloat(r3.x) + abs(asfloat(r3.y)));
		r1.y = asuint(asfloat(r2.w) + abs(asfloat(r1.y)));
		r2.z = asuint(asfloat(r4.x) + asfloat(r2.z));
		r1.y = asuint(-(int)((asfloat(r3.x)) >= (asfloat(r1.y))));
		r2.z = asuint(asfloat(r3.w) * 2.0 + asfloat(r2.z));
		r2.x = (r1.y != 0u ? r2.y : r2.x);
		r1.x = (r1.y != 0u ? r1.x : r1.z);
		r1.z = (r1.y != 0u ? asuint(Offsets0.y) : asuint(Offsets0.x));
		r2.y = asuint(asfloat(r2.z) * 0.083333336 + -asfloat(r0.y));
		r2.z = asuint(-asfloat(r0.y) + asfloat(r2.x));
		r2.w = asuint(-asfloat(r0.y) + asfloat(r1.x));
		r2.x = asuint(asfloat(r0.y) + asfloat(r2.x));
		r1.x = asuint(asfloat(r0.y) + asfloat(r1.x));
		r3.x = asuint(-(int)((abs(asfloat(r2.z))) >= (abs(asfloat(r2.w)))));
		r2.z = asuint(max(abs(asfloat(r2.w)), abs(asfloat(r2.z))));
		r1.z = asuint((r3.x != 0u ? -asfloat(r1.z) : asfloat(r1.z)));
		r1.w = asuint(saturate(asfloat(r1.w) * abs(asfloat(r2.y))));
		r2.y = r1.y & asuint(Offsets0.x);
		r2.w = (r1.y != 0u ? 0x00000000u : asuint(Offsets0.y));
		r3.yz = asuint(asfloat(r1.zz) * float2(0.5, 0.5) + v1.xy);
		r3.y = (r1.y != 0u ? asuint(v1.x) : r3.y);
		r3.z = (r1.y != 0u ? r3.z : asuint(v1.y));
		r4.xy = asuint(-asfloat(r2.yw) + asfloat(r3.yz));
		r5.xy = asuint(asfloat(r2.yw) + asfloat(r3.yz));
		r3.y = asuint(asfloat(r1.w) * -2.0 + 3.0);
		{
			const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r4.x), asfloat(r4.y)), 0.0);
			r3.z = asuint(fetched.y);
		}
		r1.w = asuint(asfloat(r1.w) * asfloat(r1.w));
		{
			const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r5.x), asfloat(r5.y)), 0.0);
			r3.w = asuint(fetched.y);
		}
		r1.x = (r3.x != 0u ? r2.x : r1.x);
		r2.x = asuint(asfloat(r2.z) * 0.25);
		r2.z = asuint(-asfloat(r1.x) * 0.5 + asfloat(r0.y));
		r1.w = asuint(asfloat(r1.w) * asfloat(r3.y));
		r2.z = asuint(-(int)((asfloat(r2.z)) < (0.0)));
		r3.x = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.z));
		r3.y = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.w));
		r3.zw = asuint(-(int2)((abs(asfloat(r3.xy))) >= (asfloat(r2.xx))));
		r4.z = asuint(-asfloat(r2.y) * 1.5 + asfloat(r4.x));
		r4.x = (r3.z != 0u ? r4.x : r4.z);
		r4.w = asuint(-asfloat(r2.w) * 1.5 + asfloat(r4.y));
		r4.z = (r3.z != 0u ? r4.y : r4.w);
		r4.yw = ~r3.zw;
		r4.y = r4.w | r4.y;
		r4.w = asuint(asfloat(r2.y) * 1.5 + asfloat(r5.x));
		r5.x = (r3.w != 0u ? r5.x : r4.w);
		r4.w = asuint(asfloat(r2.w) * 1.5 + asfloat(r5.y));
		r5.z = (r3.w != 0u ? r5.y : r4.w);
		if (r4.y != 0u) {
			if (r3.z == 0u) {
				{
					const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r4.x), asfloat(r4.z)), 0.0);
					r3.x = asuint(fetched.y);
				}
			}
			if (r3.w == 0u) {
				{
					const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r5.x), asfloat(r5.z)), 0.0);
					r3.y = asuint(fetched.y);
				}
			}
			r4.y = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.x));
			r3.x = (r3.z != 0u ? r3.x : r4.y);
			r3.z = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.y));
			r3.y = (r3.w != 0u ? r3.y : r3.z);
			r3.zw = asuint(-(int2)((abs(asfloat(r3.xy))) >= (asfloat(r2.xx))));
			r4.y = asuint(-asfloat(r2.y) * 2.0 + asfloat(r4.x));
			r4.x = (r3.z != 0u ? r4.x : r4.y);
			r4.y = asuint(-asfloat(r2.w) * 2.0 + asfloat(r4.z));
			r4.z = (r3.z != 0u ? r4.z : r4.y);
			r4.yw = ~r3.zw;
			r4.y = r4.w | r4.y;
			r4.w = asuint(asfloat(r2.y) * 2.0 + asfloat(r5.x));
			r5.x = (r3.w != 0u ? r5.x : r4.w);
			r4.w = asuint(asfloat(r2.w) * 2.0 + asfloat(r5.z));
			r5.z = (r3.w != 0u ? r5.z : r4.w);
			if (r4.y != 0u) {
				if (r3.z == 0u) {
					{
						const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r4.x), asfloat(r4.z)), 0.0);
						r3.x = asuint(fetched.y);
					}
				}
				if (r3.w == 0u) {
					{
						const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r5.x), asfloat(r5.z)), 0.0);
						r3.y = asuint(fetched.y);
					}
				}
				r4.y = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.x));
				r3.x = (r3.z != 0u ? r3.x : r4.y);
				r3.z = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.y));
				r3.y = (r3.w != 0u ? r3.y : r3.z);
				r3.zw = asuint(-(int2)((abs(asfloat(r3.xy))) >= (asfloat(r2.xx))));
				r4.y = asuint(-asfloat(r2.y) * 4.0 + asfloat(r4.x));
				r4.x = (r3.z != 0u ? r4.x : r4.y);
				r4.y = asuint(-asfloat(r2.w) * 4.0 + asfloat(r4.z));
				r4.z = (r3.z != 0u ? r4.z : r4.y);
				r4.yw = ~r3.zw;
				r4.y = r4.w | r4.y;
				r4.w = asuint(asfloat(r2.y) * 4.0 + asfloat(r5.x));
				r5.x = (r3.w != 0u ? r5.x : r4.w);
				r4.w = asuint(asfloat(r2.w) * 4.0 + asfloat(r5.z));
				r5.z = (r3.w != 0u ? r5.z : r4.w);
				if (r4.y != 0u) {
					if (r3.z == 0u) {
						{
							const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r4.x), asfloat(r4.z)), 0.0);
							r3.x = asuint(fetched.y);
						}
					}
					if (r3.w == 0u) {
						{
							const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r5.x), asfloat(r5.z)), 0.0);
							r3.y = asuint(fetched.y);
						}
					}
					r4.y = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.x));
					r3.x = (r3.z != 0u ? r3.x : r4.y);
					r1.x = asuint(-asfloat(r1.x) * 0.5 + asfloat(r3.y));
					r3.y = (r3.w != 0u ? r3.y : r1.x);
					r3.zw = asuint(-(int2)((abs(asfloat(r3.xy))) >= (asfloat(r2.xx))));
					r1.x = asuint(-asfloat(r2.y) * 12.0 + asfloat(r4.x));
					r4.x = (r3.z != 0u ? r4.x : r1.x);
					r1.x = asuint(-asfloat(r2.w) * 12.0 + asfloat(r4.z));
					r4.z = (r3.z != 0u ? r4.z : r1.x);
					r1.x = asuint(asfloat(r2.y) * 12.0 + asfloat(r5.x));
					r5.x = (r3.w != 0u ? r5.x : r1.x);
					r1.x = asuint(asfloat(r2.w) * 12.0 + asfloat(r5.z));
					r5.z = (r3.w != 0u ? r5.z : r1.x);
				}
			}
		}
		r1.x = asuint(-asfloat(r4.x) + v1.x);
		r2.y = asuint(-asfloat(r4.z) + v1.y);
		r1.x = (r1.y != 0u ? r1.x : r2.y);
		r2.xy = asuint(asfloat(r5.xz) + -v1.xy);
		r2.x = (r1.y != 0u ? r2.x : r2.y);
		r2.yw = asuint(-(int2)((asfloat(r3.xy)) < (float2(0.0, 0.0))));
		r3.x = asuint(asfloat(r1.x) + asfloat(r2.x));
		r2.yz = asuint(-(int2)((asint(r2.zz)) != (asint(r2.yw))));
		r2.w = asuint(1.0 / asfloat(r3.x));
		r3.x = asuint(-(int)((asfloat(r1.x)) < (asfloat(r2.x))));
		r1.x = asuint(min(asfloat(r1.x), asfloat(r2.x)));
		r2.x = (r3.x != 0u ? r2.y : r2.z);
		r1.w = asuint(asfloat(r1.w) * asfloat(r1.w));
		r1.x = asuint(asfloat(r1.x) * -asfloat(r2.w) + 0.5);
		r1.w = asuint(asfloat(r1.w) * 0.75);
		r1.x = r1.x & r2.x;
		r1.x = asuint(max(asfloat(r1.w), asfloat(r1.x)));
		r1.xz = asuint(asfloat(r1.xx) * asfloat(r1.zz) + v1.xy);
		r2.x = (r1.y != 0u ? asuint(v1.x) : r1.x);
		r2.y = (r1.y != 0u ? r1.z : asuint(v1.y));
		{
			const float4 fetched = Src0Tex.SampleLevel(Src0Sampler, float2(asfloat(r2.x), asfloat(r2.y)), 0.0);
			r1.xyz = asuint(fetched.xyz);
		}
		o0.xyz = asfloat(r1.xyz);
		o0.w = asfloat(r0.y);
	} else {
		o0.xyzw = asfloat(r0.xyzw);
	}
}
#endif
