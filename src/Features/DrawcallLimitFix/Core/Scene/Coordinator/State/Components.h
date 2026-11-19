#pragma once

// The features' coordinator components: their states and seed requests, the root marks and the commit's vetoes.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorComponents
	{
		// Made before the first hook runs.
		std::vector<std::shared_ptr<void>> components;
		std::vector<std::vector<FadeSeedItem>> componentSeedRequests;
		std::vector<std::vector<std::shared_ptr<const void>>> componentDeltaRequests;
		// Written only while the mirror's events are applied, never while adds prepare, since the parallel prepares read it as data.
		struct RootMark
		{
			Api::Mark mark;
			bool ownHide = false;
		};
		ankerl::unordered_dense::map<const void*, RootMark> rootMarks;

		std::uint32_t vetoHeld = 0, vetoObserved = 0;
		std::vector<Api::ObservedState> observedSlots;
		std::vector<std::uint32_t> requeued;
		std::vector<std::uint8_t> componentCommitLive;
		// During the settle, a veto dropped from Observe applies the observed phases and a Hold applies none.
		bool settling = false;
		std::vector<std::pair<std::uint32_t, std::uint8_t>> settleApply;
	};
}
