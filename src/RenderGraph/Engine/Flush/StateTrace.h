#pragma once

#include <d3d11.h>

/**
 * @brief The state readers' trace (P5.5.10, flush.md "Readers of the D3D11 state"), part of the residue census
 * (CS_ORG_RESIDUE_CENSUS=1). Flush.cpp's side of it: the check after a flush that bound its state, and the window's line.
 */
namespace RenderGraph::Engine::Flush::StateTrace
{
	/** @brief Whether the trace is on: the census is requested and the guest context is (the latch's switches' precondition). */
	bool On();

	/**
	 * @brief Render thread, right after a flush that bound its state on D3D11: any flush with the latch off, and with it on one CS
	 * asked for or one of an allowed engine draw. Compares DXVK's PS t0-t15, render target 0 and viewport with the shadow state.
	 * Slots the flush's caller marked dirty again (CS rebinding a slot after the flush) are not compared. a_caller names the flush.
	 */
	void CheckApplied(void* a_caller);

	/** @brief Main thread, at each census window's end: the window's counts, one line. */
	void Report(double a_frames);
}
