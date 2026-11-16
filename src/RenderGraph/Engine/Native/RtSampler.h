#pragma once

/**
 * @brief A sampling profile of the render thread, in the process, without ETW or elevation (CS_ORG_RT_SAMPLE=<seconds>, off by
 * default). For the CPU time no Tracy zone covers.
 *
 * About once a millisecond a helper thread suspends the render thread, unwinds its stack (up to 48 frames) and resumes it. A sample
 * counts only when the thread used CPU since the last one (its cycle time moved). After the given seconds the samples are named
 * (DbgHelp for CS and the system, the Address Library ID for the game) and logged: the leaf functions, the functions on the stack
 * (inclusive), and for each leaf outside CS and the game the first CS or game frame above it. Each line gives the share of samples
 * and the estimated ms a frame (share times the thread's CPU time a frame).
 *
 * The test step "mark start" begins it (Testing/Commands.cpp). Sampling slows the render thread a little (one suspend a
 * millisecond); use it to find costs, not to time them.
 */
namespace RenderGraph::Engine::Native::RtSampler
{
	/** @brief Render thread, once a frame: the first call with the switch on takes this thread as the one sampled; then counts frames. */
	void Frame();

	/** @brief Starts sampling (once a session), when the switch is on and the render thread is known. Any thread. */
	void Begin();
}
