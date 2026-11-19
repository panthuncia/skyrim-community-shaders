#pragma once

#include "Features/Upscaling/PresentationConfig.h"

#include <cstdint>
#include <string_view>

/**
 * @brief The swap chain's presentation as the frame variable PresentationConfig (native frame generation plan, step 9).
 *
 * Writers change the variable here. A frame revision that changes it asks the active presenter for a replacement and is adopted
 * once the presenter reports the swap chain made for it, so a frame presents into a swap chain made for the revision it runs.
 * Native's submission thread entry (Native::RequestPresentation) is the revision's input. DXVK's recreate is too, except for a
 * change of the colour space alone, which HDR Display applies to DXVK's swap chain when it adopts the revision.
 *
 * Without the render graph there are no revisions, and a request goes to the presenter at once.
 */
namespace FrameGen::PresentationRevision
{
	/**
	 * @brief Render thread. A replacement with a_config's method, vsync and tearing. The colour space stays the one HDR Display
	 * wrote. Every call is a new request, so it replaces the swap chain even with unchanged settings.
	 * @return The request's number, for Has.
	 */
	std::uint32_t Request(const PresentationConfig& a_config, std::string_view a_reason);

	/** @brief Render thread. HDR Display's display check: whether the swap chain should take HDR10. An event only on a change. */
	void SetHdr(bool a_hdr, std::string_view a_writer);

	/** @brief Render thread. Whether the frame's swap chain was made for request a_request or a newer one. */
	bool Has(std::uint32_t a_request);

	/** @brief Render thread. Whether a request was made that the frame's swap chain does not have yet. */
	bool Pending();
}
