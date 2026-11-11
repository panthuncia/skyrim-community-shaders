// Sky: the engine's BSSkyShader draws drawn by the render graph (CS_ORG_SKY=1, default off). The feature reserves the Sky anchors and
// installs the engine hooks; the rest is SkyCapture.cpp (the engine side) and SkyDraws.cpp (the graph side).

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"

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
			logger::info("[DCLF] sky: on (CS_ORG_SKY=1): the engine's sky batches are drawn at the Sky and SkyReflections anchors; cloud layers: main {}, faces {}",
				TakesClouds(View::Main) ? "the graph's" : "the engine's", TakesClouds(View::Face) ? "the graph's" : "the engine's");
		}

		std::string Status()
		{
			return SkyDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = Switches::On("CS_ORG_SKY");
		return enabled;
	}

	bool TakesClouds(View a_view)
	{
		// A feature choice, off unless asked for: docs/development/p5/sky.md, "Cloud layers".
		static const std::string clouds = Switches::Value("CS_ORG_SKY_CLOUDS");
		return clouds == "1" || (clouds == "main" && a_view == View::Main) || (clouds == "faces" && a_view == View::Face);
	}

	Api::Feature Feature()
	{
		return { .name = "sky", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
