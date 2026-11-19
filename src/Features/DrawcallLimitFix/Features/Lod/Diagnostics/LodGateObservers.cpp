// Parity observers for the LOD gates. A flip's incoming geometries must be claimed with the flip's phases and no outgoing
// geometry may be claimed. Checked in the flip's own publication and again in the claims of the frame that releases it.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Features/Lod/Diagnostics/LodGateEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"

namespace DCLF::Features::Lod
{
	namespace
	{
		using V = Diagnostics::SceneView;
		namespace G = Gates;

		void OnFlipsPublished(const GateFlipsPublished&)
		{
			// A hole here comes from the commit. A hole seen only at the release comes from the hand-over.
			const auto& store = Scene::Coordinator::Get();
			auto* gates = G::StateOf(V::componentState(store, G::component));
			if (!gates)
				return;
			auto& g = gates->observed;
			const auto* claims = V::publicationClaims(store);
			for (const auto& flip : G::Flips(V::componentDeltas(store, G::component)))
				for (const auto& [geometry, phases] : flip.incoming)
					if (const std::uint8_t claimed = claims ? claims->PhasesOf(geometry) : std::uint8_t{ 0 }; claimed != phases) {
						++g.publishHoles;
						if (g.firstPublishHole.empty())
							g.firstPublishHole = fmt::format("gate {}: geometry {} claimed {:#x}, flipped {:#x}", flip.token, static_cast<const void*>(geometry), claimed, phases);
					}
		}

		void OnReleasing(const GateReleasing& a_event)
		{
			// A later publication's legitimate change to an incoming member also reads as a hole here.
			auto& rs = G::Releases().observed;
			++rs.checked;
			for (const auto& [geometry, phases] : a_event.incoming)
				if (const std::uint8_t installedPhases = V::ClaimedPhases(a_event.claims, geometry); installedPhases != phases) {
					// The engine already moved on from a refused flip, so this is counted apart and not as a hole.
					if (!a_event.released) {
						++rs.refusedHoles;
						continue;
					}
					++rs.holes;
					if (rs.firstHole.empty())
						rs.firstHole = fmt::format("gate {}: geometry {} claimed {:#x}, flipped {:#x}", a_event.token, static_cast<const void*>(geometry), installedPhases, phases);
				}
			for (const auto* geometry : a_event.outgoing)
				if (const std::uint8_t installedPhases = V::ClaimedPhases(a_event.claims, geometry)) {
					++rs.coverage;
					if (rs.firstCoverage.empty())
						rs.firstCoverage = fmt::format("gate {}: geometry {} claimed {:#x}", a_event.token, static_cast<const void*>(geometry), installedPhases);
				}
		}
	}

	void Gates::SubscribeGateObservers()
	{
		Diagnostics::Subscribe<GateFlipsPublished>(OnFlipsPublished);
		Diagnostics::Subscribe<GateReleasing>(OnReleasing);
	}
}
