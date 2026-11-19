#pragma once

#include "Features/Upscaling/FrameGenController.h"

#include <rhi.h>

#include <cstdint>
#include <functional>

// One presenter interface with a backend per present path: DXVK's presenter in guest, the native swap chain in native
// mode. FrameGen::Controller drives either one the same way (docs/development/plans/native-frame-generation-plan.md).
namespace FrameGen
{
	// What the swap chain presents with. A change replaces the swap chain at a present boundary.
	struct PresentationConfig
	{
		Method method = Method::kNone;
		bool hdr = false;
		bool vsync = false;
		bool tearing = true;
	};

	// What the present writes into for this frame.
	struct SurfaceState
	{
		rhi::Format format = rhi::Format::Unknown;
		bool hdr10 = false;
		uint64_t serial = 0;
		bool ready = false;
	};

	class Presenter
	{
	public:
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
	};

	// The presenter of the active mode: native mode's swap chain, else DXVK's presenter.
	Presenter& ActivePresenter();
}
