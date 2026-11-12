#pragma once

// The bindings and the lookups (CoordinatorBindings): the material slots' and the shared texture bindings, and the lookups they
// write with their resolve state and published copy. MaterialBindings.cpp, SharedBindings.cpp, SceneSet.cpp (ResolveLookups,
// PublishLookups).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorBindings
	{
		// The material slots' texture bindings (MaterialBindings; Coordinator/MaterialBindings.cpp).
		MaterialBindings materialBindings;
		// The shared, projected, shadow mask and shadow texture bindings (SharedBindings; Coordinator/SharedBindings.cpp).
		SharedBindings sharedBindings;
		// The lookups (Lookups.h): written by the bindings, resolved against the newest catalog before each commit and publication; each
		// publication carries an immutable copy, the render thread's only view.
		Lookups lookups;
		LookupsResolveState lookupsResolve;
		std::shared_ptr<const PipelineCatalog> lookupsCatalog;  // the catalog they were last resolved against
		// The last publication's lookups (shared again while unchanged: Lookups::ChangeKey) and catalog; chunk copies counted.
		std::shared_ptr<const Lookups> publishedLookups;
		std::shared_ptr<const PipelineCatalog> publishedCatalog;
		std::array<std::uint64_t, 6> publishedLookupsKey{};
		std::uint64_t lookupsChunkCopies = 0;
		// A load's texture imports, asked for from the attach records while the load is up (Coordinator::PrewarmLoadTextures), so the
		// bindings the tables make after it are answered from the import registry. The imports are kept until the load's hold ends.
		struct LoadPrewarm
		{
			GpuTextures::Replies replies;
			ankerl::unordered_dense::set<const void*> asked;
			std::vector<std::shared_ptr<const void>> held;
			std::uint64_t cookie = 1;
			std::uint64_t requested = 0, imported = 0, rejected = 0;
		};
		LoadPrewarm loadPrewarm;
	};
}
