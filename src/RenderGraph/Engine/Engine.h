#pragma once

/**
* @brief The one entry from CS's startup into the engine-on-graph subsystems. Hooks.cpp and State.cpp call these, so a new
* subsystem is added here and nowhere else. Each subsystem still decides by its own switch whether it does anything.
* Device-time hooks stay where the device is made, and per-call seams stay at their call sites.
*/
namespace RenderGraph::Engine
{
	/** @brief Main thread, from Hooks::Install. The install order matters for nested detours. */
	void InstallGameHooks();

	/** @brief State::Setup, before RenderGraphRuntime::Initialize. */
	void Reserve();

	/** @brief State::Setup, after RenderGraphRuntime::Initialize and before the features' SetupResources. */
	void Setup();

	/**
	* @brief State::Setup, after the features' SetupResources, so HDR's UI texture exists. Builds every pending graph so the
	* primary-path clients are in place before the first frame. Render thread, between frames.
	*/
	void PostSetup();
}
