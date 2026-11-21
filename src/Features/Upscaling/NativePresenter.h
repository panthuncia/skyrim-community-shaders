#pragma once

#include "Features/Upscaling/FramePresenter.h"

namespace FrameGen
{
	/**
	 * @brief The native swap chain as a presenter. A configuration change replaces the swap chain on the submission thread, and
	 * the render thread sees the result with the frame revision that waited for it (FrameGen::PresentationRevision). Implemented in
	 * RenderGraph/Engine/Native/NativeDevice.cpp, beside the swap chain it drives.
	 */
	class NativePresenter final : public PresenterBase
	{
	public:
		static NativePresenter& Get();

		/** @brief Native mode is up and Streamline loaded a frame generation feature on its device. */
		bool Available() const override;
		/** @brief The swap chain of the adopted frame revision. Not ready while a replacement is in flight. Render owner. */
		SurfaceState Surface() const override;
		/**
		 * @brief Takes the replacement as the frame's presentation. For a change made at the adoption, asks for it here, after the presents
		 * queued so far, and takes the request as the frame's presentation at once.
		 */
		void Adopt(const PresentationConfig& a_config, const PresentationChange& a_change) override;
		/**
		 * @brief Asks the submission thread to replace the swap chain with one for a_config, after the presents queued so far. The latest
		 * request wins. The torn-down step stored last runs between the destroy and the create. Any thread.
		 */
		void RequestConfig(const PresentationConfig& a_config, const char* a_reason) override;
		/** @brief Stores the torn-down step, and turns DLSS-G's mode off before every swap chain change from now on. */
		void SetTornDown(std::function<bool()> a_tornDown) override;

		void BeginFrame() override {}
		/** @brief The adopted swap chain's presentation layer paces the presents. */
		bool LayerPaces() const override;
		/** @brief Nothing: the present copies kFRAMEBUFFER through the ledger. */
		void CopyFrame(IDXGISwapChain*) override {}
		/** @brief Sends the frame's work on and waits for the GPU as a present would, so per-frame resources outlive their reads. */
		void HoldPresent() override;
		void AfterPresent(bool) override {}
		/** @brief Logs whether the adopted swap chain has the frame's colour space. The revision that changed it asked for the swap chain. */
		void SetColorSpace(bool a_hdr) override;
		std::optional<bool> LayerVsync() const override;

		/** @brief The submission thread made the swap chain for request a_serial. Called by the native device only. */
		void Replaced(uint64_t a_serial) { NoteAdopted(a_serial); }

	protected:
		uint64_t Replace(const PresentationConfig& a_config, const char* a_reason) override;
		/** @brief A presentation layer bakes its colour space in. */
		bool LayerRemadeForColourSpace() const override { return true; }

	private:
		NativePresenter() = default;
	};
}
