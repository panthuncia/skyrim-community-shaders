// Blood splatter: the engine's screen blood drawn by the render graph (CS_ORG_BLOOD_SPLATTER=1, default off). The feature reserves the
// EngineDraws anchor and installs the engine hooks; the rest is BloodSplatterCapture.cpp and BloodSplatterDraws.cpp.

#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"

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
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
			BloodSplatterDraws::Reserve();
			InstallHooks();
			logger::info("[DCLF] blood splatter: on (CS_ORG_BLOOD_SPLATTER=1): the screen blood is drawn at the EngineDraws anchor");
		}

		std::string Status()
		{
			return BloodSplatterDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = Switches::On("CS_ORG_BLOOD_SPLATTER");
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "blood splatter", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
