#pragma once

// The HZB's GPU layout. Must match HzbCS.hlsl (features/Drawcall Limit Fix/Shaders/DrawcallLimitFix/Features/GpuOcclusion).

#include <cstdint>

namespace DCLF::Features::GpuOcclusion
{
	inline constexpr const char* kHzbShader = "DrawcallLimitFix/Features/GpuOcclusion/HzbCS.hlsl";
	inline constexpr std::uint32_t kHzbConstantWords = 24;
	// Levels one FidelityFX SPD dispatch writes, enough to take 4096 texels down to one.
	inline constexpr std::uint32_t kHzbDispatchMips = 12;
	inline constexpr std::uint32_t kHzbTile = 64;

	struct HzbConstants
	{
		std::uint32_t sourceIndex;
		std::uint32_t fromDepth;
		std::uint32_t validSize[2];   // Source texels that hold depth.
		std::uint32_t domainSize[2];  // Power-of-two extent. Reads past validSize return the far plane.
		std::uint32_t targetSize[2];  // Size of the first written level.
		std::uint32_t mips;
		std::uint32_t workGroups;
		std::uint32_t counterIndex;
		std::uint32_t padding;
		std::uint32_t targetIndices[kHzbDispatchMips];
	};
	static_assert(sizeof(HzbConstants) / 4 == kHzbConstantWords);
}
