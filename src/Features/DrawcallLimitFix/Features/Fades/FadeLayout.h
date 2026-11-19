#pragma once

// FadeStateCS's shader, constants and group size. These must match the HLSL.
// The rows it reads and writes are in Core/Scene/Model/Records.h because the cull reads them too.

#include <cstdint>

namespace DCLF::Draws
{
	constexpr const char* kFadeStateShader = "DrawcallLimitFix/Features/Fades/FadeStateCS.hlsl";
	// Must match the HLSL.
	struct FadeStateConstants
	{
		std::uint32_t rootsIndex = 0, statesIndex = 0, frameIndex = 0;
		std::uint32_t latchIndex = 0, latchOffset = 0, logIndex = 0;
		std::uint32_t outIndices[2]{};        // indexed by the scene frame's parity
		std::uint32_t reservedProgramsWord = 0;  // unused, the programs come from the depth latch
		std::uint32_t rootProgramsIndex = 0;  // StructuredBuffer<uint>, one program per root
		std::uint32_t animatedIndex = 0;      // StructuredBuffer<uint>, per root the scene frame of its last animation update
		std::uint32_t eventsIndex = 0;        // RWStructuredBuffer<uint> of FadeEvent
		std::uint32_t reportedIndex = 0;      // RWStructuredBuffer<uint2>, per root the generation and milestone last reported
		std::uint32_t eventCapacity = 0;      // capacity of the list and of each frame slot's readback
		std::uint32_t portalQueriesIndex = 0, portalResultsIndex = 0;
	};
	static_assert(sizeof(FadeStateConstants) == 64);
	constexpr std::uint32_t kFadeStateConstantWords = sizeof(FadeStateConstants) / 4;
	constexpr std::uint32_t kFadeStateGroup = 64;
	constexpr std::uint32_t kInitialFadeRoots = 4096;
}
