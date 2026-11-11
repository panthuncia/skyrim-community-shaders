// The master particle systems (MasterParticles.h): the unheld roots contribution over the particle manager's child array, the removal
// hook at the manager's update, the counters of the manager's child edits, and the observers' report lines.

#include "Features/DrawcallLimitFix/Features/Particles/MasterParticles.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Utils/SwitchFile.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace DCLF::Features::Particles::MasterParticles
{
	namespace
	{
		// The particle manager's global (NiPointer<BSParticleSystemManager>, written by the world's setup FUN_140643060), its lock
		// (BSReadWriteLock at the global + 8: the manager's AttachChild takes it for write, FUN_140e2ac70 for read), the removal call in
		// the manager's per-master update FUN_140e2a520 (`if (master->+0x166 && refCount < 3 && !FUN_140e2b430(master))
		// children.SetAt(index, null)`, then the master's UpdateDownwardPass otherwise; queued per slot by UpdateParticleSystemManagerJob),
		// what it calls (NiTObjectArray<NiPointer<NiAVObject>>::SetAt), and the world reset's remove-all of the manager
		// (SceneWriterFramework's detour covers it; read here to log that it is still that call).
		// AE 1.6.1170 and SE 1.5.97 are the same code at the same offsets; AE 1.7.104 is checked by CallsTo before the hook goes in.
		constexpr Engine::Offset kManager{ { 411270 }, { 524630 } };                     // 0x143284b88 / 0x143024208
		constexpr Engine::Offset kRemovalCall{ { 76601, 0x69 }, { 74853, 0x69 } };       // 0x140e2a589 / 0x140d53029
		constexpr Engine::Offset kArraySetAt{ { 70322 }, { 68970 } };                    // 0x140d1ee40 / 0x140c59560
		constexpr Engine::Offset kResetRemoveAll{ { 36601, 0x584 }, { 35593, 0x475 } };  // 0x140649254 / 0x1405b5905
		constexpr Engine::Offset kRemoveAll{ { 70295 }, { 68944 } };                     // FUN_140d1d630 / FUN_140c58020 (SWF's removeAllChildren)

		constexpr std::size_t kChildrenArray = 0x110;    // NiNode::children (the NiTObjectArray SetAt is called on)
		constexpr std::size_t kMasterRemovable = 0x166;  // byte: the master may be removed once idle
		// std::uint32_t: the emitters feeding the master (its BSTArray of the source systems at +0x128, pruned by FUN_140e36630 in its
		// update). With none, its particles die out and the removal's live count (FUN_140e2b430, AE ID 76620 / SE 74872) reaches 0. Read
		// as a plain count: the live count walks arrays its update rebuilds on a job thread.
		constexpr std::size_t kMasterEmitters = 0x138;
		constexpr std::uint32_t kHeldFrames = 600;  // a removable master kept by a reference this long is flagged (<- MASTER REFS)

		RE::NiNode* Manager()
		{
			return Engine::Global<RE::NiNode*>(kManager);
		}

		RE::BSReadWriteLock& ManagerLock()
		{
			return *reinterpret_cast<RE::BSReadWriteLock*>(REL::Module::get().base() + kManager.Get() + sizeof(void*));
		}

		std::uint32_t FrameCounter()
		{
			return Engine::Global<std::uint32_t>(Engine::Address::kFrameCounter);
		}

		// The gate between the category capture (shared: enter to leave, the render thread) and the removal hook (exclusive): no removal
		// is pushed between a master's listing and its capture's pushes.
		std::shared_mutex gate;
		bool installed = false;

		// The counters (any thread) and the masters' exits by event (keys, any thread; drained by the render thread's gathers).
		struct Counts
		{
			std::atomic<std::uint64_t> attaches{ 0 }, detaches{ 0 }, removals{ 0 }, gathers{ 0 };
		} counts;
		EventQueue<const void*> exits;

		// The render thread's view at its last gather (inside enter): the masters listed, those that left with no event seen, and each
		// removable master's first frame seen held by a reference beyond the engine's two.
		struct Observed
		{
			std::vector<const void*> listed, gathered;
			std::vector<std::pair<const void*, std::uint32_t>> held;
			std::uint64_t unseenLeft = 0, flaggedHeld = 0;
			std::uint32_t lastListed = 0, maxListed = 0, heldNow = 0;
			std::string firstUnseen, firstHeld;
		} observed;
		std::mutex observedLock;  // the render thread's gathers write, the report reads

		void Observe(const std::vector<std::pair<RE::NiNode*, bool>>& a_masters)
		{
			const std::scoped_lock lock(observedLock);
			auto& o = observed;
			std::vector<const void*> exited;
			exits.Drain([&](const void* a_key) { exited.push_back(a_key); });
			std::ranges::sort(exited);
			o.gathered.clear();
			for (const auto& [master, held] : a_masters)
				o.gathered.push_back(master);
			std::ranges::sort(o.gathered);
			for (const auto* key : o.listed)
				if (!std::ranges::binary_search(o.gathered, key) && !std::ranges::binary_search(exited, key))
					if (o.unseenLeft++ == 0)
						o.firstUnseen = fmt::format("{}", key);
			std::swap(o.listed, o.gathered);
			o.lastListed = static_cast<std::uint32_t>(o.listed.size());
			o.maxListed = std::max(o.maxListed, o.lastListed);
			// Held past removal: removable, no emitter left, and more references than the engine's own two (the slot's and one more), for
			// kHeldFrames between gathers (DCLF's own pins last a frame or two).
			const std::uint32_t frame = FrameCounter();
			std::vector<std::pair<const void*, std::uint32_t>> held;
			o.heldNow = 0;
			for (const auto& [master, removableHeld] : a_masters) {
				if (!removableHeld)
					continue;
				++o.heldNow;
				std::uint32_t since = frame;
				for (const auto& [key, first] : o.held)
					if (key == master)
						since = first;
				if (frame - since >= kHeldFrames && since != ~0u) {
					if (o.flaggedHeld++ == 0)
						o.firstHeld = fmt::format("'{}' {} ({} references)", master->name.c_str() ? master->name.c_str() : "", static_cast<const void*>(master),
							master->GetRefCount());
					since = ~0u;  // flagged once
				}
				held.emplace_back(master, since);
			}
			o.held = std::move(held);
		}

		void Enter()
		{
			gate.lock_shared();
		}

		void Leave()
		{
			gate.unlock_shared();
		}

		void Gather(Api::UnheldRootSink& a_sink)
		{
			auto* manager = Manager();
			if (!manager)
				return;
			counts.gathers.fetch_add(1, std::memory_order_relaxed);
			std::vector<std::pair<RE::NiNode*, bool>> masters;
			// Under the manager's lock: its AttachChild grows the array under it. The removal hook waits for Leave, so no idle removal
			// clears a slot read here before the sink pins its master (the manager's own DetachChild overrides do not wait: none is known
			// to detach a master during play).
			auto& lock = ManagerLock();
			lock.LockForRead();
			for (const auto& child : Engine::Children(*manager)) {
				auto* master = child ? child->AsNode() : nullptr;
				if (!master || master->parent)
					continue;
				// Read before the sink pins it. An idle master (removable, no emitter left) with a reference beyond the engine's own two (the
				// slot's and one more) is one the engine would remove once its particles died out.
				const bool removableHeld = Engine::At<std::uint8_t>(master, kMasterRemovable) && master->GetRefCount() > 2 &&
				                           std::atomic_ref<std::uint32_t>(Engine::At<std::uint32_t>(master, kMasterEmitters)).load(std::memory_order_relaxed) == 0;
				masters.emplace_back(master, removableHeld);
			}
			lock.UnlockForRead();
			for (const auto& [master, held] : masters)
				a_sink.Add(master);
			Observe(masters);
		}

		/** @brief The manager's update clearing an idle master's slot: its detach pushed first, as a child edit's would be. */
		struct RemovalHook
		{
			static void thunk(void* a_array, std::uint32_t a_index, void* a_value)
			{
				auto* manager = reinterpret_cast<RE::NiNode*>(static_cast<std::byte*>(a_array) - kChildrenArray);
				std::unique_lock lock(gate);
				const auto& children = manager->GetChildren();
				if (RE::NiAVObject* master = a_index < children.free_idx() ? children[a_index].get() : nullptr) {
					counts.removals.fetch_add(1, std::memory_order_relaxed);
					PrimaryCull::Get().NoteListStructure(manager, master, false);
					SceneTracker::Get().PushDetached(master, manager);
					exits.Push(master);
				}
				func(a_array, a_index, a_value);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The manager's child edits through SceneWriterFramework (its AttachChild override; its detaches and the world reset's
		// remove-all): counted. SceneTracker pushes their events (naming the manager as the parent).
		void OnAttached(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_ChildEvent*>(a_event);
			if (event.parent && event.parent == Manager())
				counts.attaches.fetch_add(1, std::memory_order_relaxed);
		}

		void OnDetaching(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_ChildEvent*>(a_event);
			if (event.parent && event.parent == Manager() && event.child) {
				counts.detaches.fetch_add(1, std::memory_order_relaxed);
				exits.Push(event.child);
			}
		}

		bool SitesKnown()
		{
			return kManager.Known() && kRemovalCall.Known() && kArraySetAt.Known();
		}
	}

	bool Enabled()
	{
		static const bool enabled = Util::SwitchFile::Enabled("CS_DCLF_MASTER_PARTICLES");
		return enabled;
	}

	void Preflight()
	{
		if (!Enabled())
			return;
		HookSites::RequireKnown("the particle manager's master removal", SitesKnown());
		if (SitesKnown())
			HookSites::RequireCall("the particle manager's master removal (FUN_140e2a520's SetAt of an idle master's slot)", REL::Offset(kRemovalCall).address());
	}

	void Install(Api::InstallContext& a_context)
	{
		a_context.RegisterSwitch({ .name = "CS_DCLF_MASTER_PARTICLES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never, .description = "1: the particle manager's master particle systems (torches, candles, street mist) tracked as DCLF's unheld roots, their particle systems drawn by DCLF's particle path (dclf-roadmap.md 1.4; needs CS_DCLF_PARTICLES); 0 (default) they stay the engine's", .after = "CS_DCLF_PARTICLES" });
		if (!Enabled() || installed)
			return;
		if (!SitesKnown()) {
			logger::warn("[DCLF] master particles: this runtime has no address for the particle manager's master removal; they stay the engine's");
			return;
		}
		// Chained (Preflight required a CALL): another plugin's hook there is called through. A call to another of the game's functions is
		// not the removal (a runtime whose code moved), and the masters stay the engine's.
		const auto removal = REL::Offset(kRemovalCall).address();
		const auto setAt = REL::Offset(kArraySetAt).address();
		if (!Engine::CallsTo(removal, setAt)) {
			const auto module = HookSites::BranchTargetModule(removal);
			if (module == HookSites::ModuleOf(setAt)) {
				logger::warn("[DCLF] master particles: the call at {:#x} is not the master removal's SetAt; they stay the engine's", removal);
				return;
			}
			logger::info("[DCLF] master particles: the master removal at {:#x} is hooked by {}; DCLF's thunk chains onto it", removal, module);
		}
		if (!SceneWriter::Subscribe(SWF_EVENT_CHILD_ATTACHED, &OnAttached) || !SceneWriter::Subscribe(SWF_EVENT_CHILD_DETACHING, &OnDetaching)) {
			logger::warn("[DCLF] master particles: the scene writer framework refused the child-edit subscriptions; they stay the engine's");
			return;
		}
		stl::write_thunk_call<RemovalHook>(removal);
		a_context.RegisterUnheldRoots({ .owner = &Manager, .enter = &Enter, .leave = &Leave, .gather = &Gather });
		installed = true;
		const auto reset = REL::Offset(kResetRemoveAll).address();
		logger::info(
			"[DCLF] master particles: on (CS_DCLF_MASTER_PARTICLES=1): the particle manager's masters are unheld category roots; removal hooked at {:#x}; "
			"the world reset's remove-all at {:#x} {}",
			removal, reset, Engine::CallsTo(reset, REL::Offset(kRemoveAll).address()) ? "is the engine's (SceneWriterFramework's detour fires its detaches)" : "is not a call to the remove-all <- FLAG");
	}

	void Lines(std::string& a_text)
	{
		if (!installed)
			return;
		const auto attaches = counts.attaches.exchange(0, std::memory_order_relaxed);
		const auto detaches = counts.detaches.exchange(0, std::memory_order_relaxed);
		const auto removals = counts.removals.exchange(0, std::memory_order_relaxed);
		const auto gathers = counts.gathers.exchange(0, std::memory_order_relaxed);
		const std::scoped_lock lock(observedLock);
		const auto& o = observed;
		a_text += fmt::format(
			"[DCLF] master particles (CS_DCLF_MASTER_PARTICLES): {} listed at the last gather ({} at most); {} attaches, {} detaches (the manager's child "
			"edits), {} idle removals hooked, {} gathers; {} left with no event{}{}\n",
			o.lastListed, o.maxListed, attaches, detaches, removals, gathers, o.unseenLeft, o.unseenLeft ? " <- MASTER SET" : " <- OK",
			o.firstUnseen.empty() ? std::string() : fmt::format(" (first {})", o.firstUnseen));
		a_text += fmt::format("[DCLF] master particles' references: {} idle masters held past removal at the last gather, {} held {} frames or more{}{}\n", o.heldNow,
			o.flaggedHeld, kHeldFrames, o.flaggedHeld ? " <- MASTER REFS" : " <- OK", o.firstHeld.empty() ? std::string() : fmt::format(" (first {})", o.firstHeld));
	}
}
