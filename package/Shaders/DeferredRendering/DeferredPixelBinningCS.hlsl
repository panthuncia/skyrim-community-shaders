#include "DeferredRendering/DeferredMaterial.hlsli"

Texture2D<uint4> PackedSurfaceTexture : register(t0);
RWStructuredBuffer<uint> EvaluatorCounts : register(u0);
RWStructuredBuffer<uint> EvaluatorOffsets : register(u1);
RWStructuredBuffer<uint> EvaluatorWriteCursors : register(u2);
RWStructuredBuffer<uint2> PixelList : register(u3);
RWByteAddressBuffer IndirectCommands : register(u4);
RWTexture2D<uint> MaterialClassification : register(u5);

cbuffer PixelBinningConstants : register(b0)
{
	uint width;
	uint height;
	uint evaluatorCount;
	uint indirectCommandStride;
	uint enabledEvaluatorMask;
	uint classificationEnabled;
};

static const uint CS_CLASSIFICATION_VISIBLE_BASE = 160u;
static const uint CS_CLASSIFICATION_DEFERRED_BASE = CS_CLASSIFICATION_VISIBLE_BASE + 256u;

uint ActiveEvaluatorForMaterial(uint materialClass)
{
	uint evaluator = min(CSDeferredEvaluatorForMaterial(materialClass), evaluatorCount - 1u);
	return CSDeferredEvaluatorEnabled(evaluator, enabledEvaluatorMask) ?
		evaluator : CS_EVALUATOR_COMPATIBILITY;
}

// Four root constants followed by D3D12_DISPATCH_ARGUMENTS.
static const uint CS_INDIRECT_EVALUATOR_OFFSET = 0u;
static const uint CS_INDIRECT_BASE_OFFSET = 4u;
static const uint CS_INDIRECT_COUNT_OFFSET = 8u;
static const uint CS_INDIRECT_DISPATCH_WIDTH_OFFSET = 12u;
static const uint CS_INDIRECT_DISPATCH_X_OFFSET = 16u;
static const uint CS_INDIRECT_DISPATCH_Y_OFFSET = 20u;
static const uint CS_INDIRECT_DISPATCH_Z_OFFSET = 24u;
static const uint CS_EVALUATOR_GROUP_SIZE = 64u;

[numthreads(64, 1, 1)]
void ClearBinsCS(uint3 threadID : SV_DispatchThreadID)
{
	if (threadID.x < evaluatorCount) {
		EvaluatorCounts[threadID.x] = 0u;
		EvaluatorOffsets[threadID.x] = 0u;
		EvaluatorWriteCursors[threadID.x] = 0u;
		uint command = threadID.x * indirectCommandStride;
		[unroll] for (uint byteOffset = 0u; byteOffset < 28u; byteOffset += 4u)
			IndirectCommands.Store(command + byteOffset, 0u);
	}
	// This shared diagnostic surface also contains the deferred parity counters.
	// Clear it once at the structural beginning of the epoch so later passes do
	// not erase classification results produced here.
	if (threadID.x < 672u)
		MaterialClassification[uint2(threadID.x, 0u)] = 0u;
}

groupshared uint groupHistogram[CS_EVALUATOR_COUNT];
groupshared uint groupVisibleMaterials[256];
groupshared uint groupDeferredMaterials[256];

