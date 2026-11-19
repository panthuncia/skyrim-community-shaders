#pragma once

// The coordinator's material texture bindings. Scene lane only, except the replies queue, which the import thread fills.

#include <cstdint>
#include <memory>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/**
	 * @brief The material slots' texture bindings.
	 *
	 * A new or changed record requests bindings for its views, and answers arrive as events in `replies` for the next pass.
	 * A view answered before is served from `cache`, which holds the owner weakly so an import nobody holds goes.
	 * The entries live in the lookups. This holds only what is not published.
	 */
	struct MaterialBindings
	{
		struct Cached
		{
			std::weak_ptr<const void> owner;
			std::uint32_t index = Lookups::kNone;
		};
		struct Stats
		{
			std::uint64_t requested = 0;  // views asked of the import thread
			std::uint64_t answered = 0;   // their answers drained
			std::uint64_t rejected = 0;   // of which the view was rejected
			std::uint64_t stale = 0;      // answers to a request made before a reset
			std::uint64_t cached = 0;     // views served from the cache
			std::uint64_t resolved = 0;   // entries that turned resolved
			std::uint64_t retired = 0;    // entries dropped with their slot
			std::uint64_t versioned = 0;  // entry changes versioned into the lookups (VersionMaterialBindings)
			std::uint64_t followed = 0;   // pinless: entries resolved again for a moved view (FollowRelocatedTextures)
			std::uint64_t superseded = 0;  // pinless: answers whose import a move superseded meanwhile (asked again)
		};
		struct Work
		{
			std::uint64_t sealed = 0;          // owner blocks sealed (GpuTextures::Seal)
			std::uint64_t sealShared = 0;      // owner blocks shared with another slot of the material instead
			std::uint64_t waitersDeduped = 0;  // a slot already waiting on a view, not listed again
		};

		// A slot whose new view is still requested keeps the previous view's binding meanwhile.
		std::vector<std::uint8_t> held;
		std::vector<std::uint32_t> changed;
		std::vector<std::uint8_t> changedMark;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::vector<std::uint32_t>> inFlight;
		// A slot resolved again while it waits is listed once.
		ankerl::unordered_dense::set<std::pair<ID3D11ShaderResourceView*, std::uint32_t>> waiting;
		// Weak, the entries hold it. A material's passes name the same textures, so its slots can share one block. Swept as it grows.
		ankerl::unordered_dense::map<const void*, std::weak_ptr<const Lookups::MaterialOwners>> sealed;
		std::size_t sealedSweep = 0;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, Cached> cache;
		std::size_t cacheSweep = 0;
		// Rejections included.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, GpuTextures::Binding> arrived;
		// A slot set since then is resolved.
		std::vector<std::uint64_t> usedSeen;
		LogCursor cursor;  // the tables' material log
		std::uint64_t relocationCursor = 0;  // GpuTextures::RelocatedViews
		std::uint64_t cookie = 1;  // the requests' since the last reset: an answer with another is stale
		GpuTextures::Replies replies{ &WakeSceneTextureReply };  // an answer wakes the scene pump
		Stats stats;
		Work work;

		void MarkChanged(std::uint32_t a_slot)
		{
			if (changedMark.size() <= a_slot)
				changedMark.resize(std::size_t(a_slot) + 1, 0);
			if (!changedMark[a_slot]) {
				changedMark[a_slot] = 1;
				changed.push_back(a_slot);
			}
		}
	};
}
