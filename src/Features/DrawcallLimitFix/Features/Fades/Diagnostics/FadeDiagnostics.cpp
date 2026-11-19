#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Features/Fades/Diagnostics/FadeDiagnostics.h"

namespace DCLF::Features::Fades
{
	void RegisterDiagnostics(Api::InstallContext& a_context)
	{
		RegisterReport(a_context);
		// The fade log's readback must run after the other main-epoch parity readbacks, which Diagnostics installs earlier.
		if (FadeParity()) {
			SubscribePortParity();
			SubscribeFadeLog();
		}
	}
}
