#pragma once

// The CPU-driven pipeline's draw sites. While the graph is built for the CPU pipeline, each draw epoch's commits still run on
// the render thread, but its submission is the site's. The site culls its views on DCLF's pool and records direct draws.
//
// Guest mode (DXVK) queues one callback into DXVK's stream that records on its CS thread into DXVK's own command buffer.
// Native mode submits the epochs through the graph, and their late passes record the draws inline into the epoch's own list.
//
// Lifetime: everything a frame's sites make belongs to its frame slot and is kept until the frame's signal on DCLF's CPU-frame
// timeline completes. The slot's next frame waits for that signal before resetting. The ORG latch regions the commits write
// live in an external run that retires after the same signal.

#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <rhi.h>

#include <Render/RunRef.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF::Draws
{
	struct BuildDrawsLatch;
	struct GpuCascade;
	struct GpuShadowVolume;
	struct PassFrame;
	struct Resources;
}

namespace DCLF::Direct
{
	/** @brief The draw sites' late passes, one last in each site's epoch, in the CPU pipeline's graph only. */
	std::span<const Api::PassDef> SitePassDefs();

	/** @brief Any thread. Whether the frame draws through the direct sites. */
	bool CpuFrame();

	/** @brief Render thread. The frame's mode. */
	Mode FrameMode();

	/**
	 * @brief Render thread, frame start, after adoption and before coverage and FrameValues::Kick. In CPU mode takes the frame's slot,
	 * waiting for its last signal if needed. Frames that are not CPU frames release CPU slots as they complete, never waiting.
	 */
	void FrameStart(std::shared_ptr<const ViewBatchSet> a_set);

	/**
	 * @brief Render thread, while frame start waits for GPU work. Submits what was queued into DXVK's stream since its last flush,
	 * because anything waiting on it would otherwise wait for the waiting frame itself.
	 * @param a_always Flush even when nothing counted is queued, for a wait that may hang on any work in DXVK's stream.
	 */
	void SubmitQueued(bool a_always = false);

	/**
	 * @brief Render thread, outside a CPU frame. Releases slots whose GPU work is done, without waiting. Their batch sets lease
	 * backings a new payload version may need.
	 */
	void ReleaseCompletedSlots();

	/** @brief Render thread. The frame's view batches, null before the first or in GPU mode. */
	const ViewBatchSet* FrameBatches();

	/** @brief Render thread, before Present. Queues the frame's signal behind its callbacks, once a frame. */
	void QueueFrameSignal();
	/** @brief Render thread, after Present. Queues the signal if QueueFrameSignal did not, and takes the report counters. */
	void EndFrame();

	/**
	 * @brief Render thread, at a draw epoch's trigger. In CPU mode a_body runs here outside any ORG epoch and the site it opened
	 * queues its callback after it. False when the epoch failed or no callback could be queued.
	 */
	/**
	 * @brief Render thread, end of a native site epoch's commit. Keeps the run the commit opened, or the one handed to the epoch,
	 * for the epoch's late pass.
	 */
	void AdoptSiteRun();
	bool ExecuteEpoch(RenderGraphRuntime::Segment a_segment, const std::function<void()>& a_body, std::shared_ptr<const void> a_owner);

	/** @brief A main commit's inputs to its site. */
	struct MainCommitInputs
	{
		std::shared_ptr<const Draws::PassFrame> shape;  // the revision's shape the commit wrote against
		std::shared_ptr<Draws::Resources> resources;     // the main resources (the targets, the frame constants, the pool)
		const org::services::NamedVersions* adopted = nullptr;  // held by the execution's owner
		const Draws::BuildDrawsLatch* latch = nullptr;  // the segment's BuildDraws latch as written (the views' cull inputs)
		std::span<const Draws::GpuCascade> cascades;     // the colour commit's: the sun's cascades it latched
		std::span<const Draws::GpuShadowVolume> volumes;  // ... and the local shadow volumes (clamped)
		org::RunRef latchRun{};   // the run whose latch regions it wrote (a published copy's, or the external run)
		std::uint32_t poolCopies = 0;  // 0 means its pre-work skips the dispatch
	};

