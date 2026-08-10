// Initializes every persistent deferred output from the current D3D11 frame.
// Native evaluator passes subsequently overwrite only the pixels they promote.
cbuffer SeedConstants : register(b0)
{
	uint2 renderSize;
	uint debugView;
	uint compatibilityIndex;
	uint specularIndex;
	uint reflectanceIndex;
	uint albedoIndex;
	uint normalIndex;
	uint masksIndex;
	uint packedSurfaceIndex;
	uint linearDepthIndex;
	uint compositeOutputIndex;
	uint specularOutputIndex;
	uint reflectanceOutputIndex;
	uint albedoOutputIndex;
	uint normalOutputIndex;
	uint masksOutputIndex;
};

float3 DecodeNormal(float2 encoded)
{
	float2 f = encoded * 2.0f - 1.0f;
	float3 normal = float3(f, 1.0f - abs(f.x) - abs(f.y));
	float t = saturate(-normal.z);
	normal.xy += float2(normal.x >= 0.0f ? -t : t,
		normal.y >= 0.0f ? -t : t);
	return -normalize(normal);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThread : SV_DispatchThreadID)
{
	if (any(dispatchThread.xy >= renderSize))
		return;

	Texture2D<float4> compatibility = ResourceDescriptorHeap[compatibilityIndex];
	Texture2D<float4> specular = ResourceDescriptorHeap[specularIndex];
	Texture2D<float4> reflectance = ResourceDescriptorHeap[reflectanceIndex];
	Texture2D<float4> albedo = ResourceDescriptorHeap[albedoIndex];
	Texture2D<float4> normal = ResourceDescriptorHeap[normalIndex];
	Texture2D<float4> masks = ResourceDescriptorHeap[masksIndex];
	Texture2D<uint4> packedSurface = ResourceDescriptorHeap[packedSurfaceIndex];
	Texture2D<float> linearDepth = ResourceDescriptorHeap[linearDepthIndex];
	RWTexture2D<float4> compositeOutput = ResourceDescriptorHeap[compositeOutputIndex];
	RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[specularOutputIndex];
	RWTexture2D<float4> reflectanceOutput = ResourceDescriptorHeap[reflectanceOutputIndex];
	RWTexture2D<float4> albedoOutput = ResourceDescriptorHeap[albedoOutputIndex];
	RWTexture2D<float4> normalOutput = ResourceDescriptorHeap[normalOutputIndex];
	RWTexture2D<float4> masksOutput = ResourceDescriptorHeap[masksOutputIndex];

	const int3 sourcePixel = int3(dispatchThread.xy, 0);
	float4 displayed = compatibility.Load(sourcePixel);
	if (debugView == 2u)
		displayed = albedo.Load(sourcePixel);
	else if (debugView == 3u)
		displayed = specular.Load(sourcePixel);
	else if (debugView == 4u)
		displayed = reflectance.Load(sourcePixel);
	else if (debugView == 5u)
		displayed = float4(DecodeNormal(normal.Load(sourcePixel).xy) * 0.5f + 0.5f, 1.0f);
	else if (debugView == 6u)
		displayed = masks.Load(sourcePixel);
	else if (debugView == 7u) {
		uint material = (packedSurface.Load(sourcePixel).x >> 16u) & 0xffu;
		displayed = float4(frac(float3(material, material, material) *
			float3(0.6180339f, 0.3819660f, 0.7548777f)), 1.0f);
	} else if (debugView == 8u) {
		float depth = linearDepth.Load(sourcePixel);
		float normalizedDepth = saturate(log2(1.0f + max(depth, 0.0f)) / 16.0f);
		displayed = normalizedDepth.xxxx;
	} else if (debugView >= 9u)
		displayed = 0.0f;
	compositeOutput[dispatchThread.xy] = displayed;
	specularOutput[dispatchThread.xy] = specular.Load(sourcePixel);
	reflectanceOutput[dispatchThread.xy] = reflectance.Load(sourcePixel);
	albedoOutput[dispatchThread.xy] = albedo.Load(sourcePixel);
	normalOutput[dispatchThread.xy] = normal.Load(sourcePixel);
	masksOutput[dispatchThread.xy] = masks.Load(sourcePixel);
}
