// HistogramAutoExposure::AutoExposureCB, as a struct for a LATCH entry (Common/OrgResources.hlsli). The members keep the
// cbuffer offsets the D3D11 path uploads; the padding is a vector, not an array, so the struct also packs tightly for the ORG latch.
struct AutoExposureConstants
{
	float2 AdaptArea;
	float2 AdaptationRange;
	float AdaptLerp;
	float ExposureCompensation;
	float PurkinjeStartEV;
	float PurkinjeMaxEV;
	float PurkinjeStrength;
	float ExposureTarget;

	float2 pad;
};
