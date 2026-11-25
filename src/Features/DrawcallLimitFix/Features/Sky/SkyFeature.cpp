// Sky: the engine's BSSkyShader draws drawn by the render graph in graph mode.
// The engine side is SkyCapture.cpp and the graph side SkyDraws.cpp.

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Features/Sky/SkyNative.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::Sky
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			if (!Enabled())
				return;
			Native::RegisterSwitch(a_context);
			InstallHooks();
			logger::info("[DCLF] sky: on (graph mode): the engine's sky batches and cloud layers are drawn at the Sky and SkyReflections anchors");
		}

		std::string Status()
		{
			auto status = SkyDraws::Status();
			if (const auto native = Native::Status(); !native.empty())
				status += "; " + native;
			return status;
		}
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
