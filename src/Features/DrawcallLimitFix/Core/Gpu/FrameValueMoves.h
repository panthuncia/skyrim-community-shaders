#pragma once

// FrameValues' own move events. The same hooks that feed the scene pass push into these queues too.
// FrameValues' producer is their one consumer. Neither side drains the other's queues.

#include "RenderGraph/Async/EventQueue.h"

namespace DCLF::FrameValueMoves
{
	/** @brief Keys of what an update pass moved. Never dereferenced. */
	// Per frame there are thousands, so no wake. The next producer drains them.
	inline RenderGraph::Frame::EventQueue<const void*> keys;
	/**
	 * @brief Nodes Havok wrote from rigid bodies (FUN_140ea55a0) or that gained a controller. Held references go back through
	 * EngineReleases once the producer has named their owner.
	 */
	inline RenderGraph::Frame::EventQueue<RE::NiPointer<RE::NiAVObject>> nodes;

	/** @brief Any thread. */
	inline void PushKey(const void* a_key)
	{
		if (a_key)
			keys.Push(a_key);
	}

	/** @brief Any thread. */
	inline void PushNode(RE::NiAVObject* a_node)
	{
		if (a_node)
			nodes.Push(RE::NiPointer<RE::NiAVObject>(a_node));
	}
}
