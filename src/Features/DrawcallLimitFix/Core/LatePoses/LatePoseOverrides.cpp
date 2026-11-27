// The late poses' search, plans and rows.

#include "Features/DrawcallLimitFix/Core/LatePoses/LatePoseOverrides.h"

#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LatePoses.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <algorithm>
#include <cstddef>
#include <ankerl/unordered_dense.h>
#include <atomic>
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Snapshot/EngineReads.h"

namespace DCLF::LatePoses
{
	namespace
	{
		// A node is hidden when its world scale is below this share of its parent's. Improved Camera uses 0.001, and anything this small draws nothing.
		constexpr float kHiddenRatio = 0.01f;
		// A factor at or above this is no shrink: the node was already as small at the frame's start, so the sampled rows are already shrunk.
		constexpr float kNoShrink = 0.5f;
		// Most nodes a frame may hide. A plan's scale index is 16 bits, with 0 meaning none.
		constexpr std::size_t kMaxHidden = 64;
		using BoneEntry = RE::BSFlattenedBoneTree::BoneEntry;

		// One search's result, holding the engine objects it names. Released on the render thread when replaced.
		struct Search
		{
			// Per hidden bone: the node holding it, its world transform, and its world scale as found.
			// A bone that is only an entry in a flattened bone tree uses the tree node and the entry's transform.
			std::vector<RE::NiPointer<RE::NiAVObject>> nodes;
			std::vector<const RE::NiTransform*> worlds;
			std::vector<float> worldScales;
			// Per hidden bone, the tree entry array its world points into, or null for a node.
			// The plan holds the tree, not the array, so the pointer is only used while the tree still has that array.
			std::vector<const BoneEntry*> entryArrays;
			struct Skin
			{
				RE::NiPointer<RE::BSGeometry> geometry;
				std::vector<std::uint16_t> scaleOf;
			};
			std::vector<Skin> skins;
			std::vector<std::pair<RE::NiPointer<RE::BSGeometry>, std::uint16_t>> rigid;
			bool rooted = false;  // a root was named

			bool Empty() const { return nodes.empty(); }
		};

		// Render thread.
		struct State
		{
			std::uint64_t frame = 0;    // PrepareFrame's count
			Search last;                // the newest search
			std::uint64_t lastFrame = 0;  // the frame it was taken in
			Search inUse;               // the search the frame's plan came from. Its objects outlive the frame's producer.
			bool planned = false;       // this frame has a plan
			bool searchedEmpty = false;  // this frame's depth commit named a root and found nothing hidden
		};
		State& S()
		{
			static State state;
			return state;
		}

		// Any thread.
		struct Counters
		{
			std::atomic<std::uint64_t> rooted{ 0 }, hidden{ 0 }, planned{ 0 }, confirmed{ 0 }, refused{ 0 }, unplanned{ 0 }, afterDepth{ 0 };
			std::atomic<std::uint64_t> skins{ 0 }, rigid{ 0 }, missing{ 0 }, maxNodes{ 0 };
		};
		Counters counters;

		void NoteMax(std::atomic<std::uint64_t>& a_max, std::uint64_t a_value)
		{
			for (auto max = a_max.load(std::memory_order_relaxed); a_value > max && !a_max.compare_exchange_weak(max, a_value, std::memory_order_relaxed);) {}
		}

		bool Hidden(const RE::NiAVObject& a_node)
		{
			const auto* parent = a_node.parent;
			return parent && parent->world.scale > 1e-4f && a_node.world.scale < kHiddenRatio * parent->world.scale;
		}

