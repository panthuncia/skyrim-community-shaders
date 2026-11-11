#ifndef __ORG_ENGINE_CONSTANTS_DEPENDENCY_HLSL__
#define __ORG_ENGINE_CONSTANTS_DEPENDENCY_HLSL__

// An engine constant buffer read on ORG: the bytes the engine wrote for one draw, which the
// engine-constants capture copied into its per-frame ring (src/RenderGraph/Engine/Constants,
// docs/development/p5/constants.md).
//
// The ring's chunk and the offset change every frame, so a kept recording cannot hold them. The
// pass's own latch holds them instead (an OrgEngineConstantsAt, written at commit), and the shader
// loads the buffer's struct from that chunk at that offset. Only ORG builds include this file;
// the D3D11 build keeps its cbuffer.
//
//   X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)       // in the resource list
//   struct MyPerGeometry { float4 Flags; float4 Offsets[16]; };  // the cbuffer's layout, padded
//   ORG_ENGINE_CONSTANTS(MyPerGeometry, PerGeometry, PerGeometryAt)
//   static float4 Flags = PerGeometry.Flags;                  // the names the body reads
//
// The struct must have the cbuffer's layout as raw bytes: pad a member that cbuffer packing would
// move to the next 16-byte row (a float before a float4 array is followed by a float3).

#if defined(CS_ORG_BINDLESS)

/** Where one captured constant buffer is: the ring chunk's SRV index and the byte offset in it. */
struct OrgEngineConstantsAt
{
	uint Srv;
	uint Offset;
	uint2 Padding;
};

#	define ORG_ENGINE_CONSTANTS(T, N, AT)                                           \
		static ByteAddressBuffer OrgEngineRing_##N = ResourceDescriptorHeap[AT.Srv]; \
		static T N = OrgEngineRing_##N.Load<T>(AT.Offset);

#endif

#endif  // __ORG_ENGINE_CONSTANTS_DEPENDENCY_HLSL__
