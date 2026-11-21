#pragma once

// The water reflection's own frame state: this update's captured faces, the cube and the report counters. Render thread.
// Included by the feature's sources.

// ViewEpoch.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Features/Reflection/Reflection.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DCLF::Features::Reflection
{
	inline constexpr Api::EngineView kView = Api::EngineView::Reflection;

	/** @brief The view's frame state. */
	struct ReflectionFrame
	{
		std::uint32_t lodSlots = 0, programsReady = 0, pipelinesReady = 0;
		// This update's faces (FaceOriented), until Execute takes them.
		struct Face
		{
			bool captured = false;
			std::array<std::byte, Draws::kViewBlockBytes> perFrame{};
			std::uint32_t perFrameBytes = 0;
			std::array<float, 16> viewProj{};
			RE::NiPoint3 eye;
		};
		std::array<Face, kFaces> faces;
		winrt::com_ptr<ID3D11Texture2D> cube;  // the cube target the faces render into
		std::uint32_t width = 0, height = 0;
		// Since the last report.
		std::uint32_t facesCaptured = 0, updates = 0, epochs = 0, facesDrawn = 0;
		std::array<std::uint32_t, 6> skipped{};  // not drawable, not covered (DecideCoverage), no faces, stale inputs, no resources, the epoch failed
		std::array<std::uint64_t, 2> nullTextures{};  // the frame texture registers the epochs' frame records left null (IndirectDraws::SubmitViewEpoch)
		// The last Execute's frame, and faces captured after it that frame (expected none).
		std::uint32_t executedFrame = ~0u, lateFaces = 0;
	};

	/** @brief The view's frame state. Render thread. The report reads and resets its counters. */
	inline ReflectionFrame& FrameState()
	{
		static ReflectionFrame frame;
		return frame;
	}
}