	/** @brief Render thread, inside the depth epoch's commit. Runs the depth site. */
	void DepthCommitted(const MainCommitInputs& a_inputs);

	/**
	 * @brief Render thread, inside the colour epoch's commit. Runs the colour site and the forward site's culls. A GPU-driven frame runs only the latter.
	 */
	void ColourCommitted(const MainCommitInputs& a_inputs);

	/**
	 * @brief Render thread, at the forward epoch's trigger. Runs the forward site from this frame's colour commit. False when there was no colour commit.
	 */
	bool ForwardReached(bool a_refraction);

	/**
	 * @brief Whether CPU-driven frames draw claimed water in the water family's anchor pass, inside its CS stage (native mode, where
	 * stage passes record inline at every execution), rather than in the water epochs' site passes (guest, whose stages are kept
	 * recordings). Fixed for the session, so the graph builds and the frames agree.
	 */
	bool WaterInStageAnchor();

	/**
	 * @brief Render thread, inside a water epoch's execution body, where the sites do not record inline (guest). Makes the colour
	 * commit's water site of deferred range a_range the epoch's pending run, drawing target set a_variant. False without one.
	 */
	bool WaterReached(std::uint32_t a_range, std::uint32_t a_variant);

	/** @brief Render thread. Whether this CPU frame's colour commit opened a water site that is not drawn yet. */
	bool WaterSitesOpen();

	/** @brief A water draw target: a view and its image, as the recording binds them. */
	struct WaterTarget
	{
		rhi::DescriptorSlot view{};
		rhi::ResourceHandle resource{};
	};

	/**
	 * @brief Render thread, at deferred range a_range's engine batch (0 the water mask, 1 the surface). Takes the colour commit's water
	 * site for it, drawing target set a_variant, for the water family's anchor pass. Null without one: no colour commit this frame, no
	 * water member, or a GPU-driven frame. It stays valid until the frame's slot retires.
	 */
	void* TakeWaterRun(std::uint32_t a_range, std::uint32_t a_variant);

	/**
	 * @brief The thread recording the anchor pass. Records a_run's water draws into a_list, in a rendering of a_colours (the run's target
	 * set) and the main depth. False when the run has nothing to draw.
	 */
	bool RecordWaterDraws(void* a_run, rhi::CommandList& a_list, std::span<const WaterTarget> a_colours, const WaterTarget& a_depth);

	/** @brief Whether a view's site draws in CPU mode. An epoch whose site is not live is not covered, and the engine draws it. */
	bool ViewSiteLive(std::uint32_t a_view);

	/*
	 * The GPU-driven graph sends its forward family through these sites, because ForwardSortCS can only order draws within a pipeline.
	 * Root states come from the CPU fade stage's read-only observer. The GPU's events keep reporting visibility.
	 */

	/** @brief Render thread. The frame's batches in either mode. Without them a GPU-driven forward epoch is not covered. */
	const ViewBatchSet* ForwardBatches();
	/** @brief Render thread. Withdraws the run handed off by ForwardReached after the forward epoch failed. */
	void ForwardHandOffCancel();

	/** @brief One report window's totals. */
	struct WindowCounters
	{
		FrameCounters sum;
		std::uint32_t frames = 0;
		std::array<std::uint32_t, static_cast<std::size_t>(Site::kCount)> siteRuns{}, siteEmpty{};  // callbacks, and those that drew nothing
		std::uint32_t slotWaits = 0, rowWaits = 0;
		std::uint32_t forwardSkipped = 0;  // GPU-driven frames whose forward draws had no cull frame
		double slotWaitMs = 0.0;
		// CPU time of the render thread and DXVK's CS thread from FrameStart to FrameStart, and wall time.
		double renderBusyUs = 0.0, csBusyUs = 0.0, wallUs = 0.0;
	};

	/** @brief Render thread. The window's counters since the last take. */
	WindowCounters TakeWindowCounters();
}
