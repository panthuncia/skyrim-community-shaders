// Sky: the engine's BSSkyShader draws drawn by the render graph in graph mode.
// The engine side is SkyHooks.cpp and SkyNative.cpp, the graph side SkyDraws.cpp.

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Features/Sky/SkyNative.h"
#include "RenderGraph/Engine/Native/Native.h"

#include <string>

namespace DCLF::Features::Sky
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			InstallHooks();
			// Native's main view and faces cull the sky root from the frame ticket.
			Native::RegisterRootCapture();
			logger::info("[DCLF] sky: on (graph mode): the engine's sky batches and cloud layers are drawn at the Sky and SkyReflections anchors");
		}

		std::string Status() { return SkyDraws::Status(); }
	}

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "sky", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
