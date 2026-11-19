// Engine events that change fade roots: placement snaps and fadeAmount writes. Each feeds a key-only engine channel.

#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"  // kFadeStatics, kFadeState
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace DCLF::Features::Fades
{
	namespace
	{
		/**
		 * @brief Placement snaps. FUN_14147aa20, called from the cell attach, sets a fade node's fade, snap radius and last visible frame.
		 * A root listed before its snap must be reseeded from the node.
		 */
		// No wake needed, because the snap's cell attach already wakes the pass.
		EventQueue<const void*> fadeSnapEvents;

		/**
		 * @brief Actor::SetAlpha (vfunc 0xE1) writes the actor root's fadeAmount at +0x100 every frame of an actor fade-in.
		 * Without these events an actor listed mid-fade would keep that alpha.
		 */
		// No wake needed, because a fade writes every frame and the next pass takes them.
		EventQueue<const void*> fadeAmountEvents;

		// Rare. About one per newly seeded actor root.
		std::mutex recheckMutex;
		std::vector<const void*> rechecked;
		std::atomic<std::uint32_t> recheckPending{ 0 };

		// The cell attach writes the fade statics before calling the snap, so this update carries them too.
		void OnFadeSnap(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* node = SceneWriter::Arg<RE::NiAVObject*>(a_event, 0)) {
				// Push the mirror update before the key, so the pass that takes the key has already applied it.
				if (netimmerse_cast<RE::BSFadeNode*>(node))
					Extensions::PushNodeUpdate(node, Scene::kFadeStatics | Scene::kFadeState);
				fadeSnapEvents.Push(node);
			}
		}

		std::size_t DrainFadeSnaps(Api::KeySink& a_sink)
		{
			return fadeSnapEvents.Drain(a_sink);
		}

		std::size_t DrainFadeAmounts(Api::KeySink& a_sink)
		{
			return fadeAmountEvents.Drain(a_sink);
		}

		void PushFadeAmount(const void* a_key)
		{
			auto* root = static_cast<RE::NiAVObject*>(const_cast<void*>(a_key));
			// Push the mirror value before the key. Otherwise the row can be left at an older alpha for good.
			if (root->AsFadeNode())
				Extensions::PushNodeUpdate(root, Api::kNodeFieldFadeAmount);
			fadeAmountEvents.Push(a_key);
		}
	}

	void RecheckFadeAmount(RE::NiAVObject* a_node)
	{
		if (a_node && a_node->AsFadeNode()) {
			{
				std::lock_guard lock(recheckMutex);
				// Bounded, because a node whose event never reaches a listed root is never taken.
				if (rechecked.size() >= 1024)
					rechecked.erase(rechecked.begin());
				rechecked.push_back(a_node);
				recheckPending.store(static_cast<std::uint32_t>(rechecked.size()), std::memory_order_release);
			}
			PushFadeAmount(a_node);
		}
	}

	bool TakeRecheck(const void* a_node)
	{
		if (recheckPending.load(std::memory_order_acquire) == 0)
			return false;
		std::lock_guard lock(recheckMutex);
		const auto it = std::find(rechecked.begin(), rechecked.end(), a_node);
		if (it == rechecked.end())
			return false;
		rechecked.erase(it);
		recheckPending.store(static_cast<std::uint32_t>(rechecked.size()), std::memory_order_release);
		return true;
	}

	void RegisterEngine(Api::InstallContext& a_context, Api::EngineChannelId& a_snaps, Api::EngineChannelId& a_amounts)
	{
		// Snaps register first so they apply before amounts.
		a_snaps = a_context.RegisterEngineChannel({ .name = "fade snaps",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::BeforeStructural,
			.drain = &DrainFadeSnaps });
		a_amounts = a_context.RegisterEngineChannel({ .name = "fade amounts",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::BeforeStructural,
			.drain = &DrainFadeAmounts,
			.push = &PushFadeAmount });
	}

	void InstallEarlyHooks()
	{
		// The primary cull will not install without NoteRootStatesInstalled.
		if (!SceneWriter::SubscribeCall(SWF_HOOK_FADE_SNAP, &OnFadeSnap, SWF_PHASE_POST))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the fade snap after reporting it installed");
		Extensions::NoteRootStatesInstalled();
	}
}
