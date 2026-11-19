#pragma once

// The category nodes the tracked content hangs under, the portal graphs' roots and the CategoryRoot views' roots.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorCategories
	{
		std::shared_ptr<const CategoryCapture> categoryTaken;
		std::uint64_t categoryPinsGeneration = 0;  // the newest capture whose pins went into a batch (read once: the render thread drops them after)
		std::uint64_t categorySignature = 0;
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		// Portal graphs' parentless roots, each filed under its graph's shared portal node and held while listed. Unheld roots are held by key alone.
		struct AlwaysRenderRoot
		{
			RE::NiPointer<RE::NiAVObject> root;  // null for an unheld root
			RE::NiNode* category = nullptr;
			RE::NiAVObject* object = nullptr;  // the root (a key for an unheld one)
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, AlwaysRenderRoot> alwaysRenderRoots;
		// The first-person root's kHidden is view-scoped, since Main::Draw shows it only for first person. ClassifyFrame stops at it
		// before the hidden test, and its hidden events announce nothing.
		std::array<const RE::NiNode*, Extensions::kEngineViews> viewRoots{};
		ankerl::unordered_dense::map<const RE::NiNode*, std::pair<std::uint32_t, std::uint8_t>> categoryFound;
		CoordinatorTracked::TrackSource addSource = CoordinatorTracked::TrackSource::AttachEvent;
		std::size_t validationCursor = 0;
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> publishedCategoryNodes;
		bool nodeSetsDirty = true;  // the category nodes changed
	};
}
