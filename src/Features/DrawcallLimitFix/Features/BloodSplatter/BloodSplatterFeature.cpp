// Blood splatter feature entry. On in graph mode.

#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"

#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::BloodSplatter
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			InstallHooks();
			logger::info("[DCLF] blood splatter: on (graph mode): the screen blood is drawn at the EngineDraws anchor");
		}

		std::string Status()
		{
			return BloodSplatterDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "blood splatter", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
