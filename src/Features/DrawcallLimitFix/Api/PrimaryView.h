#pragma once

// Handlers for the main view at the main pass's engine points. FrameDriver keeps the frame's bookkeeping and calls these.

namespace DCLF::Api
{
	/**
	 * @brief The main view. One contribution only. Render thread, once per frame each.
	 *
	 * - beforeOpaque: captures the main pass for the colour epoch. Returns whether it was captured.
	 * - afterOpaque: submits the colour epoch.
	 */
	struct PrimaryViewContribution
	{
		bool (*beforeOpaque)() = nullptr;
		void (*afterOpaque)() = nullptr;
	};
}
