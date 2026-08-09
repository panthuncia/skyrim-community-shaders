// Temporary producer for the deferred graph's linear-depth semantic.
// A later module may replace this producer without changing consumers.
cbuffer SharedData : register(b5)
{
	float4 SharedPrefix[33];
	float4 CameraData;
	float4 BufferDim;
};

Texture2D<float> HardwareDepth : register(t0);
RWTexture2D<float> LinearDepth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
	uint width, height;
	LinearDepth.GetDimensions(width, height);
	if (pixel.x >= width || pixel.y >= height)
		return;

	const float hardwareDepth = HardwareDepth.Load(int3(pixel.xy, 0));
	LinearDepth[pixel.xy] = CameraData.w / (-hardwareDepth * CameraData.z + CameraData.x);
}
