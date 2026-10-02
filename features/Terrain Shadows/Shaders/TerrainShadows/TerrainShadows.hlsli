namespace TerrainShadows
{
	Texture2D<float2> ShadowHeightTexture : register(t60);

	float2 GetTerrainShadowUV(float2 xy)
	{
		return xy * SharedData::terraOccSettings.Scale.xy + SharedData::terraOccSettings.Offset.xy;
	}

	float GetTerrainZ(float norm_z)
	{
		return lerp(SharedData::terraOccSettings.ZRange.x, SharedData::terraOccSettings.ZRange.y, norm_z) - 256;
	}

	float2 GetTerrainZ(float2 norm_z)
	{
		return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
	}

	float GetTerrainShadow(const float3 worldPos, SamplerState samp)
	{
		if (!SharedData::terraOccSettings.EnableTerrainShadow)
			return 1.0;
		// The penumbra's [upper, lower] heights: lit above the upper, shadowed below the lower. Where it has collapsed (the two
		// equal, as wherever nothing occludes the texel), the division would be by a difference whose sign is rounding, and
		// compilers round it differently (fused or separate multiply-adds: DXC's pipelines read -inf where FXC's read +inf,
		// and every surface lost the sun); there the shadow is the step at that height.
		float2 shadowHeight = GetTerrainZ(ShadowHeightTexture.SampleLevel(samp, GetTerrainShadowUV(worldPos.xy), 0));
		const float penumbra = shadowHeight.x - shadowHeight.y;
		if (penumbra <= 1e-3)
			return worldPos.z >= shadowHeight.y ? 1.0 : 0.0;
		return saturate((worldPos.z - shadowHeight.y) / penumbra);
	}
}
