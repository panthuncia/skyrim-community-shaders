// Fade roots on the scene lane. A fade root is an engine fade node shared by one or more object slots.
// The coordinator component lists one row per root, holds a reference to its node, and updates the row on engine events.

#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"  // NoteFadeRootUnmirrored
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SoleHolds.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace DCLF::Scene
{
	namespace
	{
		/** @brief Member slots per root. Removal swaps with the last entry. */
		struct RootMembers
		{
			std::vector<std::vector<std::uint32_t>> lists;
			std::vector<std::uint32_t> position;
			void Add(std::uint32_t a_key, std::uint32_t a_slot)
			{
				if (lists.size() <= a_key)
					lists.resize(std::size_t(a_key) + 1);
				if (position.size() <= a_slot)
					position.resize(std::size_t(a_slot) + 1, ~0u);
				position[a_slot] = static_cast<std::uint32_t>(lists[a_key].size());
				lists[a_key].push_back(a_slot);
			}
			void Remove(std::uint32_t a_key, std::uint32_t a_slot)
			{
				auto& list = lists[a_key];
				const std::uint32_t at = position[a_slot];
				const std::uint32_t moved = list.back();
				list[at] = moved;
				position[moved] = at;
				list.pop_back();
				position[a_slot] = ~0u;
			}
			std::uint32_t First(std::uint32_t a_key) const { return a_key < lists.size() && !lists[a_key].empty() ? lists[a_key].front() : ~0u; }
			void Clear()
			{
				lists.clear();
				position.clear();
			}
			/** @brief Remove's precondition. */
			bool Has(std::uint32_t a_key, std::uint32_t a_slot) const
			{
				return a_key < lists.size() && a_slot < position.size() && position[a_slot] < lists[a_key].size() && lists[a_key][position[a_slot]] == a_slot;
			}
		};

		struct FadeRootState
		{
			RootMembers members;
			// Admitted roots by node. The value says whether the root is stood in.
			ankerl::unordered_dense::map<const void*, bool> owned;
			// Node references by root slot. Retirement keeps them alive while a published version still names the root.
			std::vector<RE::NiPointer<RE::NiAVObject>> owners;
			// For the diagnostic capture.
			std::vector<std::uint8_t> lastWriter;
			std::vector<std::uint32_t> lastWriteFrame;
			// fadeAmount event counts for the session. amountNoMirror is a suspect for a stuck partial fade.
			std::uint64_t amountEvents = 0, amountNotListed = 0, amountNoMirror = 0, amountUnchanged = 0, amountChanged = 0;
			std::uint64_t rechecks = 0, recheckChanged = 0, reseeds = 0;
		};

		void NoteRootWrite(FadeRootState& a_state, std::uint32_t a_root, FadeRootWriter a_writer)
		{
			if (a_state.lastWriter.size() <= a_root) {
				a_state.lastWriter.resize(std::size_t(a_root) + 1, 0);
				a_state.lastWriteFrame.resize(std::size_t(a_root) + 1, 0);
			}
			a_state.lastWriter[a_root] = static_cast<std::uint8_t>(a_writer);
			a_state.lastWriteFrame[a_root] = Coordinator::Get().SceneStep();
		}

		// Made once and held for the session.
		FadeRootState* liveState = nullptr;

		template <class T>
		bool SameRows(const std::vector<T>& a_left, const std::vector<T>& a_right)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			return a_left.size() == a_right.size() && (a_left.empty() || std::memcmp(a_left.data(), a_right.data(), a_left.size() * sizeof(T)) == 0);
		}

		bool SameIndex(const ankerl::unordered_dense::map<const void*, std::uint32_t>& a_left, const ankerl::unordered_dense::map<const void*, std::uint32_t>& a_right)
		{
			if (a_left.size() != a_right.size())
				return false;
			for (const auto& [key, value] : a_left)
				if (const auto it = a_right.find(key); it == a_right.end() || !(it->second == value))
					return false;
			return true;
		}

		struct LiveRoots
		{
			Extensions::TableFamilyColumns& family;
			FadeRootTables& roots;
		};
		LiveRoots LiveRootsOf(const Api::CoordinatorComponentContext& a_context)
		{
			auto& family = a_context.tables.families.Mutable(FadeRootFamilyId());
			return { family, *static_cast<FadeRootTables*>(family.columns.get()) };
		}

		std::shared_ptr<void> MakeComponent()
		{
			auto state = std::make_shared<FadeRootState>();
			liveState = state.get();
			return state;
		}

		FadeRootStatic FadeRootFromMirror(const Api::CoordinatorComponentContext& a_context, const void* a_node)
		{
			// Builds use these values until the seed arrives. The plan bits are a guess that the seed corrects.
			FadeRootStatic row;
			const auto& mirror = a_context.Mirror();
			const auto* node = mirror.Node(a_node);
			if (!node) {
				// The mirror lags by a frame, or the node is outside the world. The seed will bring the real values.
				NoteFadeRootUnmirrored();
				return row;
			}
			row.initial.currentFade = node->currentFade;
			row.initial.levels = node->fadeLevel | (std::uint32_t(node->fadeType) << 8);
			row.fadeAmount = node->fadeAmount;
			row.nearDistance = node->fadeNear;
			row.farDistance = node->fadeFar;
			row.bits = (std::uint32_t(node->fade109) << kFadeRootBitsShift) |
			           ((node->kind & SceneCapture::kKindTree) ? (kFadeRootTree | kFadeRootTreeThresholds) : kFadeRootFade);
			if (node->treeLodSwitch)
				if (const auto* lodSwitch = mirror.Node(node->treeLodSwitch); lodSwitch && lodSwitch->switchIndex > 0)
					row.bits |= kFadeRootTreeLod;
			return row;
		}

		/** @brief Starts a new generation for the row and asks for a fresh seed into the other seed row. */
		void RequestFadeSeed(const Api::CoordinatorComponentContext& a_context, FadeRootTables& a_roots, std::uint32_t a_root, FadeRootStatic& a_row)
		{
			const auto& owners = a_context.State<FadeRootState>().owners;
			a_row.bits ^= kFadeRootSeedOdd;
			a_row.generation = ++a_roots.fadeRootGenerations;
			if (a_root < owners.size() && owners[a_root])
				a_context.RequestSeed({ owners[a_root], a_root, a_row.generation, static_cast<std::uint32_t>(FadeSeedRow(a_root, a_row.bits)) });
		}

		/** @brief Removes a slot from its root. The last member leaving retires the root. */
		void UnlistFadeRoot(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_slot)
		{
			auto [family, roots] = LiveRootsOf(a_context);
			if (a_slot >= family.objectRow.size() || family.objectRow[a_slot] == kNoFadeRoot)
				return;
			family.NoteWrite();
			auto& state = a_context.State<FadeRootState>();
			auto& members = state.members;
			auto& current = family.objectRow[a_slot];
			if (members.Has(current, a_slot))
				members.Remove(current, a_slot);
			// The root's centre is one of its members, so pick another if this slot was it.
			if (current < roots.fadeRoots.size() && roots.fadeRoots[current].object == a_slot && roots.fadeRootRefs[current] > 1) {
				roots.fadeRoots[current].object = members.First(current);
				family.Mark(current);
			}
			if (current < roots.fadeRootRefs.size() && --roots.fadeRootRefs[current] == 0) {
				const auto* node = static_cast<const RE::NiAVObject*>(roots.fadeRootNode[current]);
				std::erase_if(roots.fadeRootSwitch, [&](const auto& a_entry) { return a_entry.second == current; });
				roots.fadeRootIndex.erase(node);
				roots.fadeRootNode[current] = nullptr;
				if (current < state.owners.size())
					a_context.HandBack(std::move(state.owners[current]));
				roots.fadeRoots[current] = FadeRootStatic{};
				a_context.tables.Retire(SceneTables::kRetiredFamilyRow, current, FadeRootFamilyId().index);
				family.Mark(current);
			}
			current = kNoFadeRoot;
		}

		std::uint32_t ListNewRoot(const Api::CoordinatorComponentContext& a_context, const LiveRoots& a_live, const void* a_node, std::uint32_t a_slot,
			RE::NiPointer<RE::NiAVObject>&& a_owner)
		{
			auto& [family, roots] = a_live;
			auto& state = a_context.State<FadeRootState>();
			std::uint32_t r;
			if (!roots.fadeRootFree.empty()) {
				r = roots.fadeRootFree.back();
				roots.fadeRootFree.pop_back();
			} else {
				r = static_cast<std::uint32_t>(roots.fadeRoots.size());
				roots.fadeRoots.emplace_back();
				roots.fadeRootRefs.push_back(0);
				roots.fadeRootNode.push_back(nullptr);
			}
			// Scene work reads no engine memory. The node's real values come from the seed in the frame that first draws this generation.
			auto row = FadeRootFromMirror(a_context, a_node);
			row.object = a_slot;
			row.bits |= roots.fadeRoots[r].bits & kFadeRootSeedOdd;
			if (const auto owned = state.owned.find(a_node); owned != state.owned.end())
				row.bits |= kFadeRootOwned | (owned->second ? kFadeRootStoodIn | kFadeRootWritten : 0u);
			// DCLF owns and writes every listed root from the moment it is listed.
			row.bits |= kFadeRootOwned | kFadeRootWritten;
			roots.fadeRootNode[r] = a_node;
			if (state.owners.size() <= r)
				state.owners.resize(std::size_t(r) + 1);
			state.owners[r] = std::move(a_owner);
			RequestFadeSeed(a_context, roots, r, row);
			roots.fadeRoots[r] = row;
			NoteRootWrite(state, r, FadeRootWriter::Listed);
			if (const auto* record = a_context.Mirror().Node(a_node); record && record->treeLodSwitch)
				roots.fadeRootSwitch.insert_or_assign(record->treeLodSwitch, r);
			family.Mark(r);
			return r;
		}

		/** @brief Lists the fade node a written slot names, or unlists the slot if it names none. */
		void ListFadeRoot(const Api::CoordinatorComponentContext& a_context, std::uint32_t a_slot)
		{
			const auto* geometry = a_context.ObjectGeometry(a_slot);
			const auto* property = geometry ? a_context.Mirror().Leaf(geometry).property : nullptr;
			const auto* node = property ? static_cast<const RE::NiAVObject*>(property->fadeNode) : nullptr;
			// The engine's shared empty fade node sits outside the scene graph and serves thousands of geometries.
			// Treating it as a root would make FadeStateCS and the write-back write a static engine node.
			if (node == Engine::EmptyFadeNode())
				node = nullptr;
			const auto live = LiveRootsOf(a_context);
			auto& [family, roots] = live;
			// A new root is held through the entry's pinned reference, never the raw key. No reference means no root.
			RE::NiPointer<RE::NiAVObject> owner;
			if (node && !roots.fadeRootIndex.contains(node) && !(owner = a_context.HoldFadeNode(a_slot, node)))
				node = nullptr;
			if (family.objectRow.size() <= a_slot) {
				family.NoteWrite();
				family.objectRow.resize(std::size_t(a_slot) + 1, kNoFadeRoot);
			}
			if (const std::uint32_t current = family.objectRow[a_slot]; node && current < roots.fadeRootNode.size() && roots.fadeRootNode[current] == node)
				return;
			if (!node && family.objectRow[a_slot] == kNoFadeRoot)
				return;
			family.NoteWrite();
			UnlistFadeRoot(a_context, a_slot);
			if (!node)
				return;
			auto [it, inserted] = roots.fadeRootIndex.try_emplace(node, 0u);
			if (inserted)
				it->second = ListNewRoot(a_context, live, node, a_slot, std::move(owner));
			++roots.fadeRootRefs[it->second];
			a_context.State<FadeRootState>().members.Add(it->second, a_slot);
			family.objectRow[a_slot] = it->second;
			// Shadow inputs read the slot's root.
			a_context.tables.NoteChange(a_slot, kChangeBindings);
		}

		void MarkFadeRootOwned(const LiveRoots& a_live, const void* a_node, bool a_owned, bool a_standIn)
		{
			auto& [family, roots] = a_live;
			const auto it = roots.fadeRootIndex.find(a_node);
			if (it == roots.fadeRootIndex.end())
				return;
			auto& row = roots.fadeRoots[it->second];
			// The GPU has owned the state since listing, so only the stood-in bit changes here.
			row.bits = (row.bits & ~kFadeRootStoodIn) | kFadeRootOwned | kFadeRootWritten | (a_owned && a_standIn ? kFadeRootStoodIn : 0u);
			family.Mark(it->second);
		}

		/** @brief Takes the primary cull's full set of admitted roots and updates the rows that changed. */
		void ApplyAdmitted(const Api::CoordinatorComponentContext& a_context, std::span<const Api::StandInRoot> a_roots)
		{
			const auto live = LiveRootsOf(a_context);
			auto& state = a_context.State<FadeRootState>();
			ankerl::unordered_dense::map<const void*, bool> owned;
			for (const auto& root : a_roots)
				owned.insert_or_assign(root.node, root.standIn);
			for (const auto& [node, standIn] : state.owned)
				if (!owned.contains(node))
					MarkFadeRootOwned(live, node, false, false);
			for (const auto& [node, standIn] : owned)
				if (const auto it = state.owned.find(node); it == state.owned.end() || it->second != standIn)
					MarkFadeRootOwned(live, node, true, standIn);
			state.owned = std::move(owned);
		}

		/** @brief A placement snap. Rebuilds the row in a new generation, so the GPU state restarts from the node. */
		void ReseedFadeRoot(const Api::CoordinatorComponentContext& a_context, const void* a_node)
		{
			auto [family, roots] = LiveRootsOf(a_context);
			const auto it = roots.fadeRootIndex.find(a_node);
			if (it == roots.fadeRootIndex.end())
				return;
			auto& row = roots.fadeRoots[it->second];
			const std::uint32_t object = row.object;
			const std::uint32_t kept = row.bits & (kFadeRootOwned | kFadeRootStoodIn | kFadeRootSeedOdd | kFadeRootWritten);
			row = FadeRootFromMirror(a_context, a_node);
			row.object = object;
			row.bits |= kept;
			RequestFadeSeed(a_context, roots, it->second, row);
			family.Mark(it->second);
			auto& state = a_context.State<FadeRootState>();
			++state.reseeds;
			NoteRootWrite(state, it->second, FadeRootWriter::Reseed);
		}

		/** @brief A fadeAmount event from Actor::SetAlpha. Updates the row's input and keeps the GPU state. */
		void RefreshFadeAmount(const Api::CoordinatorComponentContext& a_context, const void* a_node)
		{
			auto [family, roots] = LiveRootsOf(a_context);
			auto& state = a_context.State<FadeRootState>();
			// Take the recheck first so one for an unlisted node does not linger.
			const bool recheck = Features::Fades::TakeRecheck(a_node);
			++state.amountEvents;
			state.rechecks += recheck ? 1u : 0u;
			const auto it = roots.fadeRootIndex.find(a_node);
			if (it == roots.fadeRootIndex.end()) {
				++state.amountNotListed;
				return;
			}
			auto& row = roots.fadeRoots[it->second];
			// The mirror already has the new value, applied before this batch's structural events.
			const auto* record = a_context.Mirror().Node(a_node);
			if (!record) {
				++state.amountNoMirror;
				return;
			}
			const float amount = record->fadeAmount;
			if (std::memcmp(&amount, &row.fadeAmount, sizeof(amount)) == 0) {
				++state.amountUnchanged;
				return;
			}
			// An input, not state. FadeStateCS steps amountFade toward it as the engine does.
			row.fadeAmount = amount;
			family.Mark(it->second);
			++state.amountChanged;
			state.recheckChanged += recheck ? 1u : 0u;
			NoteRootWrite(state, it->second, recheck ? FadeRootWriter::Recheck : FadeRootWriter::EngineEvent);
		}

		void ApplyFadeKeys(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, std::span<const void* const> a_keys)
		{
			if (a_channel == FadeSnapsChannelId()) {
				for (const void* node : a_keys)
					ReseedFadeRoot(a_context, node);
			} else if (a_channel == FadeAmountsChannelId()) {
				for (const void* node : a_keys)
					RefreshFadeAmount(a_context, node);
			}
		}

		/** @brief A tree root's LOD switch changed. */
		void RefreshFadeRootSwitch(const Api::CoordinatorComponentContext& a_context, const void* a_switch)
		{
			auto [family, roots] = LiveRootsOf(a_context);
			const auto it = roots.fadeRootSwitch.find(a_switch);
			if (it == roots.fadeRootSwitch.end() || it->second >= roots.fadeRoots.size() || !roots.fadeRootNode[it->second])
				return;
			auto& row = roots.fadeRoots[it->second];
			const auto* lodSwitch = a_context.Mirror().Node(a_switch);
			const bool selected = lodSwitch && lodSwitch->switchIndex > 0;
			if (selected == ((row.bits & kFadeRootTreeLod) != 0))
				return;
			// An input, not state. No new generation, so the GPU state row is kept.
			row.bits = selected ? (row.bits | kFadeRootTreeLod) : (row.bits & ~kFadeRootTreeLod);
			family.Mark(it->second);
		}

		// Node references go back through the retirement chain. Fades registers last so this runs after every other component's reset.
		void Reset(const Api::CoordinatorComponentContext& a_context)
		{
			auto& state = a_context.State<FadeRootState>();
			a_context.HandBack(state.owners);
			state.members.Clear();
		}
	}

	namespace
	{
		// Install; read-only after.
		Api::TableFamilyId fadeRootFamily;
		Api::CoordinatorComponentId fadeRootComponent;

		// The root reference is stored as root + 1 in bits 9-31 of recordFlags. Core's bits stay below.
		constexpr std::uint32_t kFadeRootRecordWord = static_cast<std::uint32_t>(offsetof(BindlessObject, recordFlags) / 4);
		static_assert(kRecordFadeRootShift == 9, "the fade roots' record reference is bits 9-31 of recordFlags");
	}

	Api::TableFamilyId FadeRootFamilyId()
	{
		return fadeRootFamily;
	}

	Api::CoordinatorComponentId FadeRootComponentId()
	{
		return fadeRootComponent;
	}

	std::shared_ptr<void> MakeFadeRoots()
	{
		return std::make_shared<FadeRootTables>();
	}

	void AssignFadeRoots(void* a_to, const void* a_from)
	{
		*static_cast<FadeRootTables*>(a_to) = *static_cast<const FadeRootTables*>(a_from);
	}

	void AssignFadeRootRow(void* a_to, const void* a_from, std::uint32_t a_row)
	{
		// Only the row column is copied per row. The other columns change under NoteWrite, which copies the whole family.
		auto& to = *static_cast<FadeRootTables*>(a_to);
		const auto& from = *static_cast<const FadeRootTables*>(a_from);
		if (a_row < to.fadeRoots.size() && a_row < from.fadeRoots.size())
			to.fadeRoots[a_row] = from.fadeRoots[a_row];
		to.fadeRootGenerations = from.fadeRootGenerations;
	}

	bool SameFadeRoots(const void* a_left, const void* a_right)
	{
		const auto& x = *static_cast<const FadeRootTables*>(a_left);
		const auto& y = *static_cast<const FadeRootTables*>(a_right);
		return SameRows(x.fadeRoots, y.fadeRoots) && SameRows(x.fadeRootRefs, y.fadeRootRefs) && SameRows(x.fadeRootFree, y.fadeRootFree) &&
		       SameRows(x.fadeRootNode, y.fadeRootNode) && SameIndex(x.fadeRootIndex, y.fadeRootIndex) && SameIndex(x.fadeRootSwitch, y.fadeRootSwitch);
	}

	void ClearFadeRoots(void* a_columns)
	{
		// Generations keep counting, because seeds are told apart by generation.
		auto& roots = *static_cast<FadeRootTables*>(a_columns);
		roots.fadeRoots.clear();
		roots.fadeRootRefs.clear();
		roots.fadeRootFree.clear();
		roots.fadeRootNode.clear();
		roots.fadeRootIndex.clear();
		roots.fadeRootSwitch.clear();
	}

	void RecycleFadeRoot(void* a_columns, std::uint32_t a_row)
	{
		static_cast<FadeRootTables*>(a_columns)->fadeRootFree.push_back(a_row);
	}

	std::uint32_t FadeRootRows(const void* a_columns)
	{
		return static_cast<std::uint32_t>(static_cast<const FadeRootTables*>(a_columns)->fadeRoots.size());
	}

	void ViewFadeRoots(const void* a_columns, Api::RootListView& a_out)
	{
		const auto& roots = *static_cast<const FadeRootTables*>(a_columns);
		a_out.rows = roots.fadeRoots;
		a_out.nodes = roots.fadeRootNode;
		a_out.index = &roots.fadeRootIndex;
	}

	Api::CoordinatorComponent FadeRootComponent()
	{
		return { .name = "fade roots",
			.make = &MakeComponent,
			.slots = { .written = &ListFadeRoot, .released = &UnlistFadeRoot },
			.retirement = { .reset = &Reset },
			.events = { .keys = &ApplyFadeKeys, .switched = &RefreshFadeRootSwitch, .admitted = &ApplyAdmitted } };
	}

	void OwnerLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text)
	{
		// Nodes that only these owners keep alive.
		Diagnostics::SoleHolds owners;
		for (const auto& owner : a_context.State<FadeRootState>().owners)
			owners.Add(owner.get());
		const auto& roots = LiveRootsOf(a_context).roots;
		a_text += fmt::format("[DCLF] fade roots: {}; {} listed by node, {} rows ({} free)", owners.Text("owners"), roots.fadeRootIndex.size(), roots.fadeRoots.size(),
			roots.fadeRootFree.size());
	}

	std::size_t FadeRootsStoodIn()
	{
		if (!liveState)
			return 0;
		return static_cast<std::size_t>(std::count_if(liveState->owned.begin(), liveState->owned.end(), [](const auto& a_root) { return a_root.second; }));
	}

	std::string_view FadeRootWriterName(FadeRootWriter a_writer)
	{
		switch (a_writer) {
		case FadeRootWriter::Listed:
			return "listed";
		case FadeRootWriter::EngineEvent:
			return "engine event";
		case FadeRootWriter::Recheck:
			return "recheck";
		case FadeRootWriter::Reseed:
			return "reseed";
		case FadeRootWriter::WriteBack:
			return "write-back";
		default:
			return "none";
		}
	}

	namespace
	{
		/** @brief Plain data, so the read can sit behind an SEH guard. */
		struct ReferenceIdentity
		{
			std::uint32_t formId = 0;
			const char* name = nullptr;  // static form data
			bool actor = false;
			bool has3D = false;  // the reference's 3D is this node
			bool read = false;   // false if the node is detached or the read faulted
		};

		/**
		 * @brief Reads the node's reference only while attached. A detached node can outlive its reference because fade roots own nodes.
		 * Diagnostics only. A fault leaves read false.
		 */
		ReferenceIdentity ReadReferenceIdentity(const RE::NiAVObject* a_node)
		{
			ReferenceIdentity identity;
			__try {
				if (a_node->parent) {
					if (const auto* reference = a_node->GetUserData()) {
						identity.formId = reference->GetFormID();
						if (const auto* base = reference->GetBaseObject())
							identity.name = base->GetName();
						identity.actor = reference->IsActor();
						identity.has3D = reference->Get3D() == a_node;
						identity.read = true;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				identity.read = false;
				identity.name = nullptr;
			}
			return identity;
		}

		std::string_view PlanName(std::uint32_t a_bits)
		{
			switch (a_bits & kFadeRootPlanMask) {
			case kFadeRootFade:
				return "fade";
			case kFadeRootLeaf:
				return "leaf";
			case kFadeRootTree:
				return "tree";
			default:
				return "other";
			}
		}

		json BitNames(std::uint32_t a_bits)
		{
			json names = json::array();
			constexpr std::pair<std::uint32_t, const char*> kBits[] = { { kFadeRootOwned, "Owned" }, { kFadeRootStoodIn, "StoodIn" }, { kFadeRootWritten, "Written" },
				{ kFadeRootSeedOdd, "SeedOdd" }, { kFadeRootTreeLod, "TreeLod" }, { kFadeRootAlwaysDraw, "AlwaysDraw" } };
			for (const auto& [bit, name] : kBits)
				if (a_bits & bit)
					names.push_back(name);
			return names;
		}

		json FloatOrNull(bool a_has, float a_value)
		{
			return a_has && std::isfinite(a_value) ? json(a_value) : json(nullptr);
		}
	}

	void FadeRootsCensus(const Api::CoordinatorComponentContext& a_context, std::string& a_text)
	{
		constexpr std::size_t kMaxRoots = 2000;
		const auto& state = a_context.State<FadeRootState>();
		const auto list = Extensions::RootListOf(a_context.tables);
		const auto& mirror = a_context.Mirror();
		const auto writeBack = Features::Fades::PeekWriteBackCensus(true);
		RE::NiPoint3 player{};
		bool havePlayer = false;
		if (auto* character = RE::PlayerCharacter::GetSingleton()) {
			player = character->GetPosition();
			havePlayer = true;
		}
		// Roots that are fading or whose copies disagree. Actors first, then by distance.
		struct Candidate
		{
			std::uint32_t root = 0;
			float distance = 0.0f;
			ReferenceIdentity identity;
		};
		std::vector<Candidate> candidates;
		std::size_t listed = 0;
		for (std::uint32_t r = 0; r < list.rows.size() && r < list.nodes.size(); ++r) {
			const auto* node = static_cast<const RE::NiAVObject*>(list.nodes[r]);
			if (!node)
				continue;
			++listed;
			const auto& row = list.rows[r];
			// Listed nodes are held, so the read is safe. The values may be mid-write.
			const float nodeAmount = Engine::At<float>(node, 0x100);
			const float current = Engine::At<float>(node, 0x130);
			const auto* record = mirror.Node(node);
			const bool differs = row.fadeAmount != nodeAmount || (record && record->fadeAmount != nodeAmount);
			if (!(nodeAmount < 1.0f || current < 1.0f || differs))
				continue;
			const float distance = havePlayer ? node->world.translate.GetDistance(player) : -1.0f;
			candidates.push_back({ r, distance, ReadReferenceIdentity(node) });
		}
		std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
			if (a.identity.actor != b.identity.actor)
				return a.identity.actor;
			return a.distance < b.distance;
		});
		json roots = json::array();
		for (std::size_t i = 0; i < candidates.size() && i < kMaxRoots; ++i) {
			const auto& candidate = candidates[i];
			const std::uint32_t r = candidate.root;
			const auto* node = static_cast<const RE::NiAVObject*>(list.nodes[r]);
			const auto& row = list.rows[r];
			const auto* record = mirror.Node(node);
			const auto sceneWriter = r < state.lastWriter.size() ? static_cast<FadeRootWriter>(state.lastWriter[r]) : FadeRootWriter::None;
			const std::uint32_t sceneFrame = r < state.lastWriteFrame.size() ? state.lastWriteFrame[r] : 0u;
			const std::uint32_t appliedFrame = r < writeBack.appliedFrame.size() ? writeBack.appliedFrame[r] : 0u;
			const bool writeBackLast = appliedFrame && appliedFrame >= sceneFrame;
			const auto& identity = candidate.identity;
			json entry = json::object();
			entry["root"] = r;
			entry["formId"] = identity.read ? fmt::format("{:08X}", identity.formId) : std::string();
			entry["name"] = identity.read && identity.name ? std::string(identity.name) : std::string();
			entry["actor"] = identity.actor;
			entry["attached"] = node->parent != nullptr;
			entry["has3D"] = identity.has3D;
			entry["distance"] = candidate.distance;
			entry["node"] = { { "fadeAmount", FloatOrNull(true, Engine::At<float>(node, 0x100)) }, { "currentFade", FloatOrNull(true, Engine::At<float>(node, 0x130)) },
				{ "amountFade", FloatOrNull(true, Engine::At<float>(node, 0x140)) } };
			entry["mirrorFadeAmount"] = FloatOrNull(record != nullptr, record ? record->fadeAmount : 0.0f);
			entry["row"] = { { "fadeAmount", FloatOrNull(true, row.fadeAmount) }, { "seedCurrentFade", FloatOrNull(true, row.initial.currentFade) },
				{ "generation", row.generation }, { "object", row.object }, { "plan", PlanName(row.bits) }, { "bits", BitNames(row.bits) } };
			entry["lastWriter"] = writeBackLast ? FadeRootWriterName(FadeRootWriter::WriteBack) : FadeRootWriterName(sceneWriter);
			entry["sceneWriter"] = FadeRootWriterName(sceneWriter);
			entry["sceneWriteFrame"] = sceneFrame;
			entry["appliedFrame"] = appliedFrame;
			roots.push_back(std::move(entry));
		}
		const auto writers = Features::Fades::PeekFadeWriterCounts();
		const auto seeds = Features::Fades::PeekSeedCounts();
		json totals = json::object();
		totals["rootsListed"] = listed;
		totals["rootsFading"] = candidates.size();
		totals["truncated"] = candidates.size() > kMaxRoots ? candidates.size() - kMaxRoots : 0;
		totals["stoodIn"] = FadeRootsStoodIn();
		totals["amountEvents"] = { { "all", state.amountEvents }, { "notListed", state.amountNotListed }, { "refreshNoMirror", state.amountNoMirror },
			{ "unchanged", state.amountUnchanged }, { "changed", state.amountChanged }, { "rechecks", state.rechecks }, { "recheckChanged", state.recheckChanged } };
		totals["reseeds"] = state.reseeds;
		totals["writers"] = { { "gateSkipped", writers.gateSkipped }, { "onVisibleSkipped", writers.onVisibleSkipped }, { "roots", writers.roots }, { "sinceLastReport", true } };
		totals["seeds"] = { { "unsent", seeds.unsent }, { "owed", seeds.owed } };
		if (writeBack.present)
			totals["writeBack"] = { { "applied", writeBack.applied }, { "stale", writeBack.stale }, { "busy", writeBack.busy }, { "notWritten", writeBack.notWritten },
				{ "dropped", writeBack.dropped }, { "harvested", writeBack.harvested }, { "harvestedWithEvents", writeBack.harvestedWithEvents }, { "events", writeBack.events },
				{ "overflowed", writeBack.overflowed }, { "missed", writeBack.missed }, { "setsAdded", writeBack.setsAdded }, { "pendingBatches", writeBack.pendingBatches },
				{ "readyStores", writeBack.readyStores }, { "sinceLastReport", true } };
		else
			totals["writeBack"] = nullptr;
		json census = json::object();
		census["roots"] = std::move(roots);
		census["totals"] = std::move(totals);
		a_text += census.dump();
	}

	void FadeRootsGrowth(const Api::CoordinatorComponentContext& a_context, std::string& a_text)
	{
		const auto& state = a_context.State<FadeRootState>();
		const auto list = Extensions::RootListOf(a_context.tables);
		std::size_t owners = 0;
		for (const auto& owner : state.owners)
			owners += owner ? 1u : 0u;
		std::size_t memberLists = 0;
		for (const auto& members : state.members.lists)
			memberLists += members.empty() ? 0u : 1u;
		a_text += fmt::format("fade roots rows {}\n", list.rows.size());
		a_text += fmt::format("fade roots listed {}\n", list.index ? list.index->size() : 0u);
		a_text += fmt::format("fade roots owned {}\n", state.owned.size());
		a_text += fmt::format("fade roots owners {} (slots {})\n", owners, state.owners.size());
		a_text += fmt::format("fade roots member lists {} (capacity {})\n", memberLists, state.members.lists.size());
		a_text += fmt::format("fade roots writer tags {}\n", state.lastWriter.size());
	}
}

