#include "Features/Upscaling/FramePresenter.h"

#include "Features/Upscaling/DxvkPresenter.h"
#include "Features/Upscaling/NativePresenter.h"
#include "RenderGraph/Engine/Native/Native.h"

namespace FrameGen
{
	Presenter& ActivePresenter()
	{
		if (RenderGraph::Engine::Native::Active())
			return NativePresenter::Get();
		return DxvkPresenter::Get();
	}

	PresentationChange PresenterBase::Prepare(const PresentationConfig& a_wanted, const PresentationConfig& a_asked, const char* a_reason)
	{
		if (OnlyHdrDiffers(a_wanted, a_asked) && (a_wanted.method == Method::kNone || !LayerRemadeForColourSpace()))
			return { 0, true };
		const uint64_t serial = Replace(a_wanted, a_reason);
		if (!serial)
			stl::report_and_fail("Community Shaders: the presenter took no presentation request, so the frame revision cannot wait for its swap chain");
		return { serial, false };
	}

	void PresenterBase::NoteAdopted(uint64_t a_serial)
	{
		uint64_t current = adopted.load(std::memory_order_acquire);
		while (current < a_serial && !adopted.compare_exchange_weak(current, a_serial, std::memory_order_acq_rel)) {}
	}

	void PresenterBase::StoreTornDown(std::function<bool()> a_tornDown)
	{
		std::lock_guard guard(lock);
		tornDown = std::move(a_tornDown);
	}

	std::function<bool()> PresenterBase::TornDown() const
	{
		std::lock_guard guard(lock);
		return tornDown;
	}
}
