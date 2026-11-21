#ifndef __ORG_RESOURCES_DEPENDENCY_HLSL__
#define __ORG_RESOURCES_DEPENDENCY_HLSL__

// Resource declarations that build for both of CS's shader paths from one list. The macros are ORGModuleServices'
// (shaders/OrgResources.hlsli, staged at Data/Shaders/ORGModuleServices); its comment explains the list and its kinds.
// - D3D11 (FXC, *_5_0): each entry is a register-bound declaration, as before.
// - ORG (DXC, CS_ORG_BINDLESS): each entry is a handle in the push block, read from the descriptor heap.
// The C++ side is src/RenderGraph/ShaderBindings.h.
//
// CS adds one kind:
// - SHARED: CS's shared constant buffers, first in the list: X(SHARED, ConstantBuffer, Shared, 0)
//   (type, name and register are not used). D3D11: nothing here; FrameBuffer (b12), SharedData
//   (b5), FeatureData (b6) and Permutation (b4) stay the cbuffers their headers declare. ORG: four
//   handles, in that order. With ORG_SHARED_CONSTANTS defined, those headers then read them from
//   the heap (Common/NamespacedCBuffer.hlsli, ORG_SHARED_*), and shader bodies are unchanged. The
//   C++ side fills them from SharedConstants (src/RenderGraph/SharedConstants.h).
//
// A shader with SHARED entries expands its push block before the shared headers and its own declarations after them:
//
//   #define ORG_SHARED_CONSTANTS
//   #include "Common/OrgResources.hlsli"
//   #define MY_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, InputTexture, 0)
//   ORG_PUSH_BLOCK(MY_RESOURCES(ORG_HANDLE))
//   #include "Common/SharedData.hlsli"
//   MY_RESOURCES(ORG_RESOURCE)
//
// Shared headers' own resources (SharedData's depth, Light Limit Fix's light lists, IBL, Terrain and Cloud Shadows,
// Physical Sky) are declared with ORG_HEADER_SRV and ORG_HEADER_SAMPLER. Handle tables are written by AnchorPasses'
// Dispatch::table and Bind::Table.

#if defined(CS_ORG_BINDLESS) && !defined(ORG_BINDLESS)
#	define ORG_BINDLESS
#endif

#include "ORGModuleServices/OrgResources.hlsli"

#if defined(CS_ORG_BINDLESS)
#	define ORG_HANDLE_SHARED(T, N, R) \
		uint OrgHandle_FrameBuffer;    \
		uint OrgHandle_SharedData;     \
		uint OrgHandle_FeatureData;    \
		uint OrgHandle_Permutation;
#endif
#define ORG_RESOURCE_SHARED(T, N, R)

#endif  // __ORG_RESOURCES_DEPENDENCY_HLSL__
