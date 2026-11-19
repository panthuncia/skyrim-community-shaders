#pragma once

/**
 * @brief An in-process sampling profile of the render thread, without ETW or elevation, behind CS_ORG_RT_SAMPLE=<seconds>.
 * It covers CPU time no Tracy zone covers.
 *
 * About once a millisecond a helper thread suspends the render thread and unwinds its stack. Samples count only when the thread
 * used CPU since the last one. The test step "mark start" begins it. Sampling slows the render thread, so use it to find costs, not time them.
 */
namespace RenderGraph::Engine::Native::RtSampler
{
	/** @brief The first call with the switch on takes this thread as the sampled one, then counts frames. Render thread. */
	void Frame();

	/** @brief Starts sampling once a session. Any thread. */
	void Begin();
}
