// Forward feature registration: object classes, hooks and report lines.

#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"

namespace DCLF::Features::Forward
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Blended Lighting geometry, sorted back to front on the GPU, drawn just before the engine's own blended draws.
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Forward });
			// Refraction geometry's only main pass is a Utility RENDER_NORMAL pass into kREFRACTION_NORMALS.
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Refraction });
			InstallHooks(a_context);
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "forward", .install = &Install, .preflight = &PreflightHooks };
	}
}
