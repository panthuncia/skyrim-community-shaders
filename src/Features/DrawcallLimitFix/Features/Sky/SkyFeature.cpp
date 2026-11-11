// Sky: the engine's BSSkyShader draws drawn by the render graph (CS_ORG_SKY=1, default off). The feature reserves the Sky anchors and
// installs the engine hooks; the rest is SkyCapture.cpp (the engine side) and SkyDraws.cpp (the graph side).

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"

#include "Features/CloudShadows/ORGCloudShadows.h"
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
			// The faces' clouds write Cloud Shadows' occlusion into its graph faces, which it makes at its setup (after this).
			if (TakesView(View::Face) && TakesClouds(View::Face))
				ORGCloudShadows::RequestGraphFaces();
			InstallHooks();
			logger::info("[DCLF] sky: on (CS_ORG_SKY=1): the engine's sky batches are drawn at the Sky and SkyReflections anchors");
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

	bool TakesView(View a_view)
	{
		static const std::string views = Switches::Value("CS_ORG_SKY_VIEWS");
		return !(views == "main" && a_view != View::Main) && !(views == "faces" && a_view != View::Face);
	}

	bool TakesClouds(View a_view)
	{
		// Off unless asked for: the graph's clouds do not match the engine's yet (docs/development/p5/sky.md, "Open").
		static const std::string clouds = Switches::Value("CS_ORG_SKY_CLOUDS");
		return clouds == "1" || (clouds == "main" && a_view == View::Main) || (clouds == "faces" && a_view == View::Face);
	}

	Api::Feature Feature()
	{
		return { .name = "sky", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
