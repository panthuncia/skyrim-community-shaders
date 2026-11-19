#ifndef __SKINNED_DEPENDENCY_HLSL__
#define __SKINNED_DEPENDENCY_HLSL__

// Drawcall Limit Fix's bindless builds read the palettes from a buffer indexed per object (below) and
// declare no bone constant buffers, so the draw's binding record does not have to supply them.
#if defined(ORG_LATCHED_BONES)
// An engine draw family's ORG build (Utility.hlsl, Lighting.hlsl): the draw's two palettes are latched with it, and the including
// shader reads a row with OrgBoneRow(palette, row) (palette 0 the previous frame's, 1 this frame's). The names the D3D11 code passes
// are the palettes' numbers, which pick the overloads below.
static const uint PreviousBones = 0;
static const uint Bones = 1;
#elif !defined(DCLF_BINDLESS)
cbuffer PreviousBonesBuffer : register(b9)
{
	float4 PreviousBones[240] : packoffset(c0);
}

cbuffer BonesBuffer : register(b10)
{
	float4 Bones[240] : packoffset(c0);
}
#endif  // ORG_LATCHED_BONES, !DCLF_BINDLESS

namespace Skinned
{
#if defined(ORG_LATCHED_BONES)
	// GetBoneTransformMatrix and GetBoneRSMatrix over a latched palette (OrgBoneRow), the same sums as the cbuffer versions below.
	float3x4 GetBoneTransformMatrix(uint palette, int4 boneIndices, float3 pivot, float4 boneWeights)
	{
		float3x4 pivotMatrix = transpose(float4x3(0.0.xxx, 0.0.xxx, 0.0.xxx, pivot));
		float3x4 boneMatrix1 = float3x4(OrgBoneRow(palette, boneIndices.x), OrgBoneRow(palette, boneIndices.x + 1), OrgBoneRow(palette, boneIndices.x + 2));
		float3x4 boneMatrix2 = float3x4(OrgBoneRow(palette, boneIndices.y), OrgBoneRow(palette, boneIndices.y + 1), OrgBoneRow(palette, boneIndices.y + 2));
		float3x4 boneMatrix3 = float3x4(OrgBoneRow(palette, boneIndices.z), OrgBoneRow(palette, boneIndices.z + 1), OrgBoneRow(palette, boneIndices.z + 2));
		float3x4 boneMatrix4 = float3x4(OrgBoneRow(palette, boneIndices.w), OrgBoneRow(palette, boneIndices.w + 1), OrgBoneRow(palette, boneIndices.w + 2));
		return (boneMatrix1 - pivotMatrix) * boneWeights.x + (boneMatrix2 - pivotMatrix) * boneWeights.y + (boneMatrix3 - pivotMatrix) * boneWeights.z +
		       (boneMatrix4 - pivotMatrix) * boneWeights.w;
	}

	float3x3 GetBoneRSMatrix(uint palette, int4 boneIndices, float4 boneWeights)
	{
		float3x3 result;
		for (int rowIndex = 0; rowIndex < 3; ++rowIndex) {
			result[rowIndex] = boneWeights.xxx * OrgBoneRow(palette, boneIndices.x + rowIndex).xyz +
			                   boneWeights.yyy * OrgBoneRow(palette, boneIndices.y + rowIndex).xyz +
			                   boneWeights.zzz * OrgBoneRow(palette, boneIndices.z + rowIndex).xyz +
			                   boneWeights.www * OrgBoneRow(palette, boneIndices.w + rowIndex).xyz;
		}
		return result;
	}
#endif  // ORG_LATCHED_BONES

#if defined(DCLF_BINDLESS)
	// The same sums over rows fetched from DCLFPalettes at a per-object base. No pivot: the epoch packs the
	// rows relative to its own eye, exactly as it packs World, so the subtraction has already happened.
	// The rows are the engine's palette as it is, in absolute world space; the pivot (the drawing camera's
	// posAdjust) comes off each bone before the blend, as GetBoneTransformMatrix does, and as the CPU used to
	// do when it packed the rows eye-relative.
	float3x4 GetBoneTransformMatrixBindless(uint base, int4 boneIndices, float3 pivot, float4 boneWeights)
	{
		precise float3x4 pivotMatrix = transpose(float4x3(0.0.xxx, 0.0.xxx, 0.0.xxx, pivot));
		precise float3x4 boneMatrix1 = float3x4(DCLFPalettes[base + boneIndices.x], DCLFPalettes[base + boneIndices.x + 1], DCLFPalettes[base + boneIndices.x + 2]) - pivotMatrix;
		precise float3x4 boneMatrix2 = float3x4(DCLFPalettes[base + boneIndices.y], DCLFPalettes[base + boneIndices.y + 1], DCLFPalettes[base + boneIndices.y + 2]) - pivotMatrix;
		precise float3x4 boneMatrix3 = float3x4(DCLFPalettes[base + boneIndices.z], DCLFPalettes[base + boneIndices.z + 1], DCLFPalettes[base + boneIndices.z + 2]) - pivotMatrix;
		precise float3x4 boneMatrix4 = float3x4(DCLFPalettes[base + boneIndices.w], DCLFPalettes[base + boneIndices.w + 1], DCLFPalettes[base + boneIndices.w + 2]) - pivotMatrix;
		return boneMatrix1 * boneWeights.x + boneMatrix2 * boneWeights.y + boneMatrix3 * boneWeights.z + boneMatrix4 * boneWeights.w;
	}

