#pragma once

// The coordinator's tracked set: each tracked geometry's entry with its caches and list keys, the tracked map, the scene identity
// and the walk's and joins' orders. Every phase reads it.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorTracked
	{
		enum class TrackSource : std::uint8_t
		{
			AttachEvent,       // an attach event's subtree walk
			CategoryAppeared,  // the walk of a category node RefreshCategoryNodes found new
		};
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			// Held from the naming event's pins. The scene work's references are copies of these.
			RE::NiPointer<RE::BSShaderProperty> property, layerProperty;
			RE::BSShaderProperty* HeldProperty(bool a_layer) const { return (a_layer ? layerProperty : property).get(); }
			// Set when written while the mirror had no record. Evaluated again when a capture names it.
			bool leafWaiting = false;
			std::uint64_t identity = 0;
			std::uint64_t groupIdentity = 0;
			const RE::TESObjectREFR* ownerReference = nullptr;  // its owner class's reference; lookup only, never published
			const void* ancestorKey = nullptr;  // the ancestor value's lookup key (AncestorKeyOf), refreshed on structural attachment
			std::uint64_t ancestorMapGeneration = 0;
			std::int32_t ancestorValue = -1;
			RE::NiNode* categoryNode = nullptr;
			Ineligible parentReason = Ineligible::None;
			std::uint32_t trackedFrame = 0;
			TrackSource trackedBy = TrackSource::AttachEvent;

			/**
			 * @brief A cached negative verdict, checked in full against its witnesses each evaluation. A stale negative would leave an object
			 * native forever. ValidateSlice covers what has no cheap witness.
			 */
			struct StaticVerdict
			{
				bool cached = false;
				Ineligible reason = Ineligible::None;
				const RE::BSGraphics::TriShape* rendererData = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
			};
			StaticVerdict verdict;

			/** @brief The cached positive derivation. Slots are checked against their keys before use, so a reused slot is never handed back. */
			struct Derived
			{
				bool valid = false;
				std::uint32_t generation = 0;  // the slot tables' generation the slots belong to
				const RE::BSGraphics::TriShape* triShape = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
				bool interior = false;
				bool alphaBelowOne = false;
				std::uint32_t alphaState = 0;  // the alpha's flags and threshold (LeafView::AlphaState): the static flags' alpha test bits
				std::uint32_t technique = 0;
				std::uint32_t subPass = 0;
				std::uint32_t hint = 0;
				std::uint32_t biasWitness = 0;
				LightingDescriptors descriptors;
				std::uint32_t staticFlags = 0;
				PipelineKey key{};
				std::uint32_t geometrySlot = ~0u;
				std::uint32_t pipelineSlot = ~0u;
				std::uint32_t materialSlot = ~0u;
			};
			Derived derived;

			/** @brief The cull-only verdict. It stands until a delta walk event retakes it. */
			std::uint32_t candidateFrame = 0;  // the pass that classified it (PassStamp); 0: never classified
			Ineligible candidateReason = Ineligible::None;
			// Resolved once by the walk.
			Api::EntryClass entryClass = Api::EntryClass::None;
			bool entryClassResolved = false;
			// Its component's next event rewrites it.
			bool streamWaiting = false;
			RE::NiPointer<RE::NiNode> sourceRef;
			// Fade-root owners are copied from this reference, never made from a key.
			RE::NiPointer<RE::NiAVObject> fadeNodeRef;
			const void* fadeNodeRequested = nullptr;  // the fade node key asked of the render thread (FadeNodeRequest), until answered
			// Read by the commit as data.
			std::uint8_t veto = 0;
			// Separate from entryClass, since a face shape is both. Resolved once by the walk.
			Extensions::OwnerClassRule owner;
			bool ownerResolved = false;
			Ineligible accumulateReason = Ineligible::None;
			std::uint32_t accumulateReasonFrame = 0;  // the frame it was taken
			std::uint64_t accumulateReasonPass = 0;   // the pass (a join failed for a verdict of this pass)
			// Valid while objectStamp equals the coordinator's.
			std::uint32_t objectStamp = 0;
			std::uint32_t objectId = 0;
			std::uint32_t slot = kNoObjectSlot;
			// The node whose bound decides sun caster candidacy, resolved once since the graph above a tracked geometry is fixed.
			// Kept apart from listedRoot, which is cleared when the entry is unlisted.
			const RE::NiAVObject* entryRoot = nullptr;
			bool sunEntryResolved = false;
			const RE::NiAVObject* lightRoot = nullptr;
			// Lives and dies with the base.
			std::uint32_t layerSlot = kNoObjectSlot;
			Derived layerDerived;
			const void* listedLayerProperty = nullptr;  // the layer's property in propertyDependents
			MembershipDerived membershipDerived, membershipLayerDerived;
			// perFrame means its inputs change every frame. It is set by a classifying evaluation and kept.
			bool perFrame = false;
			bool perFrameListed = false;
			// Why it is per frame: it moves, is skinned or is an actor's. While its classification stands the light path retakes only an
			// actor's verdict and a skin's partitions, and only when an event names it.
			std::uint32_t lightTraits = 0;
			std::uint32_t movedWalk = 0;
			// With several references on its chain, the innermost is here and the outermost in moveKeyOuter. With none, its category node.
			const void* moveKey = nullptr;
			const void* moveKeyOuter = nullptr;
			std::uint32_t gateWitnessWalk = 0;
			// Owed to the next per-frame walk, since the event walk skips per-frame entries. Otherwise the first-person switch's hide
			// of the third-person skeleton would be lost.
			std::vector<const RE::NiAVObject*> hiddenChain;
			bool hiddenEventOwed = false;
			const void* listedProperty = nullptr;
			const void* listedAlpha = nullptr;
			const void* listedEmittance = nullptr;  // external emittance's shared colour (emittanceEvents)
			const void* listedFadeNode = nullptr;   // its property's fade node: a blended fade starting or ending (BlendFading)
			const RE::NiAVObject* listedRoot = nullptr;
			std::uint32_t fullWalk = 0;  // the walk that must write it in full (an event, not the per-frame set)
			std::uint8_t bucket = kNoBucket;
			std::uint32_t scheduledWalk = 0;
			static constexpr std::uint8_t kNoBucket = 0xFF;
		};
		ankerl::unordered_dense::map<RE::BSGeometry*, Tracked> tracked;
		SceneIdentity sceneIdentity;
		struct OrderEntry
		{
			RE::BSGeometry* geometry;
			Tracked* tracked;  // mutable: BuildFrame updates the cached verdict through it
			const AccumulatedPass* accumulated;
			bool layer = false;  // the joins': the entry's layer (Tracked::layerSlot) rather than its base
		};
		std::vector<OrderEntry> order;
		std::vector<OrderEntry> accumulateOrder;
	};
}
