#ifndef PHYSICAL_LENS_DIFFRACTION_PARAMETERS
#define PHYSICAL_LENS_DIFFRACTION_PARAMETERS
#include "Common/OrgResources.hlsli"
// The register number. Dual-target: on ORG the block is the Diffraction latch, which the includer's resource list
// names as an EXTERN_LATCH entry (DiffractionConstants, Diffraction).
#ifndef WAVE_PARAMETER_REGISTER
#	define WAVE_PARAMETER_REGISTER 2
#endif
#define DIFFRACTION_FIELDS(M, A) \
	A(float4, WaveSampling, 3)   \
	M(uint2, WaveImageSize)      \
	M(float, DiffractionEnabled) \
	M(uint, WavePupilSize)       \
	M(float4, WaveSurface)       \
	M(float4, WaveStarburst)     \
	M(float4, WavePupilDirt)
// DiffractionEnabled 0: inactive, 1: identity image mapping, 2: lens image mapping.
// One more level, so the register macro expands before ORG_GLOBAL_LATCH_* paste it.
#define PHYSICAL_LENS_DIFFRACTION_BEGIN(R) ORG_GLOBAL_LATCH_BEGIN(DiffractionConstants, DiffractionParameters, R)
#define PHYSICAL_LENS_DIFFRACTION_END(R) ORG_GLOBAL_LATCH_END(DiffractionConstants, Diffraction, R)
PHYSICAL_LENS_DIFFRACTION_BEGIN(WAVE_PARAMETER_REGISTER)
DIFFRACTION_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
PHYSICAL_LENS_DIFFRACTION_END(WAVE_PARAMETER_REGISTER)
#define ORG_GLOBAL_LATCH Diffraction
DIFFRACTION_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH
#endif
