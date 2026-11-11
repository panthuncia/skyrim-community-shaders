#ifndef __ORG_ENGINE_CONSTANTS_DEPENDENCY_HLSL__
#define __ORG_ENGINE_CONSTANTS_DEPENDENCY_HLSL__

// An engine constant buffer read on ORG: the bytes the engine wrote for one draw, which the
// engine-constants capture copied into its per-frame ring (src/RenderGraph/Engine/Constants,
// docs/development/p5/constants.md).
//
// The ring's chunk and the offset change every frame, so a kept recording cannot hold them. The
// pass's own latch holds them instead (an OrgEngineConstantsAt, written at commit), and the shader
// reads the buffer by 16-byte row from that chunk at that offset. Only ORG builds include this
// file; the D3D11 build keeps its cbuffer.
//
//   X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 2)              // in the resource list
//   static float4 Flags = OrgEngineRow(PerGeometryAt, 0);          // c0, the names the body reads
//   static float4 Offsets[16] = ORG_ENGINE_ROWS16(PerGeometryAt, 1);  // c1 to c16
//
// A row past the bound buffer reads 0, as D3D11 reads past a constant buffer's end: the engine's
// pools are sized per shader (16 bytes times the size class), and the ring holds other draws'
// bytes after a copy.
//
// FrameBuffer is read the same way when the shader defines ORG_SHARED_FRAMEBUFFER_AT as the latch's
// name before Common/FrameBuffer.hlsli is included (Common/NamespacedCBuffer.hlsli): the draw then
// reads the PerFrame bytes it had bound, not CS's shared snapshot.

#if defined(CS_ORG_BINDLESS)

/**
 * Where a draw's captured constant buffers are: the ring chunk's SRV index, the byte offset and the bound size of its
 * PerGeometry (PS b2) and of its PerFrame (PS b12, read as FrameBuffer when the shader defines ORG_SHARED_FRAMEBUFFER_AT).
 */
struct OrgEngineConstantsAt
{
	uint Srv;
	uint Offset;
	uint FrameSrv;
	uint FrameOffset;
	uint Size;
	uint FrameSize;
	uint2 Padding;
};

/** One 16-byte row of a draw's PerGeometry; 0 past its bound size. */
float4 OrgEngineRow(OrgEngineConstantsAt a_at, uint a_row)
{
	ByteAddressBuffer ring = ResourceDescriptorHeap[a_at.Srv];
	return a_row * 16 < a_at.Size ? asfloat(ring.Load4(a_at.Offset + a_row * 16)) : 0.0.xxxx;
}

#	define ORG_ENGINE_ROWS16(AT, FIRST) { OrgEngineRow(AT, (FIRST) + 0), OrgEngineRow(AT, (FIRST) + 1), OrgEngineRow(AT, (FIRST) + 2), OrgEngineRow(AT, (FIRST) + 3), OrgEngineRow(AT, (FIRST) + 4), OrgEngineRow(AT, (FIRST) + 5), OrgEngineRow(AT, (FIRST) + 6), OrgEngineRow(AT, (FIRST) + 7), OrgEngineRow(AT, (FIRST) + 8), OrgEngineRow(AT, (FIRST) + 9), OrgEngineRow(AT, (FIRST) + 10), OrgEngineRow(AT, (FIRST) + 11), OrgEngineRow(AT, (FIRST) + 12), OrgEngineRow(AT, (FIRST) + 13), OrgEngineRow(AT, (FIRST) + 14), OrgEngineRow(AT, (FIRST) + 15) }

#endif

#endif  // __ORG_ENGINE_CONSTANTS_DEPENDENCY_HLSL__