		// Every outermost hidden node under a_root, the skins with bones under them, and the unskinned geometry under them.
		void SearchRoot(RE::NiAVObject* a_root, Search& a_out)
		{
			// First the nodes: each node's hidden ancestor as index + 1, and the geometry met on the way.
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint16_t> hiddenOf;
			std::vector<std::pair<RE::BSGeometry*, std::uint16_t>> geometry;
			// Flattened bone trees, such as an actor's skeleton. Most of their bones are array entries with no node, so the node walk cannot see them hidden.
			struct Tree
			{
				RE::BSFlattenedBoneTree* node = nullptr;
				const BoneEntry* entries = nullptr;
				std::uint32_t count = 0;
				std::uint16_t hidden = 0;            // the tree node's own hidden ancestor
				std::vector<std::uint16_t> hiddenOf;  // per entry
			};
			std::vector<Tree> trees;
			std::vector<std::pair<RE::NiAVObject*, std::uint16_t>> stack{ { a_root, std::uint16_t{ 0 } } };
			while (!stack.empty()) {
				auto [object, hidden] = stack.back();
				stack.pop_back();
				if (!object)
					continue;
				if (auto* shape = object->AsGeometry()) {
					geometry.emplace_back(shape, hidden);
					continue;
				}
				auto* node = object->AsNode();
				if (!node)
					continue;
				if (!hidden && object != a_root && a_out.nodes.size() < kMaxHidden)
					if (Hidden(*object)) {
						a_out.nodes.emplace_back(object);
						a_out.worlds.push_back(&object->world);
						a_out.entryArrays.push_back(nullptr);
						a_out.worldScales.push_back(object->world.scale);
						hidden = static_cast<std::uint16_t>(a_out.nodes.size());
					}
				if (hidden)
					hiddenOf.emplace(object, hidden);
				if (auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(object)) {
					// numBones is the entry array's length. numPopulatedBones is not used, since what it counts is unconfirmed.
					const auto& data = tree->GetRuntimeData();
					if (data.boneEntries && data.numBones)
						trees.push_back({ tree, data.boneEntries, data.numBones, hidden, {} });
				}
				for (auto& child : Engine::Children(*node))
					stack.emplace_back(child.get(), hidden);
			}
			bool anyTreeHidden = false;
			for (auto& tree : trees) {
				tree.hiddenOf.assign(tree.count, 0);
				// Entries are in parent-first order, which the tree's update relies on. A parent index not below the entry's own means a top bone under the tree node.
				for (std::uint32_t i = 0; i < tree.count; ++i) {
					const auto& entry = tree.entries[i];
					const std::int16_t parent = entry.parentIndex;
					const bool parentEntry = parent >= 0 && static_cast<std::uint32_t>(parent) < i;
					const std::uint16_t inherited = parentEntry ? tree.hiddenOf[parent] : tree.hidden;
					const float parentScale = parentEntry ? tree.entries[parent].world.scale : tree.node->world.scale;
					std::uint16_t hidden = inherited;
					if (!hidden && parentScale > 1e-4f && entry.world.scale < kHiddenRatio * parentScale) {
						// A bone with a node found hidden takes that node's index. One without takes a new one on the tree.
						const auto* node = entry.node;
						const auto known = std::ranges::find_if(a_out.nodes, [&](const auto& a_node) { return node && a_node.get() == node; });
						if (known != a_out.nodes.end())
							hidden = static_cast<std::uint16_t>(known - a_out.nodes.begin() + 1);
						else if (a_out.nodes.size() < kMaxHidden) {
							a_out.nodes.emplace_back(tree.node);
							a_out.worlds.push_back(&entry.world);
							a_out.worldScales.push_back(entry.world.scale);
							a_out.entryArrays.push_back(tree.entries);
							hidden = static_cast<std::uint16_t>(a_out.nodes.size());
						}
					}
					tree.hiddenOf[i] = hidden;
					anyTreeHidden |= hidden != 0;
				}
			}
			if (hiddenOf.empty() && !anyTreeHidden)
				return;
			// Then the geometry: a skin by its bones, since any skin may have bones in a hidden subtree, and the rest whole.
			for (auto [shape, hidden] : geometry) {
				auto* skin = shape->GetGeometryRuntimeData().skinInstance.get();
				const auto* data = skin ? skin->skinData.get() : nullptr;
				if (!skin || !data || !skin->bones) {
					if (hidden && !skin)
						a_out.rigid.emplace_back(RE::NiPointer<RE::BSGeometry>(shape), hidden);
					continue;
				}
				const std::uint32_t bones = data->GetBoneCount();
				std::vector<std::uint16_t> scaleOf(bones, 0);
				bool any = false;
				// A tree bone is known by its world transform's address, since skin bone pointers point into the tree's entries. Any other by its node.
				const auto* const* worlds = skin->boneWorldTransforms;
				for (std::uint32_t b = 0; b < bones; ++b) {
					const auto* world = reinterpret_cast<const std::byte*>(worlds ? worlds[b] : nullptr);
					std::size_t entry = 0;
					const auto tree = std::ranges::find_if(trees, [&](const Tree& a_tree) {
						const auto* first = reinterpret_cast<const std::byte*>(&a_tree.entries[0].world);
						if (world < first || world >= first + std::size_t(a_tree.count) * sizeof(BoneEntry) || std::size_t(world - first) % sizeof(BoneEntry))
							return false;
						entry = std::size_t(world - first) / sizeof(BoneEntry);
						return true;
					});
					if (tree != trees.end())
						scaleOf[b] = tree->hiddenOf[entry];
					else if (const auto it = hiddenOf.find(skin->bones[b]); it != hiddenOf.end())
						scaleOf[b] = it->second;
					any |= scaleOf[b] != 0;
				}
				if (any)
					a_out.skins.push_back({ RE::NiPointer<RE::BSGeometry>(shape), std::move(scaleOf) });
			}
		}