namespace DCLF::Features::Fades
{
	void RegisterScene(Api::InstallContext& a_context)
	{
		// The record reference has 23 bits. No tag is needed because each row carries its generation.
		Scene::fadeRootFamily = a_context.RegisterTableFamily({ .name = "fade roots",
			.make = &Scene::MakeFadeRoots,
			.assign = &Scene::AssignFadeRoots,
			.assignRow = &Scene::AssignFadeRootRow,
			.same = &Scene::SameFadeRoots,
			.clear = &Scene::ClearFadeRoots,
			.recycle = &Scene::RecycleFadeRoot,
			.recordWord = Scene::kFadeRootRecordWord,
			.recordShift = kRecordFadeRootShift,
			.recordBias = 1,
			.recordTag = false,
			.journaled = true,
			.rows = &Scene::FadeRootRows });
		// Must be the last component, so its reset hands owners back after every other reset.
		Scene::fadeRootComponent = a_context.RegisterCoordinatorComponent(Scene::FadeRootComponent());
		a_context.RegisterReportLines({ .name = "fade root owners", .point = Api::ReportPoint::SceneCoordinatorLastPass, .component = Scene::fadeRootComponent,
			.componentText = &Scene::OwnerLines });
		a_context.RegisterReportLines({ .name = "fade roots", .point = Api::ReportPoint::CaptureCensus, .component = Scene::fadeRootComponent,
			.componentText = &Scene::FadeRootsCensus });
		a_context.RegisterReportLines({ .name = "fade roots", .point = Api::ReportPoint::CaptureGrowth, .component = Scene::fadeRootComponent,
			.componentText = &Scene::FadeRootsGrowth });
	}
}
