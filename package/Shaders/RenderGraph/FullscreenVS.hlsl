// The fullscreen triangle of the render graph's raster programs (src/RenderGraph/RasterProgram.h): three vertices, no vertex
// buffer or input layout. SV_VertexID makes one triangle that covers the whole viewport. TEXCOORD0 runs from (0,0) at the top left
// to (1,1) at the bottom right, as PostProcessing's fullscreen.hlsli. BasicRHI flips the Vulkan viewport as D3D does, so clip space
// is y-up here too.

struct FullscreenVSOutput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
};

FullscreenVSOutput main(uint vertexID : SV_VertexID)
{
	FullscreenVSOutput output;
	output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(output.TexCoord * 2.0f - 1.0f, 0.0f, 1.0f);
	output.Position.y = -output.Position.y;
	return output;
}
