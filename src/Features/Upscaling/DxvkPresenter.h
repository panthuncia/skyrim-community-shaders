#pragma once

#include "Features/Upscaling/FramePresenter.h"

#include <atomic>
#include <mutex>

struct IDXGISwapChain4;

namespace FrameGen
{
	/**
	 * @brief DXVK's presenter (guest mode and plain D3D11 mode). A configuration change recreates DXVK's swap chain, and the
	 * torn-down step runs in DXVK's presenter between the destroy and the create. The DXVK knobs frame generation needs (sync
	 * present, the present queue depth, the tearing preference) are set here and nowhere else.
	 */
	class DxvkPresenter final : public Presenter
	{
	public:
		static DxvkPresenter& Get();

		/** @brief DXVK exports the frame generation calls, its ring has no fault, and Streamline loaded a frame generation feature. */
		bool Available() const override;
		/** @brief The surface committed for this render frame, ready when it matches the frame's colour space. Render thread. */
		SurfaceState Surface() const override;
		/** @brief Pushes the config's knobs, starts the colour space transition and asks DXVK to recreate its swap chain. */
		void RequestConfig(const PresentationConfig& a_config, const char* a_reason) override;
		/** @brief The last request a torn-down step ran for. Any thread. */
		uint64_t AdoptedSerial() const override;
		uint64_t RequestedSerial() const override;
		/** @brief Registers DXVK's torn-down callback on first use. */
		void SetTornDown(std::function<bool()> a_tornDown) override;
		/** @brief Whether DXVK runs the torn-down callback, which is the only report of a replacement. */
		bool ReportsReplacements() const;
		void Settled(Method a_owner) override;
		void BeforePresent(Method) override;
		bool Quiesce(Method a_leaving) override;

		/** @brief DXVK exports every call frame generation needs. Resolves them on first use. */
		bool HasFrameGenerationInterop();
		/** @brief Before DXVK makes its first swap chain: synchronous presents for a frame generation proxy. */
		void PresetSyncPresent(bool a_sync);
		/** @brief Before DXVK makes a swap chain: tearing (IMMEDIATE) or tear-free (MAILBOX). */
		void PresetTearing(bool a_tearing);
		/** @brief Commits the surface DXVK last reported for this render frame. Render thread, once a frame. */
		void BeginFrame();
		/** @brief After a present: reads the surface DXVK reports. */
		void AfterPresent(bool a_presented);
		/** @brief Sets the swap chain's colour space through DXGI, inside a presenter colour space transition. */
		void SetColorSpace(IDXGISwapChain4* a_swapChain, bool a_hdr);

	private:
		DxvkPresenter() = default;

		static bool TornDownTrampoline();

		mutable std::mutex lock;
		std::function<bool()> tornDown;
		bool callbackRegistered = false;
		std::atomic<uint64_t> requested{ 0 };
		std::atomic<uint64_t> adopted{ 0 };
	};
}
