#pragma once

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Features::Lod::LodGates
{
	// LOD gates hold the engine's LOD swaps until DCLF has the new blocks ready, so no frame shows a hole.
	// A swap hides the incoming blocks and steals the outgoing ones so the engine does not release them.
	// When the scene publishes claims that cover the incoming blocks, the render thread calls Flip to show them
	// and hide the outgoing. The next LOD drain then releases the outgoing through the engine's own functions.
	// Each gate's state is one atomic. Flip and forced releases race by compare-exchange, and DCLF adds no lock.
	// Engine details are in skyrim-engine-notes.md, "LOD swaps and their gates".

	/**
	 * @brief Called by the swap thunks under the terrain manager's lock, after the incoming blocks attach.
	 * The outgoing roots stay attached and shown until Flip. Tokens start at 1.
	 */
	void CaptureGate(std::uint64_t a_token, std::span<RE::NiAVObject* const> a_incoming, std::span<RE::NiAVObject* const> a_outgoing);
	/** @brief Must run before any gate opens. */
	void NoteRegistered(Api::EngineChannelId a_channel, std::uint32_t a_hiddenSite);

	/** @brief Installs the thunks and detours once. */
	void Install();
	/** @brief Install patched every site. */
	bool Enabled();
	/** @brief Idempotent. */
	void InstallDetours();
	/**
	 * @brief Checks every hook site before anything is written. A detour that fails to attach forces DCLF off,
	 * because only the detours give stolen LOD handles back.
	 */
	void Preflight();

	/**
	 * @brief The show veto, called from the engine's LOD show stores on any thread. Vetoes is true for a pending incoming
	 * node of an open gate. Lock-free, it reads a table that is never written while gates are open.
	 */
	const std::atomic<std::uint32_t>& OpenGates();
	bool Vetoes(const void* a_object);

	/**
	 * @brief Render thread. Shows the incoming blocks, hides the outgoing and queues their release for the next LOD drain.
	 * Returns false for a gate that was already force-released or is unknown.
	 */
	bool Flip(std::uint64_t a_token);

	/**
	 * @brief What became of a gate. A non-null kept names a held block the engine reattached, which stays as the node's
	 * current block, so the scene must stop holding its geometries out.
	 */
	struct Outcome
	{
		std::uint64_t token = 0;
		bool forced = false;  // released without a flip, so the coordinator drops the gate's state
		std::array<const void*, 2> kept{};  // the block's root keys: the object NiNode, or the terrain land and water nodes
	};
	/** @brief Takes every outcome posted since the last call, oldest first. */
	void DrainOutcomes(void (*a_visit)(void*, const Outcome&), void* a_context);
	template <class F>
	void DrainOutcomes(F&& a_visit)
	{
		DrainOutcomes([](void* a_context, const Outcome& a_outcome) { (*static_cast<F*>(a_context))(a_outcome); }, &a_visit);
	}


	/** @brief Counters since the last Take, then reset. Empty while not installed. */
	inline constexpr std::size_t kFlushKinds = 9;
	struct Totals
	{
		bool installed = false;
		std::uint64_t opened = 0, initialOpened = 0, initialLoadScreen = 0, flipped = 0, flipMisses = 0, forced = 0, superseded = 0, held = 0, released = 0;
		std::uint64_t reused = 0, handedOff = 0, deferred = 0, reattached = 0, kept = 0, reusedHidden = 0, orphansShown = 0, vetoes = 0;
		std::uint64_t flipFrames = 0, releases = 0;
		std::int64_t flipTicks = 0, flipTicksMax = 0, releaseTicks = 0, releaseTicksMax = 0, oldestTicks = 0;
		std::uint32_t open = 0, oldestFrames = 0;
		std::array<std::uint64_t, kFlushKinds> flushes{};
	};
	Totals Take();
	const char* FlushName(std::size_t a_index);
	double TicksToMilliseconds(std::int64_t a_ticks);
}
