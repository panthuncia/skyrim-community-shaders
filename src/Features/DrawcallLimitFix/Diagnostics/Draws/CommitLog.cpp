// Main commit warnings and setup log, always on, on the render thread.

#include "DrawsState.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void LogFadeInputsVaried(const FadeInputsVaried& a_varied)
	{
		if (DrawsState::Get().fadeInputsVaried++ < 5)
			logger::info("[DCLF] the animation job's fade updates of frame {} had differing inputs; the first's stand for all", a_varied.frame);
	}

	void LogCullingSetup(const MainCullingSetup& a_setup)
	{
		auto& state = DrawsState::Get();
		const auto& shape = a_setup.shape;
		const auto& capture = a_setup.capture;
		auto* perFrameBuffer = capture.vsBuffers[kPerFrameVertexRegister];
		if (!std::exchange(state.cullingSetupLogged, true))
			logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", shape.cullMode, a_setup.hasViewProj ? "yes" : "no", kPerFrameVertexRegister,
				perFrameBuffer ? "bound" : "not bound");
		// Both epochs must rasterise identically for the colour EQUAL depth test.
		if (state.epochLines[a_setup.depthOnly ? 1 : 0]++ % 480 != 0)
			return;
		// The packed ViewProj's z row, which produces the compared depth.
		std::string viewProjZ = "(none)";
		std::span<const std::byte> perFrame;
		if (a_setup.replay)
			perFrame = a_setup.replayPerFrame;
		else if (perFrameBuffer)
			perFrame = ConstantMirror::Get().Contents(perFrameBuffer);
		if (perFrame.size() >= 48 * sizeof(float)) {
			const auto* floats = reinterpret_cast<const float*>(perFrame.data());
			viewProjZ = fmt::format("({:.6f} {:.6f} {:.6f} {:.6f})", floats[40], floats[41], floats[42], floats[43]);
		}
		logger::info("[DCLF] {} epoch: tables frame {} holding {} objects; render area {}x{}, depth range [{}, {}] (captured [{}, {}]), replay {}, eye ({:.2f} {:.2f} {:.2f}), ViewProj z row {}",
			a_setup.depthOnly ? "z-prepass" : "colour", a_setup.frame, a_setup.liveObjects, shape.width, shape.height, shape.minDepth, shape.maxDepth, capture.minDepth,
			capture.maxDepth, a_setup.replay ? "on" : "off", capture.eye.x, capture.eye.y, capture.eye.z, viewProjZ);
	}
}
