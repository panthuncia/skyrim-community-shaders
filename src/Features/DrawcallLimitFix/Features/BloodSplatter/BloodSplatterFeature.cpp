// Blood splatter feature entry, off by default (CS_ORG_BLOOD_SPLATTER=1 turns it on).

#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"
#include "RenderGraph/Engine/Native/Native.h"

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
			// Must run before the render graph initializes, because the anchors' epochs are fixed then.
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
		static const bool enabled = ::RenderGraph::Engine::Native::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "blood splatter", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
