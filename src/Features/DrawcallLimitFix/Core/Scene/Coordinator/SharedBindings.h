#pragma once

// The coordinator's shared texture bindings: projected textures, technique shadow masks and shadow textures.
// Scene lane only, except projectedPosted, which the render thread posts, and the replies queue, which the import thread fills.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <winrt/base.h>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"

namespace DCLF
{
	/**
	 * @brief The texture bindings every record or pipeline shares, made by the scene work as MaterialBindings makes the material slots'.
	 *
	 * Each is requested where its need arises and answered as an event into `replies`, which the next pass drains. A view answered
	 * before resolves at once. The entries live in the lookups. This holds only what is not published.
	 */
	struct SharedBindings
	{
		enum Kind : std::uint32_t
		{
			kProjected,
			kMask,
			kShadow,
			kKinds
		};
		static constexpr std::array<const char*, kKinds> kKindNames{ "projected", "shadow masks", "shadow textures" };

		/** @brief As a native ProjectedUV draw bound them, each referenced. */
		struct ProjectedCapture
		{
			std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 4> views;
		};
		struct Cached
		{
			std::weak_ptr<const void> owner;
			std::uint32_t index = Lookups::kNone;
		};
		/** @brief What the lookups' mask entry was resolved for. */
		struct Mask
		{
			bool wanted = false;   // the technique binds a mask
			bool settled = false;  // answered (resolved or rejected: a rejection is permanent) or binding none: not looked up again
			bool listed = false;   // on `dirty`
		};
		/** @brief Answered entries are permanent. */
		struct ShadowTexture
		{
			bool pending = true;
		};
		struct Stats
		{
			std::array<std::uint64_t, kKinds> requested{};  // views asked of the import thread, by the first kind that asked
			std::array<std::uint64_t, kKinds> answered{};   // their answers drained, by every kind that waited on the view
			std::array<std::uint64_t, kKinds> rejected{};
			std::array<std::uint64_t, kKinds> cached{};     // views served from an answer kept (this one's or the material bindings')
			std::uint64_t stale = 0;                        // answers to a request made before a reset
			std::uint64_t shadowAdded = 0, shadowRemoved = 0;
			std::uint64_t projectedCaptures = 0;            // new captures taken by the scene work
			std::uint64_t masksHeld = 0;                    // a resolved mask kept while its new view is asked for
			std::uint64_t masksUnheld = 0;                  // a row's mask view not the frame's (FrameGlobals::shadowMaskHeld): asked for unheld
			std::uint64_t fixedPublished = 0;               // new fixed bindings taken
			std::uint64_t writtenShared = 0, writtenMasks = 0, writtenShadow = 0;
		};

		// Latest wins.
		std::atomic<std::shared_ptr<const ProjectedCapture>> projectedPosted;
		std::shared_ptr<const ProjectedCapture> projectedSeen;
		std::uint32_t projectedPending = 0;  // bit per texture: its view asked for
		std::shared_ptr<const GpuTextures::FixedBindings> fixed;
		bool sharedChanged = false;  // a shared entry's owner changed since the binding block was sealed
		std::vector<Mask> masks;
		// The masks to look up this pass, each once. Everything else stands while the witnesses below stand.
		std::vector<std::uint32_t> dirty;
		std::vector<std::uint64_t> usedSeen;          // the used pipelines as the last pass saw them
		std::vector<std::uint32_t> techniqueSeen;     // each technique row's bindingVersion then
		std::uint32_t textureGenerationSeen = ~0u;    // GpuTextures::Generation then
		std::uint64_t relocationCursor = 0;           // GpuTextures::RelocatedViews' cursor for the frame's shadow mask
		void MarkMask(std::uint32_t a_slot)
		{
			if (masks.size() <= a_slot)
				masks.resize(std::size_t(a_slot) + 1);
			if (!masks[a_slot].listed) {
				masks[a_slot].listed = true;
				dirty.push_back(a_slot);
			}
		}
		struct Work
		{
			std::uint64_t masksChecked = 0, masksResolved = 0, maskSweeps = 0;
		} work;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, ShadowTexture> shadowTextures;
		std::uint32_t shadowPending = 0;
		// With the kinds waiting on each, one bit per Kind.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::uint32_t> inFlight;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, Cached> cache;
		std::size_t cacheSweep = 0;
		// Rejections included.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, GpuTextures::Binding> arrived;
		std::uint64_t cookie = 1;  // the requests' since the last reset: an answer with another is stale
		GpuTextures::Replies replies{ &WakeSceneTextureReply };  // an answer wakes the scene pump
		Stats stats;
	};
}
