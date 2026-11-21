#ifndef PHYSICAL_LENS_EXPOSURE
#define PHYSICAL_LENS_EXPOSURE
#include "PostProcessing/PhysicalLensFlare/optics.hlsli"
PHYSICAL_LENS_EXPOSURE_RESOURCES(ORG_RESOURCE)
// Dual-target: Auto Exposure's constants, a latch region on ORG (HistogramAutoExposure::AutoExposureCB).
#define PHYSICAL_LENS_EXPOSURE_FIELDS(M, A) 	M(float2, ExposureArea)                 	M(float2, ExposureRange)                	M(float, ExposureLerp)                  	M(float, ExposureCompensation)          	M(float, UnusedStart)                   	M(float, UnusedMax)                     	M(float, UnusedStrength)                	M(float, ExposureTarget)                	M(float2, ExposurePadding)
ORG_GLOBAL_LATCH_BEGIN(ExposureConstants, ExposureParameters, 1)
PHYSICAL_LENS_EXPOSURE_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(ExposureConstants, Exposure, 1)
#define ORG_GLOBAL_LATCH Exposure
PHYSICAL_LENS_EXPOSURE_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH
float VisibilityExposure()
{
	float exposure = ManualExposure;
	if (AutoExposureEnabled > 0)
		exposure = ExposureTarget * ExposureCompensation / clamp(ExposureAdaptation[0], ExposureRange.x, ExposureRange.y);
	return exposure * ExposureDecode;
}

#endif
