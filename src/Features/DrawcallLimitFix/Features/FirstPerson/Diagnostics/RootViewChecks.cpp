// The first-person view's scene diagnostics, subscribed to Core's root view events (Diagnostics/Scene/RootViewEvents.h).

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		using V = Diagnostics::SceneView;

		bool CulledBit(const Diagnostics::RootSubtreeSynced& a_subtree, const RootSubtreeNode& a_node) { return a_subtree.culled && a_node.saved ? a_node.culled : a_node.hidden; }

		void LogSubtreeChain(const Diagnostics::RootSubtreeSynced& a_subtree)
		{
			static bool chainLogged = false;
			if (chainLogged)
				return;
			for (const auto& node : a_subtree.nodes)
				if (node.key && node.geometry && !node.hidden) {
					chainLogged = true;
					std::string chain;
					std::uint32_t at = 0;
					for (const RE::NiAVObject* object = node.key; object && at < 24; object = object->parent, ++at)
						chain += fmt::format("{}'{}' {}", at ? " <- " : "", object->name.c_str() ? object->name.c_str() : "", static_cast<const void*>(object));
					logger::info("[DCLF] first-person subtree (T7e): the root {} (parent {}); a geometry's live chain: {}", static_cast<const void*>(a_subtree.root),
						static_cast<const void*>(a_subtree.root->parent), chain);
					break;
				}
		}

		void NoteFirstPersonDifference(const Diagnostics::RootSubtreeSynced& a_subtree)
		{
			static std::string last;
			static std::uint32_t logged = 0;
			static std::uint64_t seen = ~0ull;
			if (logged >= 24 || std::exchange(seen, a_subtree.changes) == a_subtree.changes)
				return;
			auto names = [&](bool a_atCull) {
				std::string text;
				std::uint32_t geometry = 0, hidden = 0;
				for (const auto& node : a_subtree.nodes) {
					if (!node.key || !node.geometry)
						continue;
					++geometry;
					if (!(a_atCull ? CulledBit(a_subtree, node) : node.hidden))
						continue;
					++hidden;
					if (hidden <= 12)
						text += fmt::format("{}'{}'", text.empty() ? "" : ", ", node.key->name.c_str() ? node.key->name.c_str() : "");
				}
				return fmt::format("{} geometries, {} hidden{}{}", geometry, hidden, text.empty() ? "" : ": ", text);
			};
			std::string now = "at the frame's start " + names(false) + (a_subtree.culled ? "; at the last first-person culls " + names(true) : std::string("; no first-person cull seen"));
			if (now == last)
				return;
			last = now;
			++logged;
			logger::info("[DCLF] first-person subtree (T7e): {}", now);
		}

		void OnSubtreeSynced(const Diagnostics::RootSubtreeSynced& a_subtree)
		{
			if (!a_subtree.frameStart)
				return;
			LogSubtreeChain(a_subtree);
			NoteFirstPersonDifference(a_subtree);
		}

		void OnHidden(const Diagnostics::RootViewHidden& a_hidden)
		{
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged.fetch_add(1, std::memory_order_relaxed) >= 16)
				return;
			const auto& cullHidden = a_hidden.cullHidden;
			const auto* object = static_cast<const RE::NiAVObject*>(a_hidden.key);
			const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_object) { return a_entry.first < a_object; });
			const bool listed = it != cullHidden.end() && it->first == object;
			const auto& mirror = V::mirror(a_hidden.store);
			std::string chain;
			const void* at = a_hidden.geometry;
			for (std::uint32_t n = 0; at && n < 16; ++n) {
				const auto* node = mirror.Node(at);
				chain += fmt::format("{}{}", n ? " <- " : "", at);
				at = node ? node->parent : nullptr;
				if (!node)
					chain += " (no record)";
			}
			logger::info("[DCLF] first-person hidden verdict (T7e): geometry {} filed under {} (the first-person root {}{}); hidden at chain depth {}: node {}{}, {} (the mirror's bit {}); "
						 "{} nodes listed in the frame's capture; the mirror's chain: {}",
				a_hidden.geometry, a_hidden.category, a_hidden.root, a_hidden.sameRoot ? ", the same" : ", another", a_hidden.depth, a_hidden.key,
				a_hidden.key == a_hidden.category ? " (its category node)" : "",
				listed ? fmt::format("listed hidden {} in the frame's capture", it->second) : std::string("not in the frame's capture"), a_hidden.recordHidden,
				cullHidden.size(), chain);
		}
	}

	void SubscribeRootViewChecks()
	{
		Diagnostics::Subscribe<Diagnostics::RootSubtreeSynced>(&OnSubtreeSynced);
		Diagnostics::Subscribe<Diagnostics::RootViewHidden>(&OnHidden);
	}
}
