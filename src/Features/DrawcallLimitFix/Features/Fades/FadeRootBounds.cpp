// Fade root bounds, the "fade-root-bounds" frame-value pool. Each frame the producer reads every listed root node's world bound.
// BSFadeNode::OnVisible tests the node's own bound, so FadeStateCS and the CPU stage test and measure a root with these rows.
// A member's placement row is no substitute: a member the walk does not sample, such as a head part under a helmet, has none.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "RenderGraph/Async/Lanes.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace DCLF::Features::Fades
{
	namespace
	{
		/** @brief The frame's tables. The retirement hold keeps every node they name alive, so the producer may read them under a lease. */
		struct BoundsInput
		{
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const void> retirement;
		};

		Api::FrameValuePoolId boundsPool;
		// Render thread.
		std::uint64_t boundRows = 0;
		std::shared_ptr<const BoundsInput> idleBoundsInput = std::make_shared<BoundsInput>();
		// Producer only, one at a time in frame order.
		KeptWholeArray<FadeRootBound> rootBounds;
		std::vector<std::vector<std::uint32_t>> boundChunkMarks;

		Api::FrameValueInput BoundsKick(const Api::FrameValuePoolKick&)
		{
			const auto& view = Scene::FrameView::Get();
			auto tables = view.AcceptedTables();
			if (!tables)
				return { idleBoundsInput, boundRows };
			auto input = std::make_shared<BoundsInput>();
			boundRows = std::max<std::uint64_t>(boundRows, Extensions::RootListOf(*tables).Size());
			input->tables = std::move(tables);
			input->retirement = view.InstalledRetirement();
			return { std::move(input), boundRows };
		}

		void BoundsSample(Api::FrameValuePoolProducer& a_producer)
		{
			const auto* input = a_producer.Input<BoundsInput>();
			if (!input || !input->tables)
				return;
			DCLF_ZONE("CS.DCLF.FrameValues.FadeRootBounds");
			a_producer.BeginBuild(rootBounds);
			const auto list = Extensions::RootListOf(*input->tables);
			const std::size_t roots = list.Size();
			auto& out = rootBounds.Mutable();
			if (out.size() < roots) {
				const std::size_t first = out.size();
				out.resize(roots, FadeRootBound{});
				rootBounds.MarkRange(first, roots - first);
			}
			// One lease per chunk. A refused chunk keeps its rows from the last frame, as refused placement rows do.
			constexpr std::size_t kGrain = 512;
			const std::size_t chunks = (roots + kGrain - 1) / kGrain;
			if (boundChunkMarks.size() < chunks)
				boundChunkMarks.resize(chunks);
			std::atomic<bool> refused{ false };
			::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.FrameValues.FadeRootBounds", roots, kGrain, [&](std::size_t a_begin, std::size_t a_end) {
				auto& marks = boundChunkMarks[a_begin / kGrain];
				marks.clear();
				EngineReadWindow::Lease lease;
				if (!lease) {
					refused.store(true, std::memory_order_relaxed);
					return;
				}
				for (std::size_t r = a_begin; r < a_end; ++r) {
					FadeRootBound row{};
					if (const auto* node = static_cast<const RE::NiAVObject*>(list.NodeOf(static_cast<std::uint32_t>(r)))) {
						const auto& bound = node->worldBound;
						row.centre[0] = bound.center.x;
						row.centre[1] = bound.center.y;
						row.centre[2] = bound.center.z;
						row.radius = bound.radius;
						row.generation = list.rows[r].generation;
					}
					if (std::memcmp(&out[r], &row, sizeof(row)) != 0) {
						out[r] = row;
						marks.push_back(static_cast<std::uint32_t>(r));
					}
				}
			});
			for (std::size_t c = 0; c < chunks; ++c)
				for (const std::uint32_t r : boundChunkMarks[c])
					rootBounds.Mark(r);
			if (refused.load(std::memory_order_relaxed))
				FrameValues::Get().NoteRefused();
		}

		void BoundsSend(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(rootBounds);
		}
	}

	void RegisterRootBounds(Api::InstallContext& a_context)
	{
		boundsPool = a_context.RegisterFrameValuePool({ .name = "fade-root-bounds", .stride = sizeof(FadeRootBound), .kick = &BoundsKick, .sample = &BoundsSample,
			.send = &BoundsSend });
	}

	std::uint32_t RootBoundsIndex()
	{
		return FrameValues::Get().PoolIndex(boundsPool);
	}

	const std::vector<FadeRootBound>& ProducerRootBounds()
	{
		return rootBounds.Get();
	}
}
