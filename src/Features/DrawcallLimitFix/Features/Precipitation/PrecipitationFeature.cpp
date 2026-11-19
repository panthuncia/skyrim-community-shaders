// Precipitation: the engine's rain and snow drawn by the render graph (CS_ORG_PRECIPITATION=1, default off).
// The engine side is PrecipitationCapture.cpp and the graph side PrecipitationDraws.cpp.

#include "Features/DrawcallLimitFix/Features/Precipitation/Precipitation.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::Precipitation
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
			PrecipitationDraws::Reserve();
			InstallHooks();
			logger::info("[DCLF] precipitation: on (CS_ORG_PRECIPITATION=1): the rain and snow are drawn at the EngineDraws anchor, each draw at its place");
		}

		std::string Status()
		{
			return PrecipitationDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::Native::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "precipitation", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
