#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameStaging.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	include <Render/LatchBlock.h>

#	include <algorithm>
#	include <bit>

namespace RenderGraph
{
	std::shared_ptr<FrameStaging> FrameStaging::Create(Desc a_desc)
	{
		return std::make_shared<FrameStaging>(std::move(a_desc));
	}

	FrameStaging::FrameStaging(Desc a_desc) :
		desc(std::move(a_desc))
	{
		desc.runsPerFrame = (std::max)(desc.runsPerFrame, 1u);
		keep = RenderGraphRuntime::kGameFramesInFlight * desc.runsPerFrame + 1;
		if (desc.maxBlocks)
			keep = (std::min)(keep, desc.maxBlocks);
	}

	std::uint32_t FrameStaging::Spare() const
	{
		std::scoped_lock lock(mutex);
		return static_cast<std::uint32_t>(spare.size());
	}

	std::shared_ptr<org::LatchBlock> FrameStaging::Acquire(std::uint64_t a_bytes)
	{
		std::shared_ptr<org::LatchBlock> block;
		{
			std::scoped_lock lock(mutex);
			auto best = spare.end();
			for (auto it = spare.begin(); it != spare.end(); ++it)
				if ((*it)->Stride() >= a_bytes && (best == spare.end() || (*it)->Stride() < (*best)->Stride()))
					best = it;
			if (best != spare.end()) {
				block = std::move(*best);
				spare.erase(best);
			} else if (desc.maxBlocks && made.load(std::memory_order_relaxed) >= desc.maxBlocks) {
				// Every block exists. When none of the spares is big enough, the smallest goes: a larger one takes its place.
				if (spare.empty())
					return nullptr;
				spare.erase(std::ranges::min_element(spare, {}, [](const auto& a_block) { return a_block->Stride(); }));
				made.fetch_sub(1, std::memory_order_relaxed);
			}
		}
		if (!block) {
			const std::uint64_t size = std::bit_ceil((std::max)(a_bytes, static_cast<std::uint64_t>(desc.minimumBytes)));
			if (size > 0xFFFFFFFFull)
				return nullptr;
			try {
				block = org::LatchBlock::Create(desc.name, static_cast<std::uint32_t>(size), 1);
			} catch (const std::exception& e) {
				logger::error("[ORG] {}: a staging buffer of {} bytes could not be made: {}", desc.name, size, e.what());
				return nullptr;
			}
			const auto count = made.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count > keep && !churnLogged.test_and_set())
				logger::info("[ORG] {}: {} staging buffers made, more than the {} it keeps ({} runs a frame); the extra ones are freed after use",
					desc.name, count, keep, desc.runsPerFrame);
		}
		// The pool is held weakly: a block that comes back after the pool is gone is released.
		std::weak_ptr<FrameStaging> pool = weak_from_this();
		auto* raw = block.get();
		return std::shared_ptr<org::LatchBlock>(raw, [pool, block = std::move(block)](org::LatchBlock*) mutable {
			if (auto owner = pool.lock())
				owner->GiveBack(std::move(block));
		});
	}

	void FrameStaging::GiveBack(std::shared_ptr<org::LatchBlock> a_block)
	{
		std::scoped_lock lock(mutex);
		if (spare.size() < keep) {
			spare.push_back(std::move(a_block));
			return;
		}
		// Past what the pool keeps: released (through ORG's deletion path), and no longer counted.
		made.fetch_sub(1, std::memory_order_relaxed);
	}
}

#endif
