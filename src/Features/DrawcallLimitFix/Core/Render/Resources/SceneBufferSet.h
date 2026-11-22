#pragma once

// The scene's growable buffers. Every versioned buffer DCLF makes registers with one set, which grows them to the
// revision producer's requirements. Features include this header directly, so it must stay self-contained.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"

#include <ORGModuleServices/GrowableBufferSet.h>
#include <Render/Runtime/IUploadService.h>

#include <cstdint>
#include <memory>
#include <string>

namespace DCLF::Draws
{
	/** @brief Any thread. Wakes the snapshot builder. Coalesced, and a no-op before its first work. */
	void WakeSnapshotBuilder();

	using GrowthBuild = org::services::GrowableBufferSet::Build;
	using SealedSet = org::services::GrowableBufferSet::Sealed;  // a Seal's result, the sealed set and its fragment
	using LatchHandle = org::services::LatchHandle;

	/** @brief The scene's set. Make and RegisterLatch are lock-free from any thread, everything else is the revision producer's alone.
	 * A set that cannot meet a requirement is never sealed. The producer seals again once the growth batch settles. */
	class SceneBufferSet
	{
	public:
		static SceneBufferSet& Get();

		/** @brief Any thread. Registers a_first as the first version of a buffer of the set. */
		Versioned Make(std::shared_ptr<org::Buffer> a_first, GrowableSpec a_spec);

		/** @brief What a latch block holds: a region per run, or one region every run reads, such as a block of zeros. */
		using LatchRegions = org::services::LatchRegions;
		/** @brief Any thread. Registers a latch block that the seals requiring it make or grow. Shared makes a dedicated block,
		 * PerRun a block of the runtime's frame arena. */
		LatchHandle RegisterLatch(std::string a_name, LatchRegions a_regions, org::services::GrowthPolicy a_policy = org::services::GrowthPolicy::Headroom(0))
		{
			return set.Register(std::move(a_name), a_regions, a_policy);
		}

		/** @brief Producer. The uploader a growth's fills go through. */
		void SetUploads(std::weak_ptr<org::runtime::IUploadService> a_uploads) { uploads = std::move(a_uploads); }
		/** @brief Producer. Starts a build. */
		GrowthBuild Begin() { return set.Begin(); }
		/** @brief Producer. The set naming every requirement, or pending while a batch grows what it lacks. */
		SealedSet Seal(GrowthBuild&& a_build);
		/** @brief Producer. Requires at least a_units of a_buffer. */
		void Require(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_units) { set.Require(a_build, a_buffer, a_units); }
		/** @brief Producer. Requires a version newer than a_serial, which relays it out at the same size. */
		void RequireNewer(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_serial) { set.RequireNewer(a_build, a_buffer, a_serial); }
		/** @brief Producer. Requires a_latch's block of at least a_bytes a slot, laid out as a_layoutKey (0 for plain bytes). */
		static void Require(GrowthBuild& a_build, LatchHandle a_latch, std::uint32_t a_bytes, std::uint64_t a_layoutKey) { a_build.Require(a_latch, a_bytes, a_layoutKey); }
		/** @brief Producer. The units a_buffer's newest ready version holds. */
		std::uint64_t Units(const GrowthBuild& a_build, const Versioned& a_buffer) { return set.Units(a_build, a_buffer); }
		/** @brief Producer. The most asked of a_buffer, at least what it holds. */
		std::uint64_t Asked(const GrowthBuild& a_build, const Versioned& a_buffer) { return set.MostAsked(a_build, a_buffer); }
		/** @brief Producer. The OMS set, for helpers that take it. */
		org::services::GrowableBufferSet& Set() { return set; }
		/** @brief The producer's report line, which resets its counters. Empty when nothing happened. */
		std::string Report();
		/** @brief Producer. Whether a shrink waits to be observed or named. */
		bool ShrinkPending() const { return set.ShrinkPending(); }
		/** @brief Producer. Whether buffers moved since the last seal, so the next one names their new addresses. */
		bool AddressesMoved() const { return set.AddressesMoved(); }
		/**
		 * @brief Any thread. Whether a_sealed names the addresses every buffer of the set has now: false for a set sealed before an
		 * idle move, whose shapes and recordings may still hold the old ones.
		 */
		bool AddressesCurrent(const org::services::NamedVersions& a_sealed) const { return a_sealed.AddressEpoch() >= set.MovedEpoch(); }

	private:
		SceneBufferSet();
		org::services::GrowableBufferSet set;
		std::weak_ptr<org::runtime::IUploadService> uploads;
		std::uint32_t failuresLogged = 0;
	};

	/** @brief Buffers of the scene's set indexed alike. One requirement fans out to every member, and the group holds the least any member holds. */
	struct UnitGroup : org::services::GrowableUnitGroup
	{
		UnitGroup& Add(const Versioned& a_buffer)
		{
			org::services::GrowableUnitGroup::Add(a_buffer);
			return *this;
		}
		/** @brief Producer. Requires at least a_units of every member. */
		void Require(GrowthBuild& a_build, std::uint64_t a_units) const { org::services::GrowableUnitGroup::Require(SceneBufferSet::Get().Set(), a_build, a_units); }
		/** @brief Producer. The most asked of any member. */
		std::uint64_t Asked(const GrowthBuild& a_build) const { return org::services::GrowableUnitGroup::Asked(SceneBufferSet::Get().Set(), a_build); }
	};
}
