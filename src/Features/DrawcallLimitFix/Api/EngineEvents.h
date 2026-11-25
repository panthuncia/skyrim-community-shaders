#pragma once

// Engine events for features. A stand-in observer hears about the roots the primary cull takes over.
// An engine channel is a feature's own queue of keys, fed by its hooks and drained by each scene pass.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/EngineEvents.h.
	class StandInRootsChange;
	class KeySink;

	/**
	 * @brief Hears about the fade roots DCLF stands in for whole, which no engine cull visits. Called per change, never per root.
	 * Every callback is optional.
	 *
	 * - changed: render thread. Roots that left come before roots that joined. They stay alive during the call.
	 * - updateEntry: main thread at Main::Update's entry, in registration order.
	 * - afterWrites: main thread, after every updateEntry.
	 * - structure: any thread, per attach or detach. Must be lock-free and cheap. a_parent may be null.
	 * - beforeListJobsWait: render thread, once per frame right before it waits for the list jobs. The job threads cull meanwhile, so it
	 *   must not touch what the cull hooks read or write.
	 * - listJobsDone: render thread, once per frame when the list jobs finish. Data the hooks gathered on job threads is complete here.
	 */
	struct StandInObserver
	{
		void (*changed)(const StandInRootsChange& a_change) = nullptr;
		void (*cleared)() = nullptr;
		void (*updateEntry)() = nullptr;
		void (*afterWrites)() = nullptr;
		void (*structure)(const void* a_parent) = nullptr;
		void (*beforeListJobsWait)() = nullptr;
		void (*listJobsDone)() = nullptr;
	};

	/** @brief What an engine channel carries. */
	enum class ChannelKind : std::uint8_t
	{
		// Keys pushed from any thread after the engine write. The drain never dereferences them.
		Keys,
		// Events on the scene's own event stack, ordered with the attaches and detaches around them. No queue and no drain.
		SceneStack,
	};

	/**
	 * @brief Where in a scene pass a channel's keys are drained and applied. Keys pushed after the drain wait for the next pass.
	 *
	 * - BeforeStructural: after Core's fade snaps and amounts, before property and node events.
	 * - AfterSwitches: after the switch events, before the leaf holds.
	 * - AfterBatch: SceneStack channels only. Runs after the batch with no keys, so the component applies what it took itself.
	 */
	enum class ChannelApply : std::uint8_t
	{
		BeforeStructural,
		AfterSwitches,
		AfterBatch,
	};

	/** @brief What wakes the scene pass when the channel gets a key. */
	enum class WakeSource : std::uint8_t
	{
		None,
		Pass,
	};

	/** @brief A registered engine channel. */
	struct EngineChannelId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
		bool operator==(const EngineChannelId&) const = default;
	};

	/**
	 * @brief A feature's own key queue. Its hooks push, and every scene pass drains it and hands the keys to the components' keys hooks.
	 *
	 * - namesKeys: the writers' events carry no value of their own, so the mirror parity counts these keys as named.
	 * - drain: scene lane, the queue's only consumer. Moves every key pushed so far into a_sink, oldest first. None for SceneStack.
	 * - push: optional, any thread, lock-free. Lets another feature's hooks push into this channel.
	 */
	struct EngineChannel
	{
		std::string_view name;
		ChannelKind kind = ChannelKind::Keys;
		ChannelApply apply = ChannelApply::AfterSwitches;
		WakeSource wake = WakeSource::None;
		bool namesKeys = false;
		std::size_t (*drain)(KeySink& a_sink) = nullptr;
		void (*push)(const void* a_key) = nullptr;
	};

	/**
	 * @brief A SceneStack event as a pass applies it. Pinned roots stay alive while the batch holds its pins.
	 * Named roots are keys only and are never dereferenced.
	 */
	struct Stacked
	{
		std::uint64_t token = 0;
		std::span<const void* const> pinned, named;
		std::uint64_t stampNs = 0;
		std::uint32_t stampFrame = 0;
	};

	/**
	 * @brief Lets a feature skip engine show stores. The stubs test `open` and call `vetoes` only while it is non-zero.
	 * A vetoed store does not run and raises no hidden event. Core supplies defaults, so the stubs look the same with or without a veto.
	 */
	struct ShowVeto
	{
		const std::atomic<std::uint32_t>* open = nullptr;
		bool (*vetoes)(const void* a_object) = nullptr;
	};

	/** @brief A feature's own engine writer whose hook pushes move events, counted under its name in the report. */
	struct MoveWriterId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
		bool operator==(const MoveWriterId&) const = default;
	};

	/**
	 * @brief Who receives a reserved writer's move event. ScenePass is the scene pass only. WithPlacements adds FrameValues' placements.
	 * Placements is the placements only, for keys only it knows, such as a BSOrderedNode.
	 */
	enum class MoveReach : std::uint8_t
	{
		ScenePass,
		WithPlacements,
		Placements,
	};
}

// Node capture contributions and node updates.

#include <vector>

namespace DCLF::Api
{
	/**
	 * @brief An engine node class whose OnVisible visits its own ordered list instead of its children, such as a BGSDecalNode's decals.
	 * The scene capture records that list with the node. One contribution per class.
	 *
	 * - capture: runs in the engine-read window or under the engine call that holds the node. Once per captured node, never per frame.
	 */
	struct NodeCaptureContribution
	{
		std::string_view name;
		const void* (*rtti)() = nullptr;
		void (*capture)(const void* a_node, std::vector<const void*>& a_out) = nullptr;
	};

	/** @brief Node record fields a feature's hook may push. FadeAmount is the fade node's fadeAmount at +0x100. */
	inline constexpr std::uint32_t kNodeFieldCaptured = 1u << 21;
	inline constexpr std::uint32_t kNodeFieldFadeAmount = 1u << 20;
}
