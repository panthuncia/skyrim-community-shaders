#include "RenderGraph/ShaderDebugInfo.h"

#include "Globals.h"
#include "RenderGraph/Switches.h"
#include "State.h"

namespace RenderGraph
{
	bool ShaderDebugInfo()
	{
		static const bool debug = [] {
			if (Switches::On("CS_DCLF_SHADER_DEBUG"))
				return true;
			if (Switches::Off("CS_DCLF_SHADER_DEBUG"))
				return false;
#if defined(CS_SHADER_DEBUG_BUILD)
			// Dev-Fast and Debug builds: with developer mode on.
			return globals::state && globals::state->IsDeveloperMode();
#else
			return false;
#endif
		}();
		return debug;
	}
}
