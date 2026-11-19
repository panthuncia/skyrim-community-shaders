// Tree wind's seeds: a frame-value pool of the listed trees' nodes as the frame's start had them, two rows per tree slot.
// Read on the render thread at frame start, sent by FrameValues' producer, and read by TreeWindCS through the depth commit's frame row.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <cstring>
#include <utility>

namespace DCLF::Features::TreeWind
{
	namespace
	{
		float SeedNodeFloat(const void* a_node, std::size_t a_offset)
		{
			return *reinterpret_cast<const float*>(static_cast<const std::byte*>(a_node) + a_offset);
		}

		// FUN_14147d640(node): the node's object (+0x118, via vfunc 0x30). FUN_140437e50 takes a tree into the near list when
		// object+0x128 -> +0x18 -> int at +0x10 == 1.
		REL::Relocation<std::uintptr_t (*)(const void*)> treeObjectOf{ REL::Offset(Address::kTreeFirstMultiBound) };

		bool NearListSelectable(const void* a_node)
		{
			const std::uintptr_t object = treeObjectOf(a_node);
			if (!object)
				return false;
			const auto first = *reinterpret_cast<const std::uintptr_t*>(object + 0x128);
			if (!first)
				return false;
			const auto second = *reinterpret_cast<const std::uintptr_t*>(first + 0x18);
			return second && *reinterpret_cast<const std::int32_t*>(second + 0x10) == 1;
		}

		/** @brief A BSTreeNode's static row into a_out. */
		void TreeStaticOfNode(const void* a_node, TreeStatic& a_out)
		{
			a_out = {};
			const void* node = a_node;
			const auto* bytes = static_cast<const std::byte*>(node);
			std::memcpy(a_out.position, bytes + 0xA0, sizeof(a_out.position));
			a_out.leafFrequency = SeedNodeFloat(node, 0x160);
			a_out.timer = SeedNodeFloat(node, 0x164);
			a_out.previousTimer = SeedNodeFloat(node, 0x168);
			a_out.amplitude = SeedNodeFloat(node, 0x15c);
			// The manager advances a node only in its near loop, when its model (+0xF8, then +0x40) has bones (+0xB8). Other nodes keep their
			// clock, and their +0xB0 is never read and can be garbage. The near list's count cap is not replicated.
			const auto* holder = *reinterpret_cast<const std::byte* const*>(bytes + 0xF8);
			const auto* model = holder ? *reinterpret_cast<const std::byte* const*>(holder + 0x40) : nullptr;
			const bool bones = model && *reinterpret_cast<const void* const*>(model + 0xB8);
			a_out.animated = bones && NearListSelectable(node) ? 1u : 0u;
			a_out.modelAmplitude = a_out.animated ? *reinterpret_cast<const float*>(model + 0xB0) : 0.0f;
		}

		Api::FrameValuePoolId pool;  // install

		struct SeedInput
		{
			std::vector<std::pair<std::uint32_t, TreeStatic>> values;
			std::uint32_t rows = 0;
		};

		// Render thread: seeds no kick handed over yet, and the most rows named. The rows never shrink.
		std::vector<std::pair<std::uint32_t, TreeStatic>> unsent;
		std::uint32_t seedRows = 0;
		std::shared_ptr<const SeedInput> idle;

		// The producers run one at a time in frame order.
		KeptWholeArray<TreeStatic> seeds;

		// Render thread, every frame's start: the seeds the installed publications asked for (the component's listings: FrameView).
		void FrameStart(const Api::FrameValuePoolFrameStart& a_start)
		{
			FrameGlobals::Scope scope(a_start.globals);
			// Read here, not by the producer, because the near-list test reads a cull result that this frame's culls write.
			for (const auto& seed : Scene::FrameView::Get().TakeComponentSeeds(ComponentId())) {
				if (!seed.node)
					continue;
				TreeStatic row;
				TreeStaticOfNode(seed.node.get(), row);
				row.generation = seed.generation;
				row.seedOdd = seed.row & 1u;
				unsent.emplace_back(seed.row, row);
				seedRows = std::max(seedRows, seed.row + 1);
			}
		}

		// Render thread, at Kick: every seed read since the last kick, for the frame's producer.
		Api::FrameValueInput Kick(const Api::FrameValuePoolKick&)
		{
			DCLF_ZONE("CS.DCLF.FV.Kick.TreeSeeds");
			if (!unsent.empty())
				return { std::make_shared<const SeedInput>(SeedInput{ std::exchange(unsent, {}), seedRows }), seedRows };
			if (!idle || idle->rows != seedRows)
				idle = std::make_shared<const SeedInput>(SeedInput{ {}, seedRows });
			return { idle, seedRows };
		}

		void Sample(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.BeginBuild(seeds);
			auto& trees = seeds.Mutable();
			const auto* input = a_producer.Input<SeedInput>();
			const std::uint32_t rows = input ? input->rows : 0u;
			if (trees.size() < rows) {
				const std::size_t first = trees.size();
				trees.resize(rows, TreeStatic{});
				seeds.MarkRange(first, rows - first);
			}
			std::uint64_t seeded = 0;
			if (input)
				for (const auto& [at, row] : input->values) {
					if (at >= trees.size())
						continue;
					trees[at] = row;
					seeds.Mark(at);
					++seeded;
				}
			a_producer.NoteSeeded(seeded);
		}

		void Send(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(seeds);
		}
	}

	void RegisterSeeds(Api::InstallContext& a_context)
	{
		pool = a_context.RegisterFrameValuePool({ .name = "tree-seeds", .stride = sizeof(TreeStatic), .frameStart = &FrameStart, .kick = &Kick, .sample = &Sample,
			.send = &Send });
	}

	std::uint32_t SeedsIndex()
	{
		return Extensions::FrameValuePoolIndex(pool);
	}

	const std::vector<TreeStatic>* SeedsIfDone(bool a_wait)
	{
		return FrameValues::Get().ProducerDone(a_wait) ? &seeds.Get() : nullptr;
	}
}
