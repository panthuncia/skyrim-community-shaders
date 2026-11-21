#pragma once

/**
* @brief The one entry from CS's startup into the engine-on-graph subsystems. Hooks.cpp and State.cpp call these, so a new
* subsystem is added here and nowhere else. Each subsystem does its work only when GraphMode() is true.
* Device-time hooks stay where the device is made, and per-call seams stay at their call sites.
*/
namespace RenderGraph::Engine
{
	/**
	* @brief Graph mode: the frame runs on the render graph, in guest or native mode, and not on VR, whose engine was not
	* checked. The one test of the mode. Read once. Any thread.
	*/
	bool GraphMode();

	/** @brief Main thread, from Hooks::Install. The install order matters for nested detours. */
	void InstallGameHooks();

	/** @brief State::Setup, after RenderGraphRuntime::Initialize and before the features' SetupResources. */
	void Setup();

	/**
	* @brief State::Setup, after the features' SetupResources, so HDR's UI texture exists. Builds every pending graph so the
	* primary-path clients are in place before the first frame. Render thread, between frames.
	*/
	void PostSetup();
}
