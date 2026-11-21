// Policy's answers, derived from the run's Observation. Any thread, since Observe() is immutable.
#include "Policy.h"

#include "Observation.h"

namespace DCLF::Policy
{
	bool WithholdFromEngine()
	{
		// The capture check compares DCLF's draws with the engine's draws of the same objects.
		return !Diagnostics::Observe().engineDrawsAll;
	}

	bool ValidateTrackedSlice()
	{
		// Safety net for missed detaches.
		return Diagnostics::Observe().parity;
	}

	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass)
	{
		// The normal path trusts the reference counts.
		return Diagnostics::Observe().parity && (a_slotsFreed || Diagnostics::ParityDue(a_pass));
	}
}
