#pragma once

// The hidden events: actor entries by the nodes on their chain, and the hidden stores announced again at a later capture.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorHidden
	{
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> hiddenDependents;
		bool hiddenGating = false;
		bool hiddenWitness = false;
		struct BatchHidden
		{
			const void* key = nullptr;
			std::uint32_t site = 0;
			bool hidden = false;
			std::uint64_t sequence = 0;  // SceneCapture::NextSequence after the store
		};
		std::vector<BatchHidden> batchHidden;
		/** @brief Hidden events newer than the capture the pass judged by. They are announced again at the first pass with a later capture. Keys only. */
		ankerl::unordered_dense::map<const void*, std::uint64_t> hiddenAhead;
	};
}
