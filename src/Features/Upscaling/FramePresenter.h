#pragma once

#include "Features/Upscaling/FrameGenController.h"

#include <rhi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

struct IDXGISwapChain;

// One presenter interface with a backend per present path: DXVK's presenter in guest, the native swap chain in native
// mode. FrameGen::Controller and the present hook drive either one the same way (docs/development/plans/native-frame-generation-plan.md).
namespace FrameGen
{
	// What the present writes into for this frame.
	struct SurfaceState
	{
		rhi::Format format = rhi::Format::Unknown;
		bool hdr10 = false;
		uint64_t serial = 0;
		bool ready = false;
	};

	/** @brief How a presenter makes a frame revision's presentation change (FrameGen::PresentationRevision). */
	struct PresentationChange
	{
		uint64_t serial = 0;      // the replacement the revision waits for (AdoptedSerial reaches it). 0 only with atAdoption
		bool atAdoption = false;  // nothing before the adoption: the presenter makes the change inside the adoption point
	};

	class Presenter
	{
	public:
		/**
		 * @brief Render thread, when a frame revision that changes the presentation from a_asked to a_wanted starts. Asks for the
		 * replacement, or answers atAdoption when the change is made inside the adoption point. A presenter that cannot report its
		 * replacement is fatal here, so a revision never adopts before its swap chain exists.
		 */
		virtual PresentationChange Prepare(const PresentationConfig& a_wanted, const PresentationConfig& a_asked, const char* a_reason) = 0;
		/**
		 * @brief Render owner, inside the adoption point of the revision that Prepare answered with a_change. The frames from now on
		 * present into the swap chain made for a_config. With atAdoption the presenter makes the change here.
		 */
		virtual void Adopt(const PresentationConfig& a_config, const PresentationChange& a_change) = 0;

		virtual ~Presenter() = default;
		virtual bool Available() const = 0;                                               // frame generation can run on this presenter
		virtual SurfaceState Surface() const = 0;                                         // what the present writes into, for this frame
		virtual void RequestConfig(const PresentationConfig&, const char* a_reason) = 0;  // replace at the next present boundary
		virtual uint64_t AdoptedSerial() const = 0;                                       // last config the swap chain has
		virtual uint64_t RequestedSerial() const = 0;                                     // last config asked for
		virtual void SetTornDown(std::function<bool()>) = 0;                              // runs between destroy and create

		// The controller adopted a method as the present owner after a replacement. A backend may relax what it held for the switch.
		virtual void Settled(Method) {}
		// Before each present while a frame generation method presents.
		virtual void BeforePresent(Method) {}
		// Whether the GPU is past the leaving method's work that the backend recorded outside the graph. Render thread.
		virtual bool Quiesce(Method) { return true; }

		/** @name The present hook's per-frame duties, in the order a frame reaches them. Render thread. */
		/** @{ */
		/** @brief Once a frame, at its start: takes what the presenter reported as this frame's surface. */
		virtual void BeginFrame() = 0;
		/** @brief Whether a presentation layer paces the presents, so the render thread's own pacing point stays out. */
		virtual bool LayerPaces() const = 0;
		/** @brief After the UI stage's commit and before the swap chain's Present: puts the frame into what a_swapChain presents. */
		virtual void CopyFrame(IDXGISwapChain* a_swapChain) = 0;
		/** @brief Ends a frame whose present is held: its work is sent on and paced as a present would. */
		virtual void HoldPresent() = 0;
		/** @brief After the present, with whether it succeeded. */
		virtual void AfterPresent(bool a_presented) = 0;
		/** @brief HDR Display's colour space change for the frame: a_hdr is HDR10, else SDR. */
		virtual void SetColorSpace(bool a_hdr) = 0;
		/** @} */

		/**
		 * @brief The vsync the adopted swap chain's presentation layer was made for, which its presents keep. Nothing when no layer
		 * keeps one, or while a replacement is in flight. Render owner.
		 */
		virtual std::optional<bool> LayerVsync() const { return std::nullopt; }
	};

	/**
	 * @brief What both presenters share: the request and adoption serials, the colour space rule and the torn-down step's storage.
	 * A backend asks for a replacement (Replace) and reports when one is made (NoteAdopted).
	 */
	class PresenterBase : public Presenter
	{
	public:
		/**
		 * @brief A change of the colour space alone is made at the adoption, unless the backend must remake a presentation layer for
		 * it. Anything else is a replacement. Fatal when the backend reports no serial for it.
		 */
		PresentationChange Prepare(const PresentationConfig& a_wanted, const PresentationConfig& a_asked, const char* a_reason) final;
		/** @brief The request the swap chain was last made for. Any thread. */
		uint64_t AdoptedSerial() const final { return adopted.load(std::memory_order_acquire); }
		/** @brief The latest request. Any thread. */
		uint64_t RequestedSerial() const final { return requested.load(std::memory_order_acquire); }
		/** @brief The torn-down step stored last, or none. A replacement runs it between the destroy and the create. Any thread. */
		std::function<bool()> TornDown() const;

	protected:
		/** @brief Asks for a replacement with a_config for Prepare. Returns the request's serial. */
		virtual uint64_t Replace(const PresentationConfig& a_config, const char* a_reason) = 0;
		/** @brief Whether a colour space change needs a new swap chain while a presentation layer presents. */
		virtual bool LayerRemadeForColourSpace() const = 0;

		/** @brief Takes the next request serial. Any thread. */
		uint64_t NextRequest() { return requested.fetch_add(1, std::memory_order_acq_rel) + 1; }
		/** @brief The swap chain made now has every request up to a_serial. Never moves back. Any thread. */
		void NoteAdopted(uint64_t a_serial);
		/** @brief Stores the torn-down step a replacement runs. Any thread. */
		void StoreTornDown(std::function<bool()> a_tornDown);

	private:
		mutable std::mutex lock;
		std::function<bool()> tornDown;
		std::atomic<uint64_t> requested{ 0 };
		std::atomic<uint64_t> adopted{ 0 };
	};

	// The presenter of the active mode: native mode's swap chain, else DXVK's presenter.
	Presenter& ActivePresenter();
}
