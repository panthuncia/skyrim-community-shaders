// MenuViews: the menu scene renders as DCLF views (CS_ORG_MENU3D=1, CS_ORG_MAP=1, default off). The feature installs the probe's hooks
// and its report lines; the rest is MenuViewsProbe.cpp.

#include "Features/DrawcallLimitFix/Features/MenuViews/MenuViews.h"

#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::MenuViews
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		void Install(Api::InstallContext& a_context)
		{
			if (!Menu3DEnabled() && !MapEnabled())
				return;
			InstallHooks();
			a_context.RegisterReportLines({ .name = "menu views", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			logger::info("[DCLF] menu views: probe on (CS_ORG_MENU3D={}, CS_ORG_MAP={}); the engine still draws every menu scene", Menu3DEnabled() ? 1 : 0,
				MapEnabled() ? 1 : 0);
		}
	}

	bool Menu3DEnabled()
	{
		static const bool enabled = Switches::On("CS_ORG_MENU3D");
		return enabled;
	}

	bool MapEnabled()
	{
		static const bool enabled = Switches::On("CS_ORG_MAP");
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "menu views", .install = &Install, .preflight = &PreflightHooks };
	}
}
