#pragma once

#include <ORGModuleServices/Async/LatestSlot.h>

namespace RenderGraph::Frame
{
	/** @brief OMS's latest-wins mailbox (org::async::LatestSlot). */
	template <class T>
	using LatestSlot = org::async::LatestSlot<T>;
}
