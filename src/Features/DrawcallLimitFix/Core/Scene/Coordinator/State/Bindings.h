#pragma once

// The material and shared texture bindings, and the lookups they write with their resolve state and published copy.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorBindings
	{
		MaterialBindings materialBindings;
		SharedBindings sharedBindings;
		// Resolved against the newest catalog before each commit and publication. A publication's copy is the render thread's only view.
		Lookups lookups;
		LookupsResolveState lookupsResolve;
		std::shared_ptr<const PipelineCatalog> lookupsCatalog;  // the catalog they were last resolved against
		std::shared_ptr<const Lookups> publishedLookups;
		std::shared_ptr<const PipelineCatalog> publishedCatalog;
		std::array<std::uint64_t, 6> publishedLookupsKey{};
		std::uint64_t lookupsChunkCopies = 0;
		// Texture imports requested during a load, so the bindings made after it are answered from the import registry.
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