[numthreads(8, 8, 1)]
void HistogramCS(uint3 threadID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
	if (groupIndex < evaluatorCount)
		groupHistogram[groupIndex] = 0u;
	if (classificationEnabled) {
		for (uint index = groupIndex; index < 256u; index += 64u) {
			groupVisibleMaterials[index] = 0u;
			groupDeferredMaterials[index] = 0u;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (threadID.x < width && threadID.y < height) {
		uint4 packedSurface = PackedSurfaceTexture.Load(int3(threadID.xy, 0));
		uint materialClass = CSDeferredMaterialClass(packedSurface.x);
		uint evaluator = ActiveEvaluatorForMaterial(materialClass);
		InterlockedAdd(groupHistogram[evaluator], 1u);
		// Rasterization sets bit 31 only for pixels written by a material shader.
		// This excludes the packed-target clear/background from blind-spot counts.
		bool compatibilityMarker = materialClass == CS_MATERIAL_Legacy &&
			(packedSurface.w & 0x80000000u) != 0u;
		if (classificationEnabled && (materialClass != CS_MATERIAL_Legacy || compatibilityMarker)) {
			uint intendedClass = compatibilityMarker ? packedSurface.w & 0xFFu : materialClass;
			InterlockedAdd(groupVisibleMaterials[intendedClass], 1u);
			if (evaluator != CS_EVALUATOR_COMPATIBILITY)
				InterlockedAdd(groupDeferredMaterials[intendedClass], 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (groupIndex < evaluatorCount)
		InterlockedAdd(EvaluatorCounts[groupIndex], groupHistogram[groupIndex]);
	if (classificationEnabled) {
		for (uint index = groupIndex; index < 256u; index += 64u) {
			if (groupVisibleMaterials[index])
				InterlockedAdd(MaterialClassification[uint2(CS_CLASSIFICATION_VISIBLE_BASE + index, 0u)], groupVisibleMaterials[index]);
			if (groupDeferredMaterials[index])
				InterlockedAdd(MaterialClassification[uint2(CS_CLASSIFICATION_DEFERRED_BASE + index, 0u)], groupDeferredMaterials[index]);
		}
	}
}

[numthreads(64, 1, 1)]
void PrefixSumAndArgsCS(uint3 threadID : SV_DispatchThreadID)
{
	if (threadID.x != 0u)
		return;
	uint offset = 0u;
	for (uint evaluator = 0u; evaluator < evaluatorCount; ++evaluator) {
		uint count = EvaluatorCounts[evaluator];
		EvaluatorOffsets[evaluator] = offset;
		uint groupsNeeded = (count + CS_EVALUATOR_GROUP_SIZE - 1u) / CS_EVALUATOR_GROUP_SIZE;
		uint dispatchX = groupsNeeded == 0u ? 0u : min(65535u, (uint)ceil(sqrt((float)groupsNeeded)));
		uint dispatchY = dispatchX == 0u ? 0u : (groupsNeeded + dispatchX - 1u) / dispatchX;
		uint command = evaluator * indirectCommandStride;
		IndirectCommands.Store(command + CS_INDIRECT_EVALUATOR_OFFSET, evaluator);
		IndirectCommands.Store(command + CS_INDIRECT_BASE_OFFSET, offset);
		IndirectCommands.Store(command + CS_INDIRECT_COUNT_OFFSET, count);
		IndirectCommands.Store(command + CS_INDIRECT_DISPATCH_WIDTH_OFFSET, dispatchX * CS_EVALUATOR_GROUP_SIZE);
		IndirectCommands.Store(command + CS_INDIRECT_DISPATCH_X_OFFSET, dispatchX);
		IndirectCommands.Store(command + CS_INDIRECT_DISPATCH_Y_OFFSET, dispatchY);
		IndirectCommands.Store(command + CS_INDIRECT_DISPATCH_Z_OFFSET, count == 0u ? 0u : 1u);
		offset += count;
	}
}

[numthreads(8, 8, 1)]
void ScatterPixelsCS(uint3 threadID : SV_DispatchThreadID)
{
	if (threadID.x >= width || threadID.y >= height)
		return;
	uint materialClass = CSDeferredMaterialClass(PackedSurfaceTexture.Load(int3(threadID.xy, 0)).x);
	uint evaluator = ActiveEvaluatorForMaterial(materialClass);
	uint localOffset;
	InterlockedAdd(EvaluatorWriteCursors[evaluator], 1u, localOffset);
	PixelList[EvaluatorOffsets[evaluator] + localOffset] = threadID.xy;
}
