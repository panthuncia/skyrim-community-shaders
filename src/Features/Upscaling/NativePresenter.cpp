#include "Features/Upscaling/NativePresenter.h"

#include "Features/Upscaling/Streamline.h"
#include "RenderGraph/Engine/Native/Native.h"

namespace FrameGen
{
	NativePresenter& NativePresenter::Get()
	{
		static NativePresenter presenter;
		return presenter;
	}

	bool NativePresenter::Available() const
	{
		if (!RenderGraph::Engine::Native::Active())
			return false;
		const auto* streamline = Streamline::GetSingleton();
		return streamline->IsFSRFGSupported() || streamline->IsDLSSGSupported();
	}

	SurfaceState NativePresenter::Surface() const
	{
		const auto state = RenderGraph::Engine::Native::FramePresentation();
		SurfaceState surface;
		surface.format = static_cast<rhi::Format>(state.format);
		surface.hdr10 = state.hdr10;
		surface.serial = state.serial;
		// Ready once the swap chain has the latest request, so the frame's inputs match what it presents into.
		surface.ready = RenderGraph::Engine::Native::Active() && state.serial == state.requested;
		return surface;
	}

	void NativePresenter::RequestConfig(const PresentationConfig& a_config)
	{
		std::function<bool()> step;
		{
			std::lock_guard guard(lock);
			step = tornDown;
		}
		(void)RenderGraph::Engine::Native::RequestPresentation(a_config, std::move(step));
	}

	uint64_t NativePresenter::AdoptedSerial() const { return RenderGraph::Engine::Native::Presentation().serial; }

	void NativePresenter::SetTornDown(std::function<bool()> a_tornDown)
	{
		std::lock_guard guard(lock);
		tornDown = std::move(a_tornDown);
	}
}
