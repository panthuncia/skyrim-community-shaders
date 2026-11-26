#pragma once

#include <cstdint>

/**
* @brief The engine-render detector: counts each engine render function the frame still calls, per call site and frame kind, and
* the render functions' calls per frame. Each counted site is a CALL inside an engine function, retargeted to a machine-code stub
* on its own trampoline that counts and jumps on to the call's old target, so it chains onto any hook already written there and
* changes no register the callee reads. CS_SNAPSHOT_DETECT=1 or CS_ENGINE_ZONES=1 installs the sites (AE 1.6.1170; SE 1.5.97 for
* the sites with an SE match). CS_SNAPSHOT_DETECT=1 logs the tables at each load and at exit. CS_ENGINE_ZONES=1 in Tracy builds
* also gives each call a zone while Tracy is connected. The snapshot roadmap's phases 3 to 5 drive native's count to zero.
*/
namespace EngineZones
{
	/** @brief Installs the counted call sites when asked for, and registers the detectors' exit report. Call after every other hook is installed. */
	void Install();

	/** @brief Renderer::Begin, with the new frame's RenderGraph::Frame::Kind: closes the last frame's count. Begin's thread. */
	void BeginFrame(std::uint8_t a_kind) noexcept;

	/**
	* @brief The function the CALL at a_callSite leads to, past this detector's counting thunk: the hooks chained there, then the engine's
	* function. CS code that calls through an engine call site itself (to run what other features hook there) uses it, so the table
	* counts only the engine's own calls.
	*/
	std::uintptr_t CallTarget(std::uintptr_t a_callSite);

	/** @brief Logs the call-site table (under CS_SNAPSHOT_DETECT=1) and writes its summary to the test status. a_when names the moment. */
	void Report(const char* a_when);
}
