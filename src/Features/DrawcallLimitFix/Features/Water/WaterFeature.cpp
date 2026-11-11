// Water: the engine's BSWaterShader draws drawn by the render graph (CS_ORG_WATER=1, default off). The feature reserves the EngineDraws
// anchor and installs the engine hooks; the rest is WaterCapture.cpp (the engine side) and WaterDraws.cpp (the graph side).

#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::Water
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
			WaterDraws::Reserve();
			InstallHooks();
			logger::info("[DCLF] water: on (CS_ORG_WATER=1): the engine's water batches are drawn at the EngineDraws anchor");
		}

		std::string Status()
		{
			return WaterDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = Switches::On("CS_ORG_WATER");
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "water", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
