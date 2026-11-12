#ifndef __DCLF_FRAME_RECORD_DEPENDENCY_HLSL__
#define __DCLF_FRAME_RECORD_DEPENDENCY_HLSL__

// Drawcall Limit Fix's D3D12 builds: the pass's bindings read by the stage itself. On Vulkan the pipeline layout maps each register
// to its entry in the pass's frame record (DrawPipelines.h, DrawBindings) through VK_EXT_descriptor_heap. D3D12's layouts hold only
// root constants, so DCLF's FrameSource rewrite (DrawcallLimitFix, Core/Gpu/Pipelines/FrameSource.h) turns every register declaration
// a pulled build has left into a read through these: a texture or sampler from the heap at the record's index, a constant buffer
// as the pass's view of its block. ShaderPrograms puts this header first in the source of a D3D12 pulled build, before it
// preprocesses and rewrites it.

#include "Common/DeviceAddress.hlsli"

#if defined(__hlsl_dx_compiler) && !defined(__spirv__)

// The pass's push data after the draw's (DrawPipelines.h, kFramePushBinding): the frame record's address in words 0 and 1
// (kFramePushRecord, kShadowPushFrameRecord). Only the first row is declared: the root constants may be fewer than the main layout's.
cbuffer DCLFFramePush : register(b191)
{
	uint2 DCLFFrameRecordWords : packoffset(c0.x);
};

// DrawBindings' parts (DrawPipelines.h): 14 VS and 14 PS block addresses, 128 texture indices, 16 sampler indices, the pass-wide
// blocks' view indices.
static const uint kDCLFRecordVertexConstants = 0;
static const uint kDCLFRecordPixelConstants = 112;
static const uint kDCLFRecordTextures = 224;
static const uint kDCLFRecordSamplers = 736;
static const uint kDCLFRecordFrameBlocks = 800;

// The frame record's raw view and offset, looked up once.
static const uint2 DCLFFrameRecordSpan = DeviceAddressSpan((uint64_t(DCLFFrameRecordWords.y) << 32) | uint64_t(DCLFFrameRecordWords.x));

uint DCLFFrameRecordWord(uint a_byte)
{
	ByteAddressBuffer record = ResourceDescriptorHeap[DCLFFrameRecordSpan.x];
	return record.Load(DCLFFrameRecordSpan.y + a_byte);
}

uint DCLFFrameTextureIndex(uint a_register) { return DCLFFrameRecordWord(kDCLFRecordTextures + 4 * a_register); }
uint DCLFFrameSamplerIndex(uint a_register) { return DCLFFrameRecordWord(kDCLFRecordSamplers + 4 * a_register); }
// A pass-wide block's constant buffer view (FrameBlockViews), by its ordinal: VS registers ascending, then PS (kFramePushVS, kFramePushPS).
uint DCLFFrameBlockIndex(uint a_ordinal) { return DCLFFrameRecordWord(kDCLFRecordFrameBlocks + 4 * a_ordinal); }

// A block the pass gives by address only (DrawBindings' vertexConstants or pixelConstants): its raw view and offset.
uint2 DCLFFrameBlockSpan(bool a_pixel, uint a_register)
{
	const uint at = (a_pixel ? kDCLFRecordPixelConstants : kDCLFRecordVertexConstants) + 8 * a_register;
	return DeviceAddressSpan((uint64_t(DCLFFrameRecordWord(at + 4)) << 32) | uint64_t(DCLFFrameRecordWord(at)));
}
float4 DCLFSpanRegister(uint2 a_span, uint a_register)
{
	ByteAddressBuffer block = ResourceDescriptorHeap[a_span.x];
	return asfloat(block.Load4(a_span.y + 16 * a_register));
}

// A block read register by register (its members placed with packoffset): the largest constant buffer.
struct DCLFRawBlock
{
	float4 r[4096];
};

#endif

#endif  // __DCLF_FRAME_RECORD_DEPENDENCY_HLSL__
