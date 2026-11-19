// Sky: the engine's BSSkyShader draws drawn by the render graph (CS_ORG_SKY=1, default off).
// The engine side is SkyCapture.cpp and the graph side SkyDraws.cpp.

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::Sky
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
			SkyDraws::Reserve();
			InstallHooks();
			logger::info("[DCLF] sky: on (CS_ORG_SKY=1): the engine's sky batches and cloud layers are drawn at the Sky and SkyReflections anchors");
		}

		std::string Status()
		{
			return SkyDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::Native::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "sky", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
