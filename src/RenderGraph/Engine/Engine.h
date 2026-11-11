#pragma once

/**
 * @brief The one entry from CS's startup into the engine-on-graph subsystems (RenderGraph/Engine/*). Hooks.cpp and State.cpp call these
 * instead of each subsystem's own function, so a new subsystem is added here and nowhere else. Each subsystem still decides by its
 * own switch whether it does anything.
 *
 * Device-time hooks (Tripwire, Pipeline and Buffers device hooks, engine handles) stay where the device is made (Hooks.cpp), and
 * per-call seams (Targets::BeginSet, the Constants Map/Unmap seams, the Tonemap anchor) stay at their call sites.
 */
namespace RenderGraph::Engine
{
	/**
	 * @brief Main thread, Hooks::Install: every subsystem's game hooks, in this order: Textures, Buffers, Constants, Bink,
	 * LoadingScreen, Pipeline, Targets, Skeleton, ImageSpace, Scaleform.
	 */
	void InstallGameHooks();

	/** @brief State::Setup, before RenderGraphRuntime::Initialize: the subsystems' anchor reservations (Skeleton, ImageSpace). */
	void Reserve();

	/**
	 * @brief State::Setup, after RenderGraphRuntime::Initialize and before the features' SetupResources: the engine's targets move to
	 * ORG (Targets::TakeOver), then the subsystems that need them set up (Skeleton, ImageSpace).
	 */
	void Setup();
}
