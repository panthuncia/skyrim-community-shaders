#pragma once

#include <Render/Runtime/EventQueue.h>

namespace RenderGraph::Frame
{
	/**
	 * @brief Events the engine's hooks push from any thread, for one consumer to drain (ORG's EventQueue).
	 * The queues live as long as the process and are never cleaned up at its end,
	 * because nothing may release engine objects while the process is torn down.
	 */
	template <class T, std::size_t Capacity = 4096>
	using EventQueue = org::runtime::EventQueue<T, Capacity, org::runtime::QueueTeardown::Leak>;
}
