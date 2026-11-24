#ifndef __ORG_TEXEL_BOUNDS_DEPENDENCY_HLSL__
#define __ORG_TEXEL_BOUNDS_DEPENDENCY_HLSL__

// Texel loads and stores that keep D3D's rules outside a texture in every build. D3D reads 0 there and drops the store.
// Vulkan promises neither, so the ORG build (CS_ORG_BINDLESS) tests the texel first. The D3D11 build is the plain access.
// Shaders transcribed from vanilla DXBC rely on those rules at their edges.
//
//   const float4 fetched = ORG_LOAD_OR_ZERO(InputTex, int3(x, y, mip));
//   ORG_STORE_INSIDE(OutputTex, uint2(x, y), value);

#if defined(CS_ORG_BINDLESS)

/** The texel at a_texel (x, y, mip), or 0 when the mip or the texel is outside the texture. */
float4 OrgLoadOrZero(Texture2D<float4> a_texture, int3 a_texel)
{
	uint width, height, levels;
	a_texture.GetDimensions(0, width, height, levels);
	if (uint(a_texel.z) >= levels)
		return 0.0.xxxx;
	a_texture.GetDimensions(uint(a_texel.z), width, height, levels);
	if (any(uint2(a_texel.xy) >= uint2(width, height)))
		return 0.0.xxxx;
	return a_texture.Load(a_texel);
}

/** Stores a_value at a_texel when it is inside the texture. */
void OrgStoreInside(RWTexture2D<float4> a_texture, uint2 a_texel, float4 a_value)
{
	uint width, height;
	a_texture.GetDimensions(width, height);
	if (all(a_texel < uint2(width, height)))
		a_texture[a_texel] = a_value;
}

#	define ORG_LOAD_OR_ZERO(TEXTURE, TEXEL) OrgLoadOrZero(TEXTURE, TEXEL)
#	define ORG_STORE_INSIDE(TEXTURE, TEXEL, VALUE) OrgStoreInside(TEXTURE, TEXEL, VALUE)
#else
#	define ORG_LOAD_OR_ZERO(TEXTURE, TEXEL) TEXTURE.Load(TEXEL)
#	define ORG_STORE_INSIDE(TEXTURE, TEXEL, VALUE) TEXTURE[TEXEL] = VALUE
#endif

#endif  // __ORG_TEXEL_BOUNDS_DEPENDENCY_HLSL__
