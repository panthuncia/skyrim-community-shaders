#include "Features/Upscaling/DxvkPresenter.h"

#include "DxvkExports.h"
#include "Features/HDRDisplay.h"
#include "Features/Upscaling/DXVKInterop.h"
#include "Features/Upscaling/Streamline.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Engine/UiTargets.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/Switches.h"
#include "State.h"

#include <dxgi1_6.h>
#include <winrt/base.h>

namespace FrameGen
{
	namespace
	{
		// DXVK's presenter exports, from the shared table. Plain D3D11 mode may run on a DXVK without them, and DxvkLoader
		// logged which. Upscaling::Load presets the swap chain's knobs before DXVK makes it, so each call reads the table.
		const DxvkExports& Dxvk() { return DxvkExports::Get(); }

		bool HasFrameGenerationExports()
		{
			const auto& dxvk = Dxvk();
			return dxvk.requestSwapchainRecreate && dxvk.setSyncPresent && dxvk.getPresenterSurfaceState && dxvk.setSwapchainTornDownCallback;
		}

		bool IsHDRActive()
		{
			const auto& hdr = globals::features::hdrDisplay;
			return hdr.loaded && hdr.IsHDREnabledForFrame();
		}

		void PushSyncPresent(bool a_sync)
		{
			// Frame generation proxies need the present to finish before the D3D11 hook returns.
			if (const auto set = Dxvk().setSyncPresent)
				set(a_sync ? 1u : 0u);
		}

		void PushPresentQueueDepth(uint32_t a_depth)
		{
			// A bounded overlap for a steady proxy. Fully synchronous wedges the swap chain recreate that installs the FFX wrap,
			// and unrestricted wedges shortly after it.
			if (const auto set = Dxvk().setPresentQueueDepth)
				set(a_depth);
		}

		void PushTearingPreference(uint32_t a_preference)
		{
			// Presenter::pickPresentMode reads this at swap chain creation. 0 is tear-free, 1 is tearing.
			if (const auto set = Dxvk().setTearingPreference)
				set(a_preference);
		}

		/** @brief CS_ORG_PRESENT_COPY_SKIP=1 (development builds): the guest present skips its copy, for timing. */
		bool SkipPresentCopy()
		{
#if defined(CS_RELEASE_PROFILE)
			return false;
#else
			static const bool skip = [] {
				const bool on = Switches::On("CS_ORG_PRESENT_COPY_SKIP");
				if (on)
					logger::warn("[ORG] CS_ORG_PRESENT_COPY_SKIP=1: the guest present skips its copy; the screen shows garbage and timings stay valid");
				return on;
			}();
			return skip;
#endif
		}

