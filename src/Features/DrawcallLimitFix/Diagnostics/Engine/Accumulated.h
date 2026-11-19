#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Diagnostics::Accumulated
{
	/**
	 * @brief Engine side of CS_DCLF_ACCUMULATED_PARITY. DCLF writes kAccumulated, NiAVObject flag bit 26, on stood-in roots at
	 * Main::Update's entry. "<- ACCUMULATED" compares it exactly with the bit the engine's cull leaves, sampled when the list jobs end.
	 * "<- ANIM RATE" detours the animation gate FUN_1402cff60 at its three calls and checks each stood-in root against DCLF's bit.
	 * "<- FRAME CAMERA" checks the list processes' camera against the one BeginFrameCamera read.
	 */
	void Install();

	/** @brief A frame's sample. Per slot of the frame's tables, the node or null, and its kSample* bits. */
	struct Sample
	{
		std::uint32_t frame = 0;
		std::vector<const void*> rootNodes;       // the listed roots' nodes (Extensions::RootListOf)
		std::vector<std::uint8_t> rootBits;
		std::vector<const void*> objectGeometry;  // a layer's is null and its base slot's geometry is sampled
		std::vector<std::uint8_t> objectBits;
	};
	inline constexpr std::uint8_t kSampleAccumulated = 1u << 0;      // the node's kAccumulated
	inline constexpr std::uint8_t kSampleStoodIn = 1u << 1;          // a root the engine's cull did not visit, or a geometry under one
	inline constexpr std::uint8_t kSampleRootAccumulated = 1u << 2;  // a geometry's fade root's kAccumulated, set when there is no root
	inline constexpr std::uint8_t kSampleHidden = 1u << 3;           // a root that is app-culled, which the engine's cull does not reach

	/** @brief The samples up to a_through, in frame order. Render thread, at the frame-start wake. */
	std::vector<std::unique_ptr<Sample>> TakeSamples(std::uint32_t a_through);
	std::uint64_t DropSamples(std::uint32_t a_oldestPending);

	/** @brief The animation gate's three call sites, named. */
	extern const std::array<const char*, 3> kAnimGateCallers;
	/** @brief ANIM RATE counts since the last Take. */
	struct AnimRate
	{
		std::uint64_t predictions = 0, roots = 0, set = 0;
		struct Site
		{
			std::uint64_t calls = 0, neverFade = 0, fading = 0, rejected = 0, rejectedNeverFade = 0, predictedOut = 0, acceptedOut = 0, fadedOut = 0, differ = 0;
			const void* firstDiffer = nullptr;
		};
		std::array<Site, 3> sites{};
	};
	AnimRate TakeAnimRate();

	/** @brief FRAME CAMERA counts since the last Take. Frames checked and those whose list processes carried another camera. */
	struct FrameCamera
	{
		std::uint64_t checked = 0, differ = 0;
	};
	FrameCamera TakeFrameCamera();
}
