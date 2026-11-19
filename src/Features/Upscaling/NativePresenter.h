#pragma once

#include "Features/Upscaling/FramePresenter.h"

#include <mutex>

namespace FrameGen
{
	/**
	 * @brief The native swap chain as a presenter. A configuration change replaces the swap chain on the submission thread
	 * (RenderGraph::Engine::Native::RequestPresentation), and the render thread sees the result from the next frame's begin.
	 */
	class NativePresenter final : public Presenter
	{
	public:
		static NativePresenter& Get();

		/** @brief Native mode is up and Streamline loaded a frame generation feature on its device. */
		bool Available() const override;
		/** @brief The swap chain as it was at this frame's begin. Render owner. */
		SurfaceState Surface() const override;
		/** @brief Asks for a replacement with a_config. The torn-down step set last runs between the destroy and the create. */
		void RequestConfig(const PresentationConfig& a_config) override;
		/** @brief The request the swap chain was last made for. Any thread. */
		uint64_t AdoptedSerial() const override;
		void SetTornDown(std::function<bool()> a_tornDown) override;

	private:
		NativePresenter() = default;

		mutable std::mutex lock;
		std::function<bool()> tornDown;
	};
}
