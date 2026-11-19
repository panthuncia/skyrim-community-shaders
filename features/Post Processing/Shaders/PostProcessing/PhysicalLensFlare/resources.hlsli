#ifndef PHYSICAL_LENS_FLARE_RESOURCES
#define PHYSICAL_LENS_FLARE_RESOURCES
#include "Common/OrgResources.hlsli"
// The flare shaders' resource lists (Common/OrgResources.hlsli). Each file expands its own list where it declared its
// resources, so the D3D11 declarations stay as they were. An entry file puts every list it includes into its push block
// before the first include, since ORG reads each resource through a handle declared there.
// On D3D11 several names share a register (u1 is Selected and OutRays). On ORG each name is its own handle.

// optics.hlsli: the lens block, a constant buffer optics.hlsli declares on ORG, and the ghost paths.
#define PHYSICAL_LENS_OPTICS_RESOURCES(X) \
	X(EXTERN, LensConstants, Lens, 0)     \
	X(SRV, StructuredBuffer<GhostPath>, Paths, 2)

// exposure.hlsli: Auto Exposure's adaptation and its constants, the chain's latch region on ORG.
#define PHYSICAL_LENS_EXPOSURE_RESOURCES(X)                \
	X(SRV, StructuredBuffer<float>, ExposureAdaptation, 5) \
	X(EXTERN_LATCH, ExposureConstants, Exposure, 1)

// flare.cs.hlsl, in its two places.
#define PHYSICAL_LENS_FLARE_RESOURCES(X)                \
	X(SRV, Texture2D<float4>, Scene, 0)                 \
	X(SRV, StructuredBuffer<Source>, Sources, 1)        \
	X(UAV, RWStructuredBuffer<Source>, Candidates, 0)   \
	X(UAV, RWStructuredBuffer<Source>, Selected, 1)     \
	X(UAV, RWStructuredBuffer<float4>, RegionData, 4)   \
	X(UAV, RWStructuredBuffer<float4>, OutRays, 1)      \
	X(UAV, RWStructuredBuffer<Bundle>, OutBundles, 4)   \
	X(UAV, RWStructuredBuffer<Job>, OutJobs, 2)         \
	X(UAV, RWByteAddressBuffer, Arguments, 3)           \
	X(SRV, StructuredBuffer<Job>, Jobs, 3)
#define PHYSICAL_LENS_FLARE_PUPIL_RESOURCES(X)                    \
	X(SRV, StructuredBuffer<float2>, PupilCoordinates, 4)         \
	X(UAV, RWStructuredBuffer<float2>, OutPupilCoordinates, 4)    \
	X(SRV, StructuredBuffer<float4>, TracedRays, 0)

// planning.cs.hlsl.
#define PHYSICAL_LENS_PLANNING_RESOURCES(X)          \
	X(UAV, RWStructuredBuffer<float4>, PlanOutput, 5) \
	X(SRV, StructuredBuffer<float4>, PlanTable, 6)    \
	X(UAV, RWStructuredBuffer<uint>, PlanStatus, 0)   \
	X(UAV, RWStructuredBuffer<uint>, PlanRequests, 4) \
	X(SRV, StructuredBuffer<uint>, RequestedPlans, 0)
#endif
