#pragma once

// The sun and light candidates: the entries DCLF can take out of the engine's cascade and light culls, and their index spaces.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorCandidates
	{
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> lightDependents;  // by light entry
		ankerl::unordered_dense::set<const RE::NiAVObject*> lightCandidateSet;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> lightSignature;  // its geometries, as a set
		std::vector<const RE::NiAVObject*> lightEntriesDirty;
		std::shared_ptr<const EntrySpace> lightCandidates;
		std::uint32_t lightCandidatesGeneration = 0;
		std::uint64_t lightEntriesAppeared = 0;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunCandidateSet;
		// A signature change bumps the generation.
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> primarySignature;
		std::vector<const RE::NiAVObject*> sunEntriesDirty;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunEntriesForced;  // candidates whose subtree an attach or detach changed
		std::shared_ptr<const EntrySpace> sunCandidates;
		std::uint32_t sunCandidatesGeneration = 0;
		EntrySpaceTable sunTable, lightTable;
		std::uint32_t candidateVersions = 0;
		using LightEntryChange = DCLF::LightEntryChange;
		std::vector<LightEntryChange> lightEntryChanges;               // since the last publication
	};
}
