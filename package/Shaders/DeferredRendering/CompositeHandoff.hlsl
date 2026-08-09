Texture2D<float4> Source : register(t0);
#if defined(COVERAGE_OVERLAY) || defined(SELECTIVE_HANDOFF)
Texture2D<uint> PackedSurface : register(t1);
#endif

struct FullscreenVertex
{
	float4 position : SV_Position;
};

#if defined(VSHADER)
FullscreenVertex main(uint vertexID : SV_VertexID)
{
	FullscreenVertex result;
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	result.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return result;
}
#else
float4 main(FullscreenVertex input) : SV_Target0
{
#if defined(COVERAGE_OVERLAY)
	const uint materialClass = (PackedSurface.Load(int3(uint2(input.position.xy), 0)) >> 16) & 0xFFu;
	if (materialClass != 1u)
		discard;
	return float4(0, 1, 0, 1);
#elif defined(SELECTIVE_HANDOFF)
	const uint materialClass = (PackedSurface.Load(int3(uint2(input.position.xy), 0)) >> 16) & 0xFFu;
	if (materialClass != 1u)
		discard;
	return Source.Load(int3(uint2(input.position.xy), 0));
#endif
	return Source.Load(int3(uint2(input.position.xy), 0));
}
#endif
