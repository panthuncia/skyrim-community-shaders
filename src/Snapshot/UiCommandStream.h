#pragma once

#include <atomic>
#include <memory>

namespace RenderGraph::Engine::Scaleform
{
	class UiStream;
}

namespace Snapshot
{
	/**
	 * @brief A snapshot's UI command stream (phase 4, native only): GFx's renderer calls of the game update, recorded by CS's
	 * Scaleform renderer (RenderGraph/Engine/Scaleform/UiStream.h).
	 *
	 * The capture makes the holder empty. The UI producer's DrawInterface sets it once and it never changes afterwards (Scaleform.h).
	 * Today DrawInterface runs inside the update's first render, after the snapshot was published (Snapshot::Building is still the
	 * update's). In phase 7 the game thread runs it with a Scaleform::ProducerScope before the publish, and the render side replays
	 * the stream with Scaleform::ReplaySnapshotUi. A repeated render of the snapshot replays it again.
	 */
	struct UiCommandStream
	{
		/** @brief Sets the stream, once. UI producer. */
		void Set(std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream> a_stream) const { stream.store(std::move(a_stream), std::memory_order_release); }

		/** @brief Takes the stream out for the retire queue, once the render side will not render the snapshot again. Render side. */
		std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream> Take() const { return stream.exchange(nullptr, std::memory_order_acq_rel); }

		/** @brief The stream, or null before the update's DrawInterface finished it (or when GFx drew nothing). */
		std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream> Get() const { return stream.load(std::memory_order_acquire); }

	private:
		mutable std::atomic<std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream>> stream;
	};
}
