#pragma once

// The membership joins: the pass's membership passes, the bind queue and its evaluations, and the patches they make.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorJoins
	{
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedPasses;
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedLayerPasses;
		// Values only, applied without dereferencing a geometry, material, property or render pass.
		struct AccumulatePatch
		{
			std::uint32_t object = 0, material = 0, pipeline = 0, flags = 0;
			float fadeDistance = 0.0f;
			ObjectAncestorValue ancestorValue{};
			bool projectedUV = false, landBlend = false;
			std::uint64_t decalKey = 0;
		};
		/** @brief Pure. The merge stores its results in queue order. */
		struct JoinPre
		{
			enum class Kind : std::uint8_t
			{
				Skip,    // not a record to bind (free, shadow only, another entry's slot, not this walk's, verdict not None)
				Kept,    // a member whose binding stands (MemberBindingStands)
				Failed,  // no membership pass (PrimaryCull::LastSyntheticFail's reason in fail)
				Pass,    // a membership pass: joined by BuildAccumulatePhase
			};
			Kind kind = Kind::Skip;
			RE::BSGeometry* geometry = nullptr;
			bool layer = false;
			SceneCapture::LeafView leaf;
			MembershipDerived derived;  // the cache entry the pass leaves
			AccumulatedPass pass;
			std::uint8_t fail = 0;
		};
		/** @brief Pure. The derivation cache's slots are left to the merge. */
		struct JoinResult
		{
			enum class Kind : std::uint8_t
			{
				Skip,      // the mirror's property is not the held one (a swap not applied yet)
				Rejected,  // a verdict of this frame (the classification or ClassifyFrame) leaves it without bindings
				Joined,    // bound: the merge patches it
			};
			Kind kind = Kind::Skip;
			SceneCapture::LeafView leaf;
			RE::BSShaderProperty* property = nullptr;        // the entry's held property
			const RE::BSShaderMaterial* material = nullptr;  // the witness, the material slot's key
			std::uint8_t fadeState = 0;
			bool alphaBelowOne = false;
			std::uint32_t alphaState = 0;  // LeafView::AlphaState (0 for a layer: its draws apply no alpha property)
			std::uint32_t geometrySlot = 0;
			bool derivedWitness = false;  // the derivation cache's witnesses hold
			bool classified = false;      // descriptors, key and static flags are this join's
			Ineligible reason = Ineligible::None;
			LightingDescriptors descriptors;
			PipelineKey key{};
			std::uint32_t staticFlags = 0;
			bool ancestorSet = false;  // the ancestor value retaken (the ancestor map moved on: Light Limit Fix's room map)
			std::int32_t ancestorValue = -1;
		};
		std::vector<JoinPre> joinPres;
		std::vector<JoinResult> joinResults;
		// Bound by the joins and kept as residents until rewritten or released.
		std::vector<std::uint32_t> bindQueue;
		std::vector<std::uint8_t> materialMember;  // per material slot: a resident was bound to it (diagnostic)
		std::uint32_t membershipWitness = ~0u;  // PrimaryCull::MembershipWitness at the residents' binding
		std::vector<std::uint32_t> bindRetry;                // joins that waited for a material, bound by the next joins
	};
}