		Search SearchRoots()
		{
			Search search;
			// No walk while a load shows. A load rebuilds the actors' 3D, the player's too, so a walk can read a freed child and crash.
			// The player sees none of those frames, so there is nothing to hide.
			if (Scene::FrameView::LoadShown())
				return search;
			for (const auto& pose : Extensions::LatePoses())
				if (const void* root = pose.root ? pose.root() : nullptr) {
					CS_ENGINE_READ_MARK(Scene, RecordTime);
					search.rooted = true;
					SearchRoot(const_cast<RE::NiAVObject*>(static_cast<const RE::NiAVObject*>(root)), search);
				}
			return search;
		}

		// x' = center + factor * (x - center) on a 3x4 transform of three float4 rows, translation in w.
		void ScaleRows(float* a_rows, const FramePlan::Scale& a_scale)
		{
			for (int row = 0; row < 3; ++row) {
				float* r = a_rows + row * 4;
				r[0] *= a_scale.factor;
				r[1] *= a_scale.factor;
				r[2] *= a_scale.factor;
				r[3] = a_scale.center[row] + a_scale.factor * (r[3] - a_scale.center[row]);
			}
		}
	}

	std::shared_ptr<const FramePlan> PrepareFrame()
	{
		auto& s = S();
		const std::uint64_t previous = s.frame++;
		s.planned = false;
		s.searchedEmpty = false;
		// Release the last plan's objects. Its producer is done, because the frame before presented after it.
		s.inUse = {};
		if (s.last.Empty() || s.lastFrame != previous) {
			s.last = {};
			return nullptr;
		}
		s.inUse = std::move(s.last);
		s.last = {};
		// Scales about each hidden node's world position at the frame's start, by the share of its current world scale it was hidden to.
		// A node already that small shrinks nothing, and its scale index stays 0.
		auto plan = std::make_shared<FramePlan>();
		std::vector<std::uint16_t> index(s.inUse.nodes.size() + 1, 0);
		for (std::size_t i = 0; i < s.inUse.nodes.size(); ++i) {
			// Use a tree entry's world only while the tree still has the array it was found in. A rebuilt skeleton gets a new one.
			if (const auto* entries = s.inUse.entryArrays[i]) {
				const auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(s.inUse.nodes[i].get());
				if (!tree || tree->GetRuntimeData().boneEntries != entries)
					continue;
			}
			const auto& world = *s.inUse.worlds[i];
			const float factor = world.scale > 1e-6f ? s.inUse.worldScales[i] / world.scale : 1.0f;
			if (!(factor < kNoShrink))
				continue;
			const auto& at = world.translate;
			plan->scales.push_back({ { at.x, at.y, at.z }, factor });
			index[i + 1] = static_cast<std::uint16_t>(plan->scales.size());
		}
		for (const auto& skin : s.inUse.skins) {
			FramePlan::Skin planned{ skin.geometry.get(), static_cast<std::uint32_t>(skin.scaleOf.size() * SkinPalettes::kRowsPerBone), skin.scaleOf };
			bool any = false;
			for (auto& scale : planned.scaleOf)
				any |= (scale = index[scale]) != 0;
			if (any)
				plan->skins.push_back(std::move(planned));
		}
		for (const auto& [shape, scale] : s.inUse.rigid)
			if (index[scale])
				plan->rigid.push_back({ shape.get(), index[scale] });
		if (plan->skins.empty() && plan->rigid.empty())
			return nullptr;
		s.planned = true;
		++counters.planned;
		return plan;
	}

