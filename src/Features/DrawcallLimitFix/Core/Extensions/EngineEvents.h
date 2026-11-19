#pragma once

// Core's side of Api/EngineEvents.h: what stand-in observers receive, the calls features' engine hooks make, and where channel keys go.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
	class BSGeometry;
	class NiAVObject;
}

namespace DCLF::Api
{
	/** @brief The stood-in roots that left and joined. */
	class StandInRootsChange
	{
	public:
		const std::vector<const RE::NiAVObject*>& left;
		const std::vector<const RE::NiAVObject*>& joined;
	};

	/** @brief Where an engine channel's drain puts its keys: the pass's batch, a load screen's named keys, or nowhere. Called per key. */
	class KeySink
	{
	public:
		explicit KeySink(std::vector<const void*>* a_out) :
			out(a_out) {}

		void operator()(const void* a_key) const
		{
			if (out)
				out->push_back(a_key);
		}

	private:
		std::vector<const void*>* out;
	};
}

namespace DCLF::Extensions
{
	/** @brief Most engine channels a batch holds keys for. Grow it before registering more. */
	inline constexpr std::uint32_t kEngineChannelSlots = 6;
	/** @brief Marks a channel id as a need resolved by name. The low bits are the need's slot. */
	inline constexpr std::uint32_t kEngineChannelNeedBit = 1u << 31;
	inline constexpr std::uint32_t kEngineChannelNeedSlots = 2;
	/** @brief Main thread, at the end of InstallFeatures. Binds each needed name to its channel. Unbound needs drop their pushes. */
	void BindEngineChannelNeeds(std::span<const std::string> a_needs);
	/**
	 * @brief Any thread, lock-free, after the store it reports. Pushes a_key through the channel's push.
	 * Dropped before needs are bound, for an unbound need, or for a channel without push.
	 */
	void PushChannelKey(Api::EngineChannelId a_channel, const void* a_key);

	/** @brief Main thread, before InstallFeatures. The hidden-node source's site count. Reserved sites come after these. */
	void NoteHiddenSourceSites(std::uint32_t a_sites);
	std::uint32_t HiddenSourceSites();

	// Pushes from the features' engine hooks. Any thread, after InstallFeatures.
	/**
	 * @brief Pushes a SceneStack event onto the scene's event stack, after what this thread pushed before.
	 * a_pinned stay pinned until the batch is released. Ignored for token 0 or once the tracker has stopped.
	 */
	void PushStacked(Api::EngineChannelId a_channel, std::uint64_t a_token, std::span<RE::NiAVObject* const> a_pinned, std::span<RE::NiAVObject* const> a_named);
	/** @brief Wakes a scene pass if the channel's wake is Pass. */
	void WakeChannel(Api::EngineChannelId a_channel);
	/** @brief Reports a feature's own store to a_object's kHidden bit as a hidden event on a_site. Call after the store. */
	void PushReservedHidden(const RE::NiAVObject* a_object, std::uint32_t a_site);
	/** @brief Whether the hidden-node source is installed, so hidden events flow. Features that follow them engage only then. */
	bool HiddenEventsLive();
	/** @brief Reports a feature's own store to a ranged geometry's segments, after the store and in order with this thread's earlier pushes. */
	void PushRangesUpdate(const RE::BSGeometry* a_geometry);
	/** @brief Most move writers the report counts, Core's and reserved. Grow it before reserving more. */
	inline constexpr std::uint32_t kMoveWriterSlots = 10;
	/**
	 * @brief Main thread, before DCLF's threads start and before the writer's hooks go in. Reserves a named move writer.
	 * Core's writers keep slots 0, 1, 5, 6 and 9, and reserved ones fill the free slots in order, so report order stays stable.
	 * Scene::MoveEventsLive stays false until MoveWriterInstalled.
	 */
	Api::MoveWriterId ReserveMoveWriter(std::string_view a_name);
	/** @brief Main thread. Reports that a_writer's hooks are in. */
	void MoveWriterInstalled(Api::MoveWriterId a_writer);
	/**
	 * @brief Main thread, after Core's scene events. Whether Core's own move writers are in.
	 * A feature whose writers complete Core's movers must install them only then, so they are all in or all out.
	 */
	bool CoreMoveWritersLive();
	/** @brief Any thread, in a reserved writer's hook after the write. Reports that a_key moved. A null key is counted but not pushed. */
	void PushReservedMove(Api::MoveWriterId a_writer, const void* a_key, Api::MoveReach a_reach);

	/** @brief Main thread. Reports that the primary cull is installed, so stand-in observers will be called. */
	void NoteStandInInstalled();
	/** @brief Main thread, at install. Whether the primary cull is installed. */
	bool StandInInstalled();
	// The primary cull's calls, each to every stand-in observer in order.
	void StandInRootsChanged(const Api::StandInRootsChange& a_change);
	void StandInRootsCleared();
	void StandInUpdateEntry(bool a_running);
	void StandInAfterWrites(bool a_running);
	/** @brief Any thread, no lock. Each observer's structure. */
	void StandInStructure(const void* a_parent);
	/** @brief Render thread, when the list jobs finish. Each observer's listJobsDone. */
	void StandInListJobsDone();
	// DCLF's main-camera verdicts as written at Main::Update's entry. Main thread, after that point. Reads no engine memory.
	/** @brief A stood-in root's last written verdict. Visible if none is written yet. */
	bool StandInVerdict(const void* a_root);
	/** @brief Writes the verdict of any root DCLF lists into a_visible. Returns false when it has none. */
	bool ListedVerdictOf(const void* a_root, bool& a_visible);
}
