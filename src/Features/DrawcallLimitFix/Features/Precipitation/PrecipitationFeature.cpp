// Precipitation: the engine's rain and snow drawn by the render graph (CS_ORG_PRECIPITATION=1, default off). The feature reserves the
// EngineDraws anchor and installs the engine hook; the rest is PrecipitationCapture.cpp and PrecipitationDraws.cpp.

#include "Features/DrawcallLimitFix/Features/Precipitation/Precipitation.h"

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
		static const bool enabled = Switches::On("CS_ORG_PRECIPITATION");
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "precipitation", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