	float3x3 GetBoneRSMatrixBindless(uint base, int4 boneIndices, float4 boneWeights)
	{
		float3x3 result;
		for (int rowIndex = 0; rowIndex < 3; ++rowIndex) {
			result[rowIndex] = boneWeights.xxx * DCLFPalettes[base + boneIndices.x + rowIndex].xyz +
			                   boneWeights.yyy * DCLFPalettes[base + boneIndices.y + rowIndex].xyz +
			                   boneWeights.zzz * DCLFPalettes[base + boneIndices.z + rowIndex].xyz +
			                   boneWeights.www * DCLFPalettes[base + boneIndices.w + rowIndex].xyz;
		}
		return result;
	}
#endif  // DCLF_BINDLESS

	float3x4 GetBoneTransformMatrix(float4 bonePositions[240], int4 boneIndices, float3 pivot, float4 boneWeights)
	{
		float3x4 pivotMatrix = transpose(float4x3(0.0.xxx, 0.0.xxx, 0.0.xxx, pivot));

		float3x4 boneMatrix1 =
			float3x4(bonePositions[boneIndices.x], bonePositions[boneIndices.x + 1], bonePositions[boneIndices.x + 2]);
		float3x4 boneMatrix2 =
			float3x4(bonePositions[boneIndices.y], bonePositions[boneIndices.y + 1], bonePositions[boneIndices.y + 2]);
		float3x4 boneMatrix3 =
			float3x4(bonePositions[boneIndices.z], bonePositions[boneIndices.z + 1], bonePositions[boneIndices.z + 2]);
		float3x4 boneMatrix4 =
			float3x4(bonePositions[boneIndices.w], bonePositions[boneIndices.w + 1], bonePositions[boneIndices.w + 2]);

		float3x4 unitMatrix = float3x4(1.0.xxxx, 1.0.xxxx, 1.0.xxxx);
		float3x4 weightMatrix1 = unitMatrix * boneWeights.x;
		float3x4 weightMatrix2 = unitMatrix * boneWeights.y;
		float3x4 weightMatrix3 = unitMatrix * boneWeights.z;
		float3x4 weightMatrix4 = unitMatrix * boneWeights.w;

		return (boneMatrix1 - pivotMatrix) * weightMatrix1 +
		       (boneMatrix2 - pivotMatrix) * weightMatrix2 +
		       (boneMatrix3 - pivotMatrix) * weightMatrix3 +
		       (boneMatrix4 - pivotMatrix) * weightMatrix4;
	}

	float3x3 GetBoneRSMatrix(float4 bonePositions[240], int4 boneIndices, float4 boneWeights)
	{
		float3x3 result;
		for (int rowIndex = 0; rowIndex < 3; ++rowIndex) {
			result[rowIndex] = boneWeights.xxx * bonePositions[boneIndices.x + rowIndex].xyz +
			                   boneWeights.yyy * bonePositions[boneIndices.y + rowIndex].xyz +
			                   boneWeights.zzz * bonePositions[boneIndices.z + rowIndex].xyz +
			                   boneWeights.www * bonePositions[boneIndices.w + rowIndex].xyz;
		}
		return result;
	}
}

#endif  // __SKINNED_DEPENDENCY_HLSL__