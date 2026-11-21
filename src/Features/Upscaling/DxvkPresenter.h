#pragma once

#include "Features/Upscaling/FramePresenter.h"

#include <mutex>

namespace FrameGen
{
	/**
	 * @brief DXVK's presenter (guest mode and plain D3D11 mode). A configuration change recreates DXVK's swap chain, and the
	 * torn-down step runs in DXVK's presenter between the destroy and the create. The DXVK knobs frame generation needs (sync
	 * present, the present queue depth, the tearing preference) are set here and nowhere else.
	 */
	class DxvkPresenter final : public PresenterBase
	{
	public:
		static DxvkPresenter& Get();

		/** @brief DXVK exports the frame generation calls, its ring has no fault, and Streamline loaded a frame generation feature. */
		bool Available() const override;
		/** @brief The surface committed for this render frame, ready when it matches the frame's colour space. Render thread. */
		SurfaceState Surface() const override;
		/** @brief Nothing: DXVK presents into its newest swap chain, and HDR Display applies the colour space at the adoption. */
		void Adopt(const PresentationConfig& a_config, const PresentationChange& a_change) override;
		/** @brief Pushes the config's knobs, starts the colour space transition and asks DXVK to recreate its swap chain. */
		void RequestConfig(const PresentationConfig& a_config, const char* a_reason) override;
		/** @brief Registers DXVK's torn-down callback on first use. */
		void SetTornDown(std::function<bool()> a_tornDown) override;
		void Settled(Method a_owner) override;
		void BeforePresent(Method) override;
		bool Quiesce(Method a_leaving) override;

		/** @brief Commits the surface DXVK last reported for this render frame. Render thread, once a frame. */
		void BeginFrame() override;
		/** @brief A frame generation proxy paces DXVK's presents while frame generation is active. */
		bool LayerPaces() const override;
		/**
		 * @brief At every present: copies kFRAMEBUFFER's slot (the frame, interface included) into DXVK's back buffer, in DXVK's stream,
		 * inside the "CS present copy" GPU event. Nothing before the engine target set holds kFRAMEBUFFER (the engine then draws into
		 * DXVK's back buffer). CS_ORG_PRESENT_COPY_SKIP=1 skips the copy. Render thread.
		 */
		void CopyFrame(IDXGISwapChain* a_swapChain) override;
		/** @brief Flushes the frame's work to DXVK, as its present would. */
		void HoldPresent() override;
		/** @brief After a present: reads the surface DXVK reports. */
		void AfterPresent(bool a_presented) override;
		/** @brief Sets the swap chain's colour space through DXGI, inside a presenter colour space transition. */
		void SetColorSpace(bool a_hdr) override;

		/** @brief DXVK exports every call frame generation needs. Resolves them on first use. */
		bool HasFrameGenerationInterop();
		/** @brief Before DXVK makes its first swap chain: synchronous presents for a frame generation proxy. */
		void PresetSyncPresent(bool a_sync);
		/** @brief Before DXVK makes a swap chain: tearing (IMMEDIATE) or tear-free (MAILBOX). */
		void PresetTearing(bool a_tearing);

	protected:
		/**
		 * @brief Asks DXVK for a replacement in the adopted colour space (HDR Display sets a new one through DXGI at the adoption).
		 * Fatal when DXVK cannot recreate its swap chain or report the replacement.
		 */
		uint64_t Replace(const PresentationConfig& a_config, const char* a_reason) override;
		/** @brief DXVK's colour space follows SetColorSpace1, with a layer too. */
		bool LayerRemadeForColourSpace() const override { return false; }

	private:
		DxvkPresenter() = default;

		static bool TornDownTrampoline();
		/** @brief Registers DXVK's torn-down callback, the only report of a replacement, once. DXVK must export it. */
		void RegisterTornDown();

		std::mutex registerLock;
		bool callbackRegistered = false;
	};
}
