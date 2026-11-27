#pragma once

// Helpers shared by the shadow epoch files.

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPayload.h"

#include <cstdint>

namespace DCLF::Draws
{
	std::uint32_t EngineRasterStateId(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_bias, std::uint32_t a_scissor, std::uint32_t a_renderMode);
	void CaptureViewTarget(PendingView& a_view, std::uint32_t a_target);
	void CapturePerFrame(PendingView& a_view);

	ShadowViewLayout LayoutOf(const PendingView& a_view, std::uint32_t a_slot, std::uint32_t a_mode, std::uint32_t a_target);
}
