#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Features/Fades/Diagnostics/FadeDiagnostics.h"

namespace DCLF::Features::Fades
{
	void RegisterDiagnostics(Api::InstallContext& a_context)
	{
		RegisterReport(a_context);
	}
}
