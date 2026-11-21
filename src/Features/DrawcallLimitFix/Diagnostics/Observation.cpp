// The run's Observation, read from the switches once on first use.
#include "Observation.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		Observation Read()
		{
			Observation o;
			o.parity = SwitchEnabled(Switch::Parity);
			o.capture = SwitchEnabled(Switch::CaptureParity);
			o.engineDrawsAll = SwitchEnabled(Switch::ParityBoth);
			o.stats = SwitchEnabled(Switch::Stats);
			o.passStats = SwitchEnabled(Switch::PassStats);
			o.profile = SwitchEnabled(Switch::Profile);
			o.releaseGuard = SwitchEnabled(Switch::ReleaseGuard);
			return o;
		}
	}

	const Observation& Observe()
	{
		static const Observation observation = Read();
		return observation;
	}
}
