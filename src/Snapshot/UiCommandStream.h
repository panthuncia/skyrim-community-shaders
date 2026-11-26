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
	 * The capture makes the holder empty. MenuManager::DrawInterface runs inside the update's first render (phases 0-6), so the
	 * stream is complete only at its end, after the snapshot was published; it is set there once and never changes afterwards.
	 * A repeated render of the snapshot replays it. In phase 7 DrawInterface runs in the game-side publish and the stream is set
	 * before the snapshot is published.
	 */
	struct UiCommandStream
	{
		/** @brief Sets the stream, once. Render owner. */
		void Set(std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream> a_stream) const { stream.store(std::move(a_stream), std::memory_order_release); }

		/** @brief Takes the stream out for the retire queue, once the render side will not render the snapshot again. Render side. */
		std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream> Take() const { return stream.exchange(nullptr, std::memory_order_acq_rel); }

		/** @brief The stream, or null before the update's DrawInterface finished it (or when GFx drew nothing). */
		std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream> Get() const { return stream.load(std::memory_order_acquire); }

	private:
		mutable std::atomic<std::shared_ptr<const RenderGraph::Engine::Scaleform::UiStream>> stream;
	};
}
