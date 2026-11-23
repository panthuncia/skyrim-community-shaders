// The handle tables of the shared volume's programs on the render graph (view-atmosphere-port.md, 3.6): one uint per
// resource the program reads, in this order. HLSL (LightScattering.cs.hlsl, Integrate.cs.hlsl) and C++
// (ORGViewAtmosphereVolume) both include this file, so keep it to these two macros. Names are the shaders' declared
// names. MediumLevel is a value, not a handle: the grid the dispatch works on (D3D11's b0).
#define VIEW_ATMOSPHERE_INJECT_TABLE(X) \
	X(MediumLevel)                      \
	X(LinearSampler)                    \
	X(ShadowSampler)                    \
	X(MediumHistory)                    \
	X(DirectionalShadowMap)             \
	X(MediumMsLut)                      \
	X(MediumSource)                     \
	X(DirectionalShadowLights)          \
	X(ViewAtmosphereData)               \
	X(TexTrLut)                         \
	X(TexDiskPhaseLut)                  \
	X(TexShadowVolume)                  \
	X(MeanProfile)                      \
	X(TexCloudDiffuseTransport)         \
	X(ShadowHeightTexture)              \
	X(CloudShadowsTexture)              \
	X(EnvIBLTexture)                    \
	X(SkyIBLTexture)                    \
	X(MediumEnvironment)                \
	X(SkylightingProbeArray)            \
	X(lights)                           \
	X(lightList)                        \
	X(lightGrid)                        \
	X(LocalShadows)                     \
	X(LocalShadowMaps)                  \
	X(EngineLocalShadowMaps)

#define VIEW_ATMOSPHERE_INTEGRATE_TABLE(X) \
	X(MediumLevel)                         \
	X(MediumSource)                        \
	X(IntegratedLuminance)                 \
	X(IntegratedTransmittance)             \
	X(ViewAtmosphereData)                  \
	X(SkylightingProbeArray)
