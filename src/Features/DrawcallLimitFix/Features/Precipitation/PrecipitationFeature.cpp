// Precipitation: the engine's rain and snow drawn by the render graph in graph mode.
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
			InstallHooks();
			logger::info("[DCLF] precipitation: on (graph mode): the rain and snow are drawn at the EngineDraws anchor, each draw at its place");
		}

		std::string Status()
		{
			return PrecipitationDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "precipitation", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
