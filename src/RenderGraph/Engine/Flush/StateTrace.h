#pragma once

#include <d3d11.h>

/** @brief Flush.cpp's side of the state readers' trace, part of the residue census. */
namespace RenderGraph::Engine::Flush::StateTrace
{
	/** @brief Whether the trace is on, which needs the census and the guest context. */
	bool On();

	/**
	 * @brief Compares DXVK's PS t0-t15, render target 0 and viewport with the shadow state after a flush that bound its state.
	 * Slots the caller marked dirty again are skipped. Render thread.
	 */
	void CheckApplied(void* a_caller);

	void Report(double a_frames);
}
