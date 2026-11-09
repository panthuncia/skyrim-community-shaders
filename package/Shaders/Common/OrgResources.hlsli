#ifndef __ORG_RESOURCES_DEPENDENCY_HLSL__
#define __ORG_RESOURCES_DEPENDENCY_HLSL__

// Resource declarations that build for both of CS's shader paths from one list:
// - D3D11 (FXC, *_5_0): each entry is a register-bound declaration, as before.
// - ORG (DXC, CS_ORG_BINDLESS): each entry is a handle in the push block, a cbuffer at b0. The
//   resource is fetched from the descriptor heap by that handle (BasicRHI's bindless ABI). This
//   expansion builds for SPIR-V (Vulkan) and for DXIL (D3D12, SM 6.6) alike.
//
// A shader lists its resources once, as an X-macro, then expands it twice: into the push block,
// and into the declarations.
//
//   #define MY_RESOURCES(X)                           \
//       X(SRV, Texture2D<float4>, InputTexture, 0)    \
//       X(UAV, RWTexture2D<float4>, OutputTexture, 0) \
//       X(SAMPLER, SamplerState, LinearSampler, 0)    \
//       X(CBUFFER, MyConstants, Constants, 1)         \
//       X(LATCH, MyFrame, Frame, 2)                   \
//       X(PUSH, MyDispatch, Dispatch, 3)
//   ORG_PUSH_BLOCK(MY_RESOURCES(ORG_HANDLE))
//   MY_RESOURCES(ORG_RESOURCE)
//
// The shader invokes the list itself: FXC's preprocessor cannot take a list's name as a macro
// argument.
//
// Each entry is X(kind, type, name, register number):
// - SRV: t#. Texture2D/3D/Cube(+Array), StructuredBuffer, ByteAddressBuffer, Buffer.
// - UAV: u#. RWTexture*, RWStructuredBuffer, RWByteAddressBuffer, RWBuffer.
// - SAMPLER: s#. SamplerState or SamplerComparisonState.
// - CBUFFER: b#. A struct. D3D11: a cbuffer holding one member of that struct. ORG: a constant
//   buffer read from the heap (ConstantBuffer<T>). Code reads its fields as name.field on both.
// - LATCH: b#. A struct. D3D11: as CBUFFER. ORG: the struct loaded from a raw buffer at a byte
//   offset, both given in the push block: the execution's region of an org::LatchBlock. For values
//   that change every frame, which a recording reused across frames cannot hold: the host writes the
//   region just before the submission. The struct must have the same layout packed tightly as in
//   cbuffer packing: no member crosses a 16-byte row, and there are no gaps.
// - PUSH: b#. A struct. D3D11: as CBUFFER. ORG: the struct itself, inside the push block. For
//   small values fixed in the recording that change between its dispatches (a mip level, a pass
//   index). PUSH entries go last.
// - EXTERN: a resource an included header declares with ORG_RESOURCE. D3D11: nothing here (the
//   header binds the register). ORG: its handle only; the header reads OrgHandle_<name>.
// - EXTERN_LATCH: a LATCH an included header declares with ORG_RESOURCE. D3D11: nothing here. ORG:
//   its handle and byte offset only, as a LATCH entry's; the header reads them.
// - SHARED: CS's shared constant buffers, first in the list: X(SHARED, ConstantBuffer, Shared, 0)
//   (type, name and register are not used). D3D11: nothing here; FrameBuffer (b12), SharedData
//   (b5), FeatureData (b6) and Permutation (b4) stay the cbuffers their headers declare. ORG: four
//   handles, in that order. With ORG_SHARED_CONSTANTS defined, those headers then read them from
//   the heap (Common/NamespacedCBuffer.hlsli, ORG_SHARED_*), and shader bodies are unchanged. The
//   C++ side fills them from SharedConstants (src/RenderGraph/SharedConstants.h).
//
// Resources declared by headers: their handles must exist before the header is included, so a
// shader with EXTERN or SHARED entries expands its push block before those includes and its own
// declarations after them:
//
//   #define ORG_SHARED_CONSTANTS
//   #include "Common/OrgResources.hlsli"
//   #define MY_RESOURCES(X)                  //       X(SHARED, ConstantBuffer, Shared, 0) //       X(SRV, Texture2D<float4>, InputTexture, 0)
//   ORG_PUSH_BLOCK(MY_RESOURCES(ORG_HANDLE))
//   #include "Common/SharedData.hlsli"
//   MY_RESOURCES(ORG_RESOURCE)
//
// The ORG push block, which the C++ side mirrors (src/RenderGraph/ShaderBindings.h):
// 1. one uint handle per SRV, UAV, SAMPLER, CBUFFER and EXTERN entry, two uints per LATCH and
//    EXTERN_LATCH entry (the handle, then the byte offset), and four for SHARED, in list order;
// 2. then each PUSH struct, in list order, each starting on a 16-byte boundary (cbuffer packing).
// The whole block is at most 32 words (128 bytes, Vulkan's guaranteed push constant size).
//
// Not covered: arrays of resources and append/consume counters. A list cannot hold another list
// (FXC rejects a macro passed on as an argument), so the shared handles are one SHARED entry.

