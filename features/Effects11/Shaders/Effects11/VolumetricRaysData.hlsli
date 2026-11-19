#ifndef EFFECTS11_VOLUMETRIC_RAYS_DATA_HLSLI
#define EFFECTS11_VOLUMETRIC_RAYS_DATA_HLSLI

// The half-res target's dimensions (the dynamic resolution area), which the blur passes and the apply pass's
// upsample read. Mirrors VLData in Effects11.cpp.
struct VLData
{
	int2 ScreenSize;
	int2 ScreenSizeMin1;
};

#endif
