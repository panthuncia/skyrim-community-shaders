#pragma once

// The scene mirror and its reads: the mirror, the read counters, and the batch's pins.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorMirror
	{
		enum class MirrorRead : std::uint8_t
		{
			CategoryNode,
			Subtree,
			SwitchEvent,
			NodeEvent,
			HiddenChain,
			MoveKey,
			LightEntry,
			SunEntry,
			Reference,  // a geometry the mirror's walk found with no pin
			kCount
		};
		static constexpr std::array<const char*, static_cast<std::size_t>(MirrorRead::kCount)> kMirrorReadNames{ "category node", "subtree", "switch event", "node event",
			"hidden chain", "move key", "light entry", "sun entry", "unpinned geometry" };
		struct MirrorReadStats
		{
			std::array<std::uint64_t, static_cast<std::size_t>(MirrorRead::kCount)> reads{}, unmirrored{};
		};
		mutable MirrorReadStats mirrorReads;
		static inline thread_local MirrorReadStats* mirrorReadSink = nullptr;
		std::vector<SceneMirror::Prepared> mirrorPreps;
		static constexpr std::size_t kMirrorFanoutRecords = 256;
		/** @brief The applied batch's pins by key, valid while it is applied. */
		ankerl::unordered_dense::map<const void*, RE::NiRefObject*> batchPins;
		struct ReferenceStats
		{
			std::uint64_t pins = 0, pinned = 0, live = 0, refused = 0;
			std::uint64_t properties = 0, propertiesRefused = 0;  // a tracked geometry's properties held anew, and with no pin
			std::uint64_t fadeNodes = 0, fadeNodesRefused = 0;   // their fade nodes held anew, and with no pin
			std::uint64_t fadeNodesFromRoots = 0, fadeNodesRequested = 0, fadeNodesAnswered = 0, fadeNodesAnsweredNone = 0;
			std::uint32_t fadeNodeNamed = 0;
			std::string fadeNodeFirst;
			std::uint64_t fadeRootsUnowned = 0;                  // a fade root (or a component's node) left unlisted for want of its entry's reference
			std::string firstPropertyRefused;
		} referenceStats;
		struct LeafStats
		{
			std::uint64_t missing = 0, rescheduled = 0;
		} leafStats;
		SceneMirror mirror;
	};
}
