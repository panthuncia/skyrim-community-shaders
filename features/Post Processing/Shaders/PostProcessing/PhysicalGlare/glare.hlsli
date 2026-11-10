#ifndef __PHYSICAL_GLARE_DEPENDENCY_HLSL__
#define __PHYSICAL_GLARE_DEPENDENCY_HLSL__

// Physical Glare — the constants every pass reads
//
// The layout of PhysicalGlare::GlareCB. The shaders are dual-target
// (Common/OrgResources.hlsli): each lists this struct as the LATCH entry
// Glare at b1. D3D11 binds it as a constant buffer; the ORG chain
// (ORG/ORGPhysicalGlare.cpp) loads it from its latch.

#include "Common/OrgResources.hlsli"

struct GlareConstants
{
	// Row 0: core controls
	float Threshold;
	float Intensity;
	float ScatterStrength;
	uint ApertureMode;

	// Row 1: geometry and time
	int ApertureBlades;
	float ApertureRotation;
	float AdaptSpeed;
	float DeltaTime;

	// Row 2: resolution and padding
	uint FFTResolution;
	float PaddingRatio;
	float ScreenWidth;
	float ScreenHeight;

	// Row 3: channel and optics
	uint ChannelIndex;  // Kept for the layout
	float FresnelExponent;
	float ChromaticSpread;
	float ApertureSize;

	// Row 4: PSF shaping and eyelash bending
	float PSFSharpness;
	float PSFNoiseFloor;
	uint EnableEyelashes;
	float EyelashCurvature;

	// Row 5: eye mode, eyelashes and scatter particles
	uint EyelashCount;
	float EyelashLength;
	uint ParticleCount;
	float ParticleSize;

	// Row 6: eye mode, gratings and tear film
	uint GratingCount;
	float GratingStrength;
	float TearFilmStrength;
	float TearFilmSpeed;

	// Row 7: eye mode, tear film and sutures
	uint TearFilmComplexity;
	float TearFilmTime;
	uint SutureBranches;
	float SutureStrength;

	// Row 8: eye mode, suture width and starburst
	float SutureWidth;
	uint StarburstCount;
	float StarburstStrength;
	float StarburstIrregularity;

	// Row 9: lens mode, dust and blade roughness
	uint DustCount;
	float DustSize;
	uint BladeRoughnessFreq;
	float BladeRoughnessAmp;

	// Row 10: lens mode, scratches
	uint ScratchCount;
	float ScratchOpacity;
	float ScratchLength;
	float ScratchWidth;

	// Row 11: more optics
	float SphericalAberration;
	uint UseAP1;
	float KernelScale;
	float _pad11;
};

#endif  // __PHYSICAL_GLARE_DEPENDENCY_HLSL__
