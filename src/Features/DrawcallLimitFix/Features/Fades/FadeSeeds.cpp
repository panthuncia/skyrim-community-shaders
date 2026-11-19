// Fade seeds, the "fade-seeds" frame-value pool. A seed is a root's node as it was at the start of the frame.
// The render thread copies the node's bytes, and the producer turns the copy into a row. FadeStateCS and the builds read the rows.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <cstring>
#include <utility>

namespace DCLF::Features::Fades
{
	namespace
	{
		// Engine references must be released on the render thread, never on a worker.
		template <class T>
		void ReleaseLater(RE::NiPointer<T>& a_reference)
		{
			if (a_reference)
				EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(a_reference)));
		}

		/**
		 * @brief A copy of the node's bytes taken before the frame's culls change it. StaticOf on the copy still reads the live
		 * tree LOD switch, which no cull writes, so the node is held until the producer is done.
		 */
		struct SeedCopy
		{
			alignas(16) std::array<std::byte, FadeState::kMaxNodeBytes> bytes{};
			RE::NiPointer<RE::NiAVObject> node;
			std::shared_ptr<const FrameGlobals> globals;  // for StaticOf's LOD scale
			std::uint32_t row = 0, generation = 0;
		};

		/** @brief Copies taken since the last kick, in request order. */
		struct SeedInput
		{
			std::vector<SeedCopy> values;
			std::uint32_t rows = 0;

			~SeedInput()
			{
				for (auto& copy : values)
					ReleaseLater(copy.node);
			}
		};

		Api::FrameValuePoolId fadeSeedsPool;

		// Render thread. idleSeeds is reused while nothing is new, to avoid an allocation every frame.
		std::vector<SeedCopy> unsentSeeds;
		std::uint32_t seedRows = 0;
		std::shared_ptr<const SeedInput> idleSeeds;

		// Producer only, one at a time in frame order. owedSeeds are copies a refused lease left unread.
		KeptWholeArray<FadeRootStatic> fadeSeeds;
		std::vector<SeedCopy> owedSeeds;

		// For the diagnostic capture.
		std::atomic<std::size_t> unsentSeedCount{ 0 }, owedSeedCount{ 0 };

		// Render thread, before this frame's culls update the nodes. FadeStateCS's first update is this frame's.
		void FadeSeedsFrameStart(const Api::FrameValuePoolFrameStart& a_start)
		{
			auto seeds = Scene::FrameView::Get().TakeComponentSeeds(Scene::FadeRootComponentId());
			for (auto& seed : seeds) {
				if (!seed.node)
					continue;
				if (const auto* reference = seed.node->GetUserData(); reference && reference->IsActor())
					RecheckFadeAmount(seed.node.get());
				auto& copy = unsentSeeds.emplace_back();
				std::memcpy(copy.bytes.data(), seed.node.get(), FadeState::NodeBytes(FadeState::PlanOf(*seed.node)));
				copy.node = std::move(seed.node);
				copy.globals = a_start.globals;
				copy.row = seed.row;
				copy.generation = seed.generation;
				seedRows = std::max(seedRows, seed.row + 1);
			}
			unsentSeedCount.store(unsentSeeds.size(), std::memory_order_relaxed);
		}

		Api::FrameValueInput FadeSeedsKick(const Api::FrameValuePoolKick&)
		{
			DCLF_ZONE("CS.DCLF.FV.Kick.FadeSeeds");
			if (!unsentSeeds.empty()) {
				auto input = std::make_shared<SeedInput>();
				input->values = std::exchange(unsentSeeds, {});
				unsentSeedCount.store(0, std::memory_order_relaxed);
				input->rows = seedRows;
				return { std::move(input), seedRows };
			}
			if (!idleSeeds || idleSeeds->rows != seedRows) {
				auto input = std::make_shared<SeedInput>();
				input->rows = seedRows;
				idleSeeds = std::move(input);
			}
			return { idleSeeds, seedRows };
		}

		void FadeSeedsSample(Api::FrameValuePoolProducer& a_producer)
		{
			DCLF_ZONE("CS.DCLF.FrameValues.FadeSeeds");
			a_producer.BeginBuild(fadeSeeds);
			auto& out = fadeSeeds.Mutable();
			const auto* input = a_producer.Input<SeedInput>();
			const std::uint32_t rows = input ? input->rows : 0u;
			if (out.size() < rows) {
				const std::size_t first = out.size();
				out.resize(rows, FadeRootStatic{});
				fadeSeeds.MarkRange(first, rows - first);
			}
			// Owed copies go first so a later value for the same row wins. StaticOf needs an engine read lease.
			// Once a lease is refused, the rest wait in order for the next producer.
			auto carried = std::move(owedSeeds);
			owedSeeds.clear();
			bool refusedHere = false;
			std::uint64_t seeded = 0;
			const auto seed = [&](const SeedCopy& a_copy) {
				if (!refusedHere) {
					EngineReadWindow::Lease lease;
					if (lease) {
						FrameGlobals::Scope scope(a_copy.globals);
						FadeRootStatic row = FadeState::StaticOf(*reinterpret_cast<const RE::NiAVObject*>(a_copy.bytes.data()));
						row.generation = a_copy.generation;
						if (a_copy.row < out.size()) {
							out[a_copy.row] = row;
							fadeSeeds.Mark(a_copy.row);
							++seeded;
						}
						return;
					}
					refusedHere = true;
					FrameValues::Get().NoteRefused();
				}
				owedSeeds.push_back(a_copy);
			};
			for (const auto& copy : carried)
				seed(copy);
			if (input)
				for (const auto& copy : input->values)
					seed(copy);
			a_producer.NoteSeeded(seeded);
			owedSeedCount.store(owedSeeds.size(), std::memory_order_relaxed);
			for (auto& copy : carried)
				ReleaseLater(copy.node);
		}

		void FadeSeedsSend(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(fadeSeeds);
		}
	}

	void RegisterSeeds(Api::InstallContext& a_context)
	{
		fadeSeedsPool = a_context.RegisterFrameValuePool({ .name = "fade-seeds", .stride = sizeof(FadeRootStatic), .frameStart = &FadeSeedsFrameStart,
			.kick = &FadeSeedsKick, .sample = &FadeSeedsSample, .send = &FadeSeedsSend });
	}

	std::uint32_t SeedsIndex()
	{
		return FrameValues::Get().PoolIndex(fadeSeedsPool);
	}

	const std::vector<FadeRootStatic>& ProducerSeeds()
	{
		return fadeSeeds.Get();
	}

	const std::vector<FadeRootStatic>* SeedsIfDone(bool a_wait)
	{
		return FrameValues::Get().ProducerDone(a_wait) ? &fadeSeeds.Get() : nullptr;
	}

	SeedCounts PeekSeedCounts()
	{
		return { unsentSeedCount.load(std::memory_order_relaxed), owedSeedCount.load(std::memory_order_relaxed) };
	}
}