		/** @brief The format a copy compares: sRGB and UNORM views of one image copy alike. */
		DXGI_FORMAT CopyFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			default:
				return a_format;
			}
		}

		rhi::Format FormatOf(VkFormat a_format)
		{
			switch (a_format) {
			case VK_FORMAT_R8G8B8A8_UNORM:
				return rhi::Format::R8G8B8A8_UNorm;
			case VK_FORMAT_R8G8B8A8_SRGB:
				return rhi::Format::R8G8B8A8_UNorm_sRGB;
			case VK_FORMAT_B8G8R8A8_UNORM:
				return rhi::Format::B8G8R8A8_UNorm;
			case VK_FORMAT_B8G8R8A8_SRGB:
				return rhi::Format::B8G8R8A8_UNorm_sRGB;
			// Both 10-bit orders take the one 10-bit D3D11 format, as the hudless always did.
			case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
			case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
				return rhi::Format::R10G10B10A2_UNorm;
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return rhi::Format::R16G16B16A16_Float;
			default:
				return rhi::Format::Unknown;
			}
		}
	}

	DxvkPresenter& DxvkPresenter::Get()
	{
		static DxvkPresenter presenter;
		return presenter;
	}

	bool DxvkPresenter::HasFrameGenerationInterop()
	{
		return HasFrameGenerationExports();
	}

	bool DxvkPresenter::Available() const
	{
		if (!HasFrameGenerationExports())
			return false;
		auto* dxvk = DXVKInterop::GetSingleton();
		if (!dxvk->IsAvailable() || dxvk->HasCommandRingFault())
			return false;
		const auto* streamline = Streamline::GetSingleton();
		return streamline->IsFSRFGSupported() || streamline->IsDLSSGSupported();
	}

	SurfaceState DxvkPresenter::Surface() const
	{
		auto* dxvk = DXVKInterop::GetSingleton();
		const auto encoding = dxvk->GetPresenterEncodingForFrame();
		SurfaceState surface;
		// The presenter reports its format asynchronously and nothing until it has committed one.
		if (encoding != DXVKInterop::PresenterEncoding::kUnknown)
			surface.format = FormatOf(dxvk->GetPresenterFormatForFrame());
		surface.hdr10 = encoding == DXVKInterop::PresenterEncoding::kHDR10;
		surface.serial = AdoptedSerial();
		surface.ready = dxvk->IsPresenterStateReadyForFrame(IsHDRActive());
		return surface;
	}

	uint64_t DxvkPresenter::Replace(const PresentationConfig& a_wanted, const char* a_reason)
	{
		if (!Dxvk().requestSwapchainRecreate || !Dxvk().setSwapchainTornDownCallback)
			stl::report_and_fail(
				"Community Shaders: DXVK cannot recreate its swap chain or report the replacement (dxvkRequestSwapchainRecreate, "
				"dxvkSetSwapchainTornDownCallback), so a presentation change cannot wait for its swap chain");
		RegisterTornDown();
		PresentationConfig config = a_wanted;
		// DXVK's colour space transition names the space its next swap chain has. HDR Display changes it at the adoption.
		namespace Vars = RenderGraph::Frame::Vars;
		config.hdr = Vars::Adopted(Vars::PresentationConfig()).hdr;
		RequestConfig(config, a_reason);
		return RequestedSerial();
	}

	void DxvkPresenter::Adopt(const PresentationConfig&, const PresentationChange&) {}

	void DxvkPresenter::RequestConfig(const PresentationConfig& a_config, const char* a_reason)
	{
		// In this order, as measured: sync present before a proxy installs, the present mode before the recreate reads it.
		if (a_config.method != Method::kNone)
			PushSyncPresent(true);
		PushTearingPreference(a_config.tearing ? 1u : 0u);
		DXVKInterop::GetSingleton()->BeginPresenterColorSpaceTransition(a_config.hdr, true);
		(void)NextRequest();
		if (const auto recreate = Dxvk().requestSwapchainRecreate) {
			recreate();
			logger::info("[Streamline] requested DXVK swapchain recreate ({})", a_reason);
		} else {
			logger::warn("[Streamline] this DXVK build cannot recreate its swap chain: {} cannot take effect", a_reason);
		}
	}

	bool DxvkPresenter::TornDownTrampoline()
	{
		auto& presenter = Get();
		// The swap chain made after this has every request made so far.
		const uint64_t serial = presenter.RequestedSerial();
		const auto step = presenter.TornDown();
		const bool done = !step || step();
		presenter.NoteAdopted(serial);
		return done;
	}

	void DxvkPresenter::SetTornDown(std::function<bool()> a_tornDown)
	{
		StoreTornDown(std::move(a_tornDown));
		// Streamline features may only be loaded or unloaded while no swap chain exists.
		if (!Dxvk().setSwapchainTornDownCallback) {
			logger::warn("[Streamline] {}: frame-generation switching is off", Dxvk().Loaded() ? "this DXVK build has no swap chain callback" : "DXVK is not loaded");
			return;
		}
		RegisterTornDown();
	}

	void DxvkPresenter::RegisterTornDown()
	{
		{
			std::lock_guard guard(registerLock);
			if (std::exchange(callbackRegistered, true))
				return;
		}
		Dxvk().setSwapchainTornDownCallback(&TornDownTrampoline);
		logger::info("[Streamline] registered DXVK swapchain-torn-down callback");
	}

	void DxvkPresenter::Settled(Method a_owner)
	{
		if (a_owner == Method::kNone) {
			PushSyncPresent(false);
			return;
		}
		// The proxy is steady now, so relax the fully synchronous present of the switch into a bounded overlap. Depth zero
		// blocks the render thread on every present (measured: the GPU idle a third of the frame with FSR-FG). Neither extreme
		// works: fully synchronous wedges the recreate that installs the FFX wrap, unrestricted wedges shortly after it.
		if (a_owner == Method::kFSR) {
			// Depth also decides how far DXVK's presenter may run ahead of FFX's paced present. Overridable for sweeping.
			static const uint32_t s_depth = [] {
				const auto v = Switches::Number("CS_FSRFG_QDEPTH", 2);
				return v >= 1 && v <= 8 ? static_cast<uint32_t>(v) : 2u;
			}();
			PushPresentQueueDepth(s_depth);
		}
	}

	void DxvkPresenter::BeforePresent(Method)
	{
		// The proxy install sets depth zero again, so the bounded overlap is applied from the steady present, not once at settle.
		// FSR-FG only. DLSS-G paces by blocking in its own present, and a second waiting gate deadlocks it.
		if (Streamline::GetSingleton()->IsFSRFGPresentOwner())
			PushPresentQueueDepth(2u);
	}

	bool DxvkPresenter::Quiesce(Method a_leaving)
	{
		auto* dxvk = DXVKInterop::GetSingleton();
		// DLSS-G must be off and drained before teardown. FSR-FG's work is in the interop ring.
		return a_leaving == Method::kDLSSG ? dxvk->WaitDeviceIdle() : dxvk->DrainCommandRing();
	}

	void DxvkPresenter::PresetSyncPresent(bool a_sync) { PushSyncPresent(a_sync); }

	void DxvkPresenter::PresetTearing(bool a_tearing) { PushTearingPreference(a_tearing ? 1u : 0u); }

	void DxvkPresenter::BeginFrame() { DXVKInterop::GetSingleton()->CommitPresenterSurfaceStateForRenderFrame(); }

	bool DxvkPresenter::LayerPaces() const { return globals::features::upscaling.IsFrameGenerationActive(); }

	void DxvkPresenter::HoldPresent() { globals::d3d::context->Flush(); }

	void DxvkPresenter::AfterPresent(bool a_presented)
	{
		if (a_presented)
			DXVKInterop::GetSingleton()->RefreshPresenterSurfaceState();
	}

	void DxvkPresenter::CopyFrame(IDXGISwapChain* a_swapChain)
	{
		if (!a_swapChain)
			return;
		auto* field = RenderGraph::Engine::UiTargets::BackBufferTexture();
		if (!field)
			return;
		// The field is a tripwire handle; DXVK's context takes only DXVK's own objects.
		auto* source = RenderGraph::Engine::Tripwire::Unwrap(field);
		winrt::com_ptr<ID3D11Texture2D> backBuffer;
		if (!source || FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()))) || !backBuffer)
			stl::report_and_fail("Community Shaders: the present copy has no frame or no swap chain back buffer");
		D3D11_TEXTURE2D_DESC from{}, to{};
		source->GetDesc(&from);
		backBuffer->GetDesc(&to);
		if (CopyFormat(from.Format) != CopyFormat(to.Format))
			stl::report_and_fail(fmt::format("Community Shaders: the present copy's frame (DXGI format {}) and the swap chain (DXGI format {}) differ in format",
				static_cast<int>(from.Format), static_cast<int>(to.Format)));
		// Between a swap chain resize and the adoption of the targets at the new size, the smaller extent is copied (design 3.4).
		const D3D11_BOX box{ 0, 0, 0, (std::min)(from.Width, to.Width), (std::min)(from.Height, to.Height), 1 };
		ScopedPerfEvent event("CS present copy");
		if (SkipPresentCopy())
			return;
		globals::d3d::context->CopySubresourceRegion(backBuffer.get(), 0, 0, 0, 0, source, 0, &box);
	}

	void DxvkPresenter::SetColorSpace(bool a_hdr)
	{
		winrt::com_ptr<IDXGISwapChain4> swapChain;
		if (!globals::d3d::swapChain || FAILED(globals::d3d::swapChain->QueryInterface(IID_PPV_ARGS(swapChain.put()))) || !swapChain)
			return;
		auto* dxvk = DXVKInterop::GetSingleton();
		if (dxvk->IsAvailable() || dxvk->Initialize())
			dxvk->BeginPresenterColorSpaceTransition(a_hdr);

		if (a_hdr) {
			HRESULT hr = swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
			if (SUCCEEDED(hr)) {
				logger::info("[HDR] Set swap chain color space to HDR10 (PQ/BT.2020)");
				// No HDR10 static metadata: it causes issues on some monitors. HGiG compresses highlights in the shader instead.
				swapChain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
			} else {
				logger::warn("[HDR] Failed to set HDR10 color space");
				dxvk->CancelPresenterColorSpaceTransition(true);
			}
		} else {
			HRESULT hr = swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
			if (SUCCEEDED(hr)) {
				logger::info("[HDR] Set swap chain color space to SDR (sRGB)");
				swapChain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
			} else {
				logger::warn("[HDR] Failed to set SDR color space");
				dxvk->CancelPresenterColorSpaceTransition(false);
			}
		}
	}
}