#if defined(CS_ORG_BINDLESS)

#	define ORG_PUSH_BLOCK(FIELDS) \
		cbuffer OrgBindings : register(b0) { FIELDS }

#	define ORG_HANDLE_SRV(T, N, R) uint OrgHandle_##N;
#	define ORG_HANDLE_UAV(T, N, R) uint OrgHandle_##N;
#	define ORG_HANDLE_SAMPLER(T, N, R) uint OrgHandle_##N;
#	define ORG_HANDLE_CBUFFER(T, N, R) uint OrgHandle_##N;
#	define ORG_HANDLE_LATCH(T, N, R) uint OrgHandle_##N, OrgOffset_##N;
#	define ORG_HANDLE_PUSH(T, N, R) T N;
#	define ORG_HANDLE_EXTERN(T, N, R) uint OrgHandle_##N;
#	define ORG_HANDLE_EXTERN_LATCH(T, N, R) uint OrgHandle_##N, OrgOffset_##N;
#	define ORG_HANDLE_SHARED(T, N, R) \
		uint OrgHandle_FrameBuffer;    \
		uint OrgHandle_SharedData;     \
		uint OrgHandle_FeatureData;    \
		uint OrgHandle_Permutation;
#	define ORG_HANDLE(K, T, N, R) ORG_HANDLE_##K(T, N, R)

#	define ORG_RESOURCE_SRV(T, N, R) static T N = ResourceDescriptorHeap[OrgHandle_##N];
#	define ORG_RESOURCE_UAV(T, N, R) static T N = ResourceDescriptorHeap[OrgHandle_##N];
#	define ORG_RESOURCE_SAMPLER(T, N, R) static T N = SamplerDescriptorHeap[OrgHandle_##N];
#	define ORG_RESOURCE_CBUFFER(T, N, R) static ConstantBuffer<T> N = ResourceDescriptorHeap[OrgHandle_##N];
#	define ORG_RESOURCE_LATCH(T, N, R)                                                   \
		static ByteAddressBuffer OrgLatch_##N = ResourceDescriptorHeap[OrgHandle_##N]; \
		static T N = OrgLatch_##N.Load<T>(OrgOffset_##N);
#	define ORG_RESOURCE_PUSH(T, N, R)
#	define ORG_RESOURCE_EXTERN(T, N, R)
#	define ORG_RESOURCE_EXTERN_LATCH(T, N, R)
#	define ORG_RESOURCE_SHARED(T, N, R)
#	define ORG_RESOURCE(K, T, N, R) ORG_RESOURCE_##K(T, N, R)

#else

// FXC rejects an empty macro argument, so the handles expand to their kinds, which
// ORG_PUSH_BLOCK drops.
#	define ORG_PUSH_BLOCK(FIELDS)
#	define ORG_HANDLE(K, T, N, R) K

#	define ORG_RESOURCE_SRV(T, N, R) T N : register(t##R);
#	define ORG_RESOURCE_UAV(T, N, R) T N : register(u##R);
#	define ORG_RESOURCE_SAMPLER(T, N, R) T N : register(s##R);
#	define ORG_RESOURCE_CBUFFER(T, N, R) \
		cbuffer N##Buffer : register(b##R) { T N; }
#	define ORG_RESOURCE_LATCH(T, N, R) ORG_RESOURCE_CBUFFER(T, N, R)
#	define ORG_RESOURCE_PUSH(T, N, R) ORG_RESOURCE_CBUFFER(T, N, R)
#	define ORG_RESOURCE_EXTERN(T, N, R)
#	define ORG_RESOURCE_EXTERN_LATCH(T, N, R)
#	define ORG_RESOURCE_SHARED(T, N, R)
#	define ORG_RESOURCE(K, T, N, R) ORG_RESOURCE_##K(T, N, R)

#endif

#endif  // __ORG_RESOURCES_DEPENDENCY_HLSL__
