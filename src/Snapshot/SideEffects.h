#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <cstdint>
#include <string>

/**
* @brief The engine render half's side effects that game code reads, produced by CS on the game thread in native mode
* (docs/development/plans/render-side-effects.md, decisions 9-14 in render-thread-decisions.md).
*
* Native only. Guest and D3D11 keep the engine's writes. In native the engine's own writes are turned off where that is
* safe, so each value has one producer, and the cadence test mode's skipped updates get the same values as rendered ones.
*
* - S1: render stages 5 to 9 (SetJobTaskEvent) are signalled after the render half, not inside it.
* - S2: gFrameCounter (State+0x4C) moves once per game update, after the render half, not in Renderer::End.
* - S4: the main thread's render-unsafe bit is cleared after the render half, not after the world draw.
* - S5: the fade enable globals come back on 20 world updates after a teleport, counted here, not in Main::Draw.
* - S6: the actor pre-cull (kNotVisible, Actor+0x27C) is decided by a CPU frustum test here; the engine's culler is skipped.
* - S17: queued decal occlusion tests are answered "visible" here; the pending array is cleared before the render half.
*/
namespace Snapshot::SideEffects
{
	/** @brief Native only: turns off the engine's writes listed above. Called by Snapshot::InstallHooks. */
	void InstallHooks();

	/** @brief Game thread, right after the snapshot is published and before the render half. Native only. */
	void AfterCapture(const FrameSnapshot& a_snapshot);

	/** @brief Game thread, after the render half ran a_renders times (0 for a skipped update). Native only. */
	void AfterRenderHalf(std::uint32_t a_renders);

	/** @brief Marks the render half on the game thread, so the engine's own stage signals inside it are skipped. */
	class RenderHalfScope
	{
	public:
		RenderHalfScope();
		~RenderHalfScope();
		RenderHalfScope(const RenderHalfScope&) = delete;
		RenderHalfScope& operator=(const RenderHalfScope&) = delete;
	};

	/** @brief The counters since the last call, as one log line, or empty outside native. */
	std::string TakeStatsLine();
}
