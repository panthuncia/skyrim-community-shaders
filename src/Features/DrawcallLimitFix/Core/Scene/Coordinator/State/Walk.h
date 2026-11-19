#pragma once

// The walk: the frame state both halves read, the per-frame set, the evaluation rounds' results and counters, and the fan-out.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"

namespace DCLF::Scene
{
	struct CoordinatorWalk
	{
		// Per-frame entries are walked once per frame input.
		std::uint32_t perFrameFrame = ~0u;
		bool perFrameWalked = true;
		// Only for CS_DCLF_WALK_PARITY's dense rebuild.
		bool denseWalk = false;
		ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible> referenceReasons;
		// Read once by the walk and reused by the joins, so both halves agree.
		bool sceneBuilt = false;  // the walk ran: the records are this frame's
		bool frameResolveBuffers = false;
		bool frameInterior = false;
		std::array<std::uint32_t, 4> frameDecalBias{};
		bool graphWasActive = false;  // the previous BuildFrame's resolveBuffers, to log the flip
		enum PerFrameTrait : std::uint32_t
		{
			kTraitEntryClass = 1u << 0,  // a per-frame entry class's entry (a face shape; the report's "face")
			kTraitOwnerClass = 1u << 1,  // a per-frame owner class's entry (EntryClassContribution::everyFrame; the report's "actor")
			// 1u << 2 is unused but the report still names the bits.
			kTraitSkin = 1u << 3,
			kTraitAnimatedShading = 1u << 4,  // a controller on its shader or alpha property: its shading is named by events, never per frame
			kTraitMoves = 1u << 5,            // a controller or a non-fixed rigid body on its chain, or a geometry type contribution's mover (its toggle on)
			kTraitRootMoves = 1u << 6,        // its reference root's subtree moves (RootMoves)
		};
		static constexpr std::size_t kFanoutGrain = 64;
		static constexpr std::size_t kFanoutMinEntries = 128;
		/** @brief A chunk's counters, merged in chunk order so first-named strings match the serial walk. */
		struct EvalCounters
		{
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			std::array<std::uint32_t, 64> techniqueRejects{};
			std::uint32_t classifyHits = 0, classifyChecked = 0, classifyDiffers = 0;
			std::string classifyDiffersFirst;
			std::uint64_t verdictsChecked = 0, verdictsSkipped = 0, verdictsMissed = 0;
			std::string firstVerdictMissed;
			std::uint64_t leafMissing = 0;  // leafStats.missing
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};  // the chunk's PartTimer (CS_DCLF_PROFILE)
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			CoordinatorMirror::MirrorReadStats mirror;  // mirrorReadSink
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;  // RootMovesNow's answers
		};
		struct TrackedDelta
		{
			bool entryClassSet = false;                        // resolved once
			Api::EntryClass entryClass = Api::EntryClass::None;
			bool ownerSet = false;                             // resolved once
			Extensions::OwnerClassRule owner;
			bool verdictSet = false;
			CoordinatorTracked::Tracked::StaticVerdict verdict;
			bool candidateSet = false;                         // classified, or its frame verdict changed
			std::uint32_t candidateFrame = 0;
			Ineligible candidateReason = Ineligible::None;
		};
		/** @brief Reads only the frozen mirror, the entry's own Tracked and the pass snapshot. Never a table, slot or other entry. */
		struct EvalResult
		{
			enum class Write : std::uint8_t
			{
				None,      // kept by the light path
				NoLeaf,    // leafWaiting
				Rejected,  // its verdict gives it no record
				Record,    // the merge resolves the face, the geometry and the slots
			};
			EvaluateKind kind = EvaluateKind::Event;
			bool light = false;
			bool lightRecorded = false;
			bool lightSkin = false;
			std::uint32_t skinRows = 0;
			std::uint16_t skinPartitions = 0;
			std::uint32_t skinLodPartitions = 0;
			bool verdictUnannounced = false;  // the frame verdict changed with no hidden event
			Write write = Write::None;
			TrackedDelta delta;
			SceneCapture::LeafView leaf;
			Ineligible reason = Ineligible::None;  // its bucket
			bool shadowOnly = false;
			bool focusOnly = false;  // with shadowOnly: a hidden actor part, drawn only by focus views (kObjectFocusOnly)
			bool entryTaken = false;       // a taken entry class's: its snapshot is the merge's (ComponentsResolve)
			bool referenceReason = false;  // the dense walk's: reason into referenceReasons
			std::uint16_t partitionMask = 0;
			bool lodChain = false;
			bool skinCandidate = false;  // in this walk's skinnedObjects
			std::uint32_t boneRows = 0;  // 0 without a palette (or past 240)
			std::uint32_t particleRows = 0;  // a particle member's block of the frame's palettes (its palette rows class's rule)
			std::uint8_t shadowReject = 0;
			std::uint32_t shadowTechnique = 0;
			const RE::BSShaderMaterial* shadowMaterial = nullptr;  // the alpha-tested techniques' diffuse
			ID3D11ShaderResourceView* shadowDiffuse = nullptr;
			std::array<std::uint32_t, kOcclusionViews> occlusion{};
			std::uint32_t skinLodWord = 0;  // SkinLodPartitionsOf (0 for a LOD chain)
			bool classified = false;
			std::uint32_t traits = 0;
			bool mayRecord = false;
			const RE::NiAVObject* rootCandidate = nullptr;
			bool rootMovesNow = false;
			const void* moveKey = nullptr;
			const void* moveKeyOuter = nullptr;
		};
		/** @brief What a pass's evaluations read in place of the live toggles and classify inputs, bound over the whole pass. */
		struct PassSnapshot
		{
			ToggleSet toggles;
			Extensions::ClassifyInputSet inputs;
		};
		PassSnapshot passSnapshot;
		std::shared_ptr<const FrameGlobals> passGlobals;  // the frame globals the pass binds, on the pool too
		std::vector<EvalResult> evalResults;
		std::vector<EvalCounters> evalCounters;
		struct FanoutStats
		{
			std::uint64_t rounds = 0, parallelRounds = 0, entries = 0, parallelEntries = 0, chunks = 0;
			std::uint64_t joinRounds = 0, joinParallelRounds = 0, joinEntries = 0;
			std::uint64_t lightMissed = 0;  // light-path entries rewritten in full at the merge
			std::uint64_t subtreeRounds = 0, subtreeParallelRounds = 0, subtreeRoots = 0, subtreeParallelRoots = 0, subtreeGeometries = 0;
			double subtreePrepareMs = 0.0, subtreeMergeMs = 0.0;
		} fanoutStats;
		bool fullEvaluation = true;  // a reset, a load or a live toggle: the next delta walk evaluates everything
		std::uint32_t walkSerial = 0;
		/** @brief The per-frame entries with their addresses, trusted only while trackedLayout equals perFrameLayout. */
		struct PerFrameItem
		{
			RE::BSGeometry* geometry = nullptr;
			CoordinatorTracked::Tracked* tracked = nullptr;
		};
		std::vector<PerFrameItem> perFrameSet;
		std::uint64_t trackedLayout = 0;
		std::uint64_t perFrameLayout = ~0ull;
		std::uint32_t validatedFrame = ~0u;  // the pass that ran the validation slice: once a pass
		std::vector<RE::BSGeometry*> pendingEvaluation;
		std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> buckets{};
		struct DeltaStats
		{
			std::uint32_t walks = 0, full = 0;
			std::uint64_t evaluated = 0, perFrame = 0, pending = 0, property = 0, node = 0, roots = 0, geometryDirty = 0, restored = 0, moved = 0, kept = 0;
			std::uint64_t propertyEvents = 0, nodeEvents = 0;
			std::uint64_t switchEvents = 0, switchChanges = 0, switchCatchUps = 0, switchReclassified = 0, attachCatchUps = 0;
			std::uint64_t live = 0;
			std::uint32_t evaluatedMax = 0;
		} delta;
		std::vector<RE::BSGeometry*> skinnedObjects;  // this walk's, in object order
	};
}
