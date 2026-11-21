// Publishing flips on the scene lane, and releasing them on the render thread when a frame installs them.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"

namespace DCLF::Features::Lod::Gates
{
	namespace
	{
		ReleaseStats releaseStats;  // render thread
	}

	ReleaseStats& Releases() { return releaseStats; }

	void Adopt(const Api::AdoptionContext& a_context)
	{
		auto& rs = releaseStats;
		if (a_context.deltas.empty())
			return;
		rs.any = true;
		// Null before DCLF's first covered frame.
		const void* claims = a_context.claims;
		const std::uint64_t now = GateNowNs();
		for (const auto& delta : a_context.deltas) {
			const auto& flip = *static_cast<const GateFlip*>(delta.get());
			const bool released = LodGates::Flip(flip.token);
			if (!claims)
				++rs.beforeDrawing;
			rs.flipToReleaseMs.push_back(GateMs(flip.flipNs, now));
			rs.requestToReleaseMs.push_back(GateMs(flip.requestNs, now));
			rs.flipToReleaseFrames.push_back(a_context.frame >= flip.flipFrame ? a_context.frame - flip.flipFrame : 0u);
			if (released)
				++rs.released;
			else
				++rs.refused;
		}
		// The coordinator must pick up the outcomes the flips posted.
		Extensions::WakeChannel(channel);
	}
}
