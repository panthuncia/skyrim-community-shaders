#ifndef __DCLF_FRAME_RECORD_DEPENDENCY_HLSL__
#define __DCLF_FRAME_RECORD_DEPENDENCY_HLSL__

// Drawcall Limit Fix's frame record (DrawPipelines.h, DrawBindings) as a stage reads it itself, by its buffer view ref.
//
// On Vulkan the pipeline layout maps each register to its entry in the pass's frame record through VK_EXT_descriptor_heap, from the
// record's device address in the pass's push data. Those addresses, and the block addresses in the record and in a draw's sequence,
// stay device addresses: the layout reads them. A stage that reads through one turns it into a ref with the record's backings
// (DCLFRefFromBacking): each is a buffer such an address points into, by its ref and its address.
//
// D3D12's layouts hold only root constants, so DCLF's FrameSource rewrite (DrawcallLimitFix, Core/Gpu/Pipelines/FrameSource.h) turns
// every register declaration a pulled build has left into a read through the record: a texture or sampler from the heap at the record's
// index, a constant buffer as the pass's view of its block. ShaderPrograms puts this header first in the source of a D3D12 pulled build,
// before it preprocesses and rewrites it.

#include "Common/DeviceRef.hlsli"

#if defined(__spirv__) || defined(__HLSL_VERSION)

// The pass's push data after the draw's (DrawPipelines.h, kFramePushBinding): the frame record's device address in words 0 and 1
// (kFramePushRecord, kShadowPushFrameRecord: the layout's), its ref in words 2 and 3 (kFramePushRecordRef, kShadowPushFrameRecordRef).
// Only the first row is declared: the shadow views' push words are fewer than the main layout's.
cbuffer DCLFFramePush : register(b191)
{
	uint2 DCLFFrameRecordWords : packoffset(c0.x);
	uint2 DCLFFrameRecordRef : packoffset(c0.z);
};

// DrawBindings' parts (DrawPipelines.h): 14 VS and 14 PS block addresses, 128 texture indices, 16 sampler indices, the pass-wide
// blocks' view indices, then the backings (RecordBacking: ref, address, bytes; 32 bytes each).
static const uint kDCLFRecordVertexConstants = 0;
static const uint kDCLFRecordPixelConstants = 112;
static const uint kDCLFRecordTextures = 224;
static const uint kDCLFRecordSamplers = 736;
static const uint kDCLFRecordFrameBlocks = 800;
static const uint kDCLFRecordBackings = 864;
// RecordBackingIndex (DrawPipelines.h).
static const uint kDCLFBackingFrameConstants = 0;
static const uint kDCLFBackingPipelineRows = 1;
static const uint kDCLFBackingMaterialRows = 2;
static const uint kDCLFBackingAnimatedRows = 3;
static const uint kDCLFBackingShadowRows = 4;

uint DCLFFrameRecordWord(uint a_byte) { return DeviceLoad(uint, DeviceRefAdd(DCLFFrameRecordRef, a_byte)); }

// Backing a_backing's ref (xy) and device address (zw).
uint4 DCLFRecordBacking(uint a_backing) { return DeviceLoad(uint4, DeviceRefAdd(DCLFFrameRecordRef, kDCLFRecordBackings + 32 * a_backing)); }
// A device address into backing a_backing as a ref (the null ref for 0).
DeviceRef DCLFRefFromBacking(uint a_backing, uint2 a_address)
{
	const uint4 backing = DCLFRecordBacking(a_backing);
	return DeviceRefFromVA(backing.xy, backing.zw, a_address);
}
// Whether a_address is in backing a_backing (its bytes after its address).
bool DCLFInBacking(uint a_backing, uint2 a_address)
{
	const uint4 backing = DCLFRecordBacking(a_backing);
	const uint bytes = DCLFFrameRecordWord(kDCLFRecordBackings + 32 * a_backing + 16);
	const uint64_t at = ((uint64_t(a_address.y) << 32) | uint64_t(a_address.x)) - ((uint64_t(backing.w) << 32) | uint64_t(backing.z));
	return any(backing.zw != 0) && at < uint64_t(bytes);
}
// A material row's device address (a sequence's: BuildDrawsCS's MaterialRowAddress) as a ref: in the frame's copy of the animated rows
// when it is there, else in the installed material rows.
DeviceRef DCLFMaterialRowRef(uint2 a_address)
{
	return DCLFRefFromBacking(DCLFInBacking(kDCLFBackingAnimatedRows, a_address) ? kDCLFBackingAnimatedRows : kDCLFBackingMaterialRows, a_address);
}

#endif

#if defined(__hlsl_dx_compiler) && !defined(__spirv__)

uint DCLFFrameTextureIndex(uint a_register) { return DCLFFrameRecordWord(kDCLFRecordTextures + 4 * a_register); }
uint DCLFFrameSamplerIndex(uint a_register) { return DCLFFrameRecordWord(kDCLFRecordSamplers + 4 * a_register); }
// A pass-wide block's constant buffer view (FrameBlockViews), by its ordinal: VS registers ascending, then PS (kFramePushVS, kFramePushPS).
uint DCLFFrameBlockIndex(uint a_ordinal) { return DCLFFrameRecordWord(kDCLFRecordFrameBlocks + 4 * a_ordinal); }

// A block the pass gives by address only (DrawBindings' vertexConstants or pixelConstants, in the frame constants): its ref.
DeviceRef DCLFFrameBlockSpan(bool a_pixel, uint a_register)
{
	const uint at = (a_pixel ? kDCLFRecordPixelConstants : kDCLFRecordVertexConstants) + 8 * a_register;
	return DCLFRefFromBacking(kDCLFBackingFrameConstants, uint2(DCLFFrameRecordWord(at), DCLFFrameRecordWord(at + 4)));
}
float4 DCLFSpanRegister(DeviceRef a_block, uint a_register)
{
	return asfloat(DeviceLoad(uint4, DeviceRefAdd(a_block, 16 * a_register)));
}

// A block read register by register (its members placed with packoffset): the largest constant buffer.
struct DCLFRawBlock
{
	float4 r[4096];
};

#endif

#endif  // __DCLF_FRAME_RECORD_DEPENDENCY_HLSL__
