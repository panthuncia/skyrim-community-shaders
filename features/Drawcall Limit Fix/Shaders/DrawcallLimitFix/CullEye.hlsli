#ifndef DCLF_CULL_EYE_HLSLI
#define DCLF_CULL_EYE_HLSLI

// BuildDrawsLatch::cullEye, the eye the latch's view-projection is relative to. Project a world position less this eye in one
// precise expression, since folding it into a matrix or reordering would cancel large terms in float. w is the radius margin.
static const uint kLatchCullEye = 368;

float4 LatchCullEye(ByteAddressBuffer a_latch, uint a_latchOffset)
{
	return asfloat(a_latch.Load4(a_latchOffset + kLatchCullEye));
}

// A portal query's radius with the margin. Only a positive one grows, since the test culls a zero radius whatever the planes.
float PortalRadiusWithMargin(float a_radius, float a_margin)
{
	return a_radius > 0.0f ? a_radius + a_margin : a_radius;
}

#endif  // DCLF_CULL_EYE_HLSLI