	bool ConfirmFrame()
	{
		auto& s = S();
		auto search = SearchRoots();
		if (search.rooted)
			++counters.rooted;
		if (!search.Empty()) {
			++counters.hidden;
			NoteMax(counters.maxNodes, search.nodes.size());
		}
		s.searchedEmpty = search.rooted && search.Empty();
		// The plan holds only if each node it shrinks is still shrunk. Otherwise the plugin stopped hiding one, and the frame draws its own rows.
		bool holds = s.planned;
		for (std::size_t i = 0; holds && i < s.inUse.worlds.size(); ++i)
			holds = std::ranges::find(search.worlds, s.inUse.worlds[i]) != search.worlds.end();
		if (s.planned)
			++(holds ? counters.confirmed : counters.refused);
		else if (!search.Empty())
			++counters.unplanned;
		s.last = std::move(search);
		s.lastFrame = s.frame;
		return holds;
	}

	void CheckAtPresent()
	{
		auto& s = S();
		if (!std::exchange(s.searchedEmpty, false))
			return;
		if (!SearchRoots().Empty())
			++counters.afterDepth;
	}

	void BuildRows(const FramePlan& a_plan, const std::function<bool(const void*, Location&)>& a_find, const std::vector<BindlessPlacement>& a_rows,
		const std::vector<FrameValues::PaletteRow>& a_palettes, Rows& a_out)
	{
		a_out.Clear();
		const auto scaleOf = [&](std::uint16_t a_index) -> const FramePlan::Scale* {
			return a_index && a_index <= a_plan.scales.size() ? &a_plan.scales[a_index - 1] : nullptr;
		};
		for (const auto& skin : a_plan.skins) {
			Location at;
			if (!a_find(skin.geometry, at) || at.boneRows != skin.boneRows) {
				++counters.missing;
				continue;
			}
			const auto block = PaletteRowsOf(at.boneOffset, at.boneRows);
			if (std::size_t(block.current) + 2ull * at.boneRows > a_palettes.size()) {
				++counters.missing;
				continue;
			}
			// The whole block, current then previous, with its hidden bones shrunk in both.
			const std::size_t first = a_out.palettes.size();
			a_out.palettes.insert(a_out.palettes.end(), a_palettes.begin() + block.current, a_palettes.begin() + block.current + 2ull * at.boneRows);
			for (std::size_t b = 0; b < skin.scaleOf.size(); ++b)
				if (const auto* scale = scaleOf(skin.scaleOf[b])) {
					ScaleRows(a_out.palettes[first + b * SkinPalettes::kRowsPerBone].v, *scale);
					ScaleRows(a_out.palettes[first + at.boneRows + b * SkinPalettes::kRowsPerBone].v, *scale);
				}
			a_out.paletteRuns.emplace_back(block.current, 2 * at.boneRows);
			++counters.skins;
		}
		for (const auto& rigid : a_plan.rigid) {
			Location at;
			const auto* scale = scaleOf(rigid.scale);
			if (!scale || !a_find(rigid.geometry, at)) {
				++counters.missing;
				continue;
			}
			for (const auto slot : { at.slot, at.layerSlot }) {
				if (slot == kNoObjectSlot || slot >= a_rows.size())
					continue;
				auto row = a_rows[slot];
				ScaleRows(row.world, *scale);
				ScaleRows(row.previousWorld, *scale);
				a_out.placements.push_back(row);
				a_out.placementSlots.push_back(slot);
			}
			++counters.rigid;
		}
	}

	std::string TakeReport()
	{
		auto& c = counters;
		const auto rooted = c.rooted.exchange(0);
		const auto hidden = c.hidden.exchange(0), planned = c.planned.exchange(0), confirmed = c.confirmed.exchange(0), refused = c.refused.exchange(0);
		const auto unplanned = c.unplanned.exchange(0), afterDepth = c.afterDepth.exchange(0);
		const auto skins = c.skins.exchange(0), rigid = c.rigid.exchange(0), missing = c.missing.exchange(0), maxNodes = c.maxNodes.exchange(0);
		if (!rooted)
			return {};
		return fmt::format("late poses: {} frames with a root, {} with hidden nodes (at most {}), {} found only after the depth commit; {} plans, "
						   "{} used, {} refused (a node shown again), {} frames hidden without a plan (a hide's first frame); {} skins and {} rigid "
						   "geometries shrunk, {} not among the producer's movers (so far the engine's own draws, which its hide covers)",
			rooted, hidden, maxNodes, afterDepth, planned, confirmed, refused, unplanned, skins, rigid, missing);
	}
}
