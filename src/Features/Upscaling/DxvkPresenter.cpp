#include "Features/Upscaling/DxvkPresenter.h"

#include "Features/HDRDisplay.h"
#include "Features/Upscaling/DXVKInterop.h"
#include "Features/Upscaling/NativePresenter.h"
#include "Features/Upscaling/Streamline.h"
#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Switches.h"

#include <dxgi1_6.h>

namespace FrameGen
{
	namespace
	{
		// DXVK's exports for its presenter, resolved on first use. Upscaling::Load presets the swap chain's knobs before
		// DXVK makes it, which is before Streamline's initialization, so every call resolves first.
		struct DxvkExports
		{
			using RequestSwapchainRecreateFn = void (*)();
			using SetSyncPresentFn = void (*)(uint32_t);
			using SetPresentQueueDepthFn = void (*)(uint32_t);
			using SetTearingPreferenceFn = void (*)(uint32_t);
			using SetSwapchainTornDownCallbackFn = void (*)(bool (*)());

			HMODULE module = nullptr;
			RequestSwapchainRecreateFn requestSwapchainRecreate = nullptr;
			SetSyncPresentFn setSyncPresent = nullptr;
			SetPresentQueueDepthFn setPresentQueueDepth = nullptr;
			SetTearingPreferenceFn setTearingPreference = nullptr;
			bool presenterSurfaceStateAvailable = false;
			SetSwapchainTornDownCallbackFn setSwapchainTornDownCallback = nullptr;

			void EnsureResolved()
			{
				if (module)
					return;
				module = GetModuleHandleW(L"dxvk_d3d11.dll");
				if (!module)
					return;
				requestSwapchainRecreate = reinterpret_cast<RequestSwapchainRecreateFn>(GetProcAddress(module, "dxvkRequestSwapchainRecreate"));
				setSyncPresent = reinterpret_cast<SetSyncPresentFn>(GetProcAddress(module, "dxvkSetSyncPresent"));
				setPresentQueueDepth = reinterpret_cast<SetPresentQueueDepthFn>(GetProcAddress(module, "dxvkSetPresentQueueDepth"));
				setTearingPreference = reinterpret_cast<SetTearingPreferenceFn>(GetProcAddress(module, "dxvkSetTearingPreference"));
				presenterSurfaceStateAvailable = GetProcAddress(module, "dxvkGetPresenterSurfaceState") != nullptr;
				setSwapchainTornDownCallback = reinterpret_cast<SetSwapchainTornDownCallbackFn>(GetProcAddress(module, "dxvkSetSwapchainTornDownCallback"));
			}

			[[nodiscard]] bool HasFrameGenerationInterop() const
			{
				return module && requestSwapchainRecreate && setSyncPresent && presenterSurfaceStateAvailable && setSwapchainTornDownCallback;
			}
		} g_dxvk;

		bool IsHDRActive()
		{
			const auto& hdr = globals::features::hdrDisplay;
			return hdr.loaded && hdr.IsHDREnabledForFrame();
		}

		void PushSyncPresent(bool a_sync)
		{
			g_dxvk.EnsureResolved();
			// Frame generation proxies need the present to finish before the D3D11 hook returns.
			if (g_dxvk.setSyncPresent) {
				g_dxvk.setSyncPresent(a_sync ? 1u : 0u);
			} else {
				static bool s_warned = false;
				if (!std::exchange(s_warned, true))
					logger::warn("[Streamline] dxvkSetSyncPresent not found - synchronous present control inactive");
			}
		}

		void PushPresentQueueDepth(uint32_t a_depth)
		{
			g_dxvk.EnsureResolved();
			// A bounded overlap for a steady proxy. Fully synchronous wedges the swap chain recreate that installs the FFX wrap,
			// and unrestricted wedges shortly after it.
			if (g_dxvk.setPresentQueueDepth) {
				g_dxvk.setPresentQueueDepth(a_depth);
			} else {
				static bool s_warned = false;
				if (!std::exchange(s_warned, true))
					logger::warn("[Streamline] dxvkSetPresentQueueDepth not found - bounded present overlap inactive");
			}
		}

		void PushTearingPreference(uint32_t a_preference)
		{
			g_dxvk.EnsureResolved();
			// Presenter::pickPresentMode reads this at swap chain creation. 0 is tear-free, 1 is tearing.
			if (g_dxvk.setTearingPreference) {
				g_dxvk.setTearingPreference(a_preference);
			} else {
				static bool s_warned = false;
				if (!std::exchange(s_warned, true))
					logger::warn("[Streamline] dxvkSetTearingPreference not found - present-mode control inactive");
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

	Presenter& ActivePresenter()
	{
		if (RenderGraph::Engine::Native::Active())
			return NativePresenter::Get();
		return DxvkPresenter::Get();
	}

	DxvkPresenter& DxvkPresenter::Get()
	{
		static DxvkPresenter presenter;
		return presenter;
	}

	bool DxvkPresenter::HasFrameGenerationInterop()
	{
		g_dxvk.EnsureResolved();
		return g_dxvk.HasFrameGenerationInterop();
	}

	bool DxvkPresenter::Available() const
	{
		if (!g_dxvk.HasFrameGenerationInterop())
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
		surface.serial = adopted.load(std::memory_order_acquire);
		surface.ready = dxvk->IsPresenterStateReadyForFrame(IsHDRActive());
		return surface;
	}

	void DxvkPresenter::RequestConfig(const PresentationConfig& a_config, const char* a_reason)
	{
		g_dxvk.EnsureResolved();
		// In this order, as measured: sync present before a proxy installs, the present mode before the recreate reads it.
		if (a_config.method != Method::kNone)
			PushSyncPresent(true);
		PushTearingPreference(a_config.tearing ? 1u : 0u);
		DXVKInterop::GetSingleton()->BeginPresenterColorSpaceTransition(a_config.hdr, true);
		requested.fetch_add(1, std::memory_order_acq_rel);
		if (g_dxvk.requestSwapchainRecreate) {
			g_dxvk.requestSwapchainRecreate();
			logger::info("[Streamline] requested DXVK swapchain recreate ({})", a_reason);
		} else {
			logger::warn("[Streamline] dxvkRequestSwapchainRecreate not found - {} cannot take effect", a_reason);
		}
	}

	uint64_t DxvkPresenter::AdoptedSerial() const { return adopted.load(std::memory_order_acquire); }

	uint64_t DxvkPresenter::RequestedSerial() const { return requested.load(std::memory_order_acquire); }

	bool DxvkPresenter::TornDownTrampoline()
	{
		auto& presenter = Get();
		// The swap chain made after this has every request made so far.
		const uint64_t serial = presenter.requested.load(std::memory_order_acquire);
		std::function<bool()> step;
		{
			std::lock_guard guard(presenter.lock);
			step = presenter.tornDown;
		}
		const bool done = !step || step();
		uint64_t current = presenter.adopted.load(std::memory_order_acquire);
		while (current < serial && !presenter.adopted.compare_exchange_weak(current, serial, std::memory_order_acq_rel)) {}
		return done;
	}

	void DxvkPresenter::SetTornDown(std::function<bool()> a_tornDown)
	{
		g_dxvk.EnsureResolved();
		bool registerNow = false;
		{
			std::lock_guard guard(lock);
			tornDown = std::move(a_tornDown);
			registerNow = !callbackRegistered;
			callbackRegistered = true;
		}
		if (!registerNow)
			return;
		if (!g_dxvk.module) {
			logger::warn("[Streamline] DXVK module not loaded - cannot register swapchain callbacks");
			return;
		}
		// Streamline features may only be loaded or unloaded while no swap chain exists.
		if (g_dxvk.setSwapchainTornDownCallback) {
			g_dxvk.setSwapchainTornDownCallback(&TornDownTrampoline);
			logger::info("[Streamline] registered DXVK swapchain-torn-down callback");
		} else {
			logger::warn("[Streamline] dxvkSetSwapchainTornDownCallback not found - frame-generation switching disabled");
		}
	}

	bool DxvkPresenter::ReportsReplacements() const
	{
		std::lock_guard guard(lock);
		return callbackRegistered && g_dxvk.module && g_dxvk.setSwapchainTornDownCallback;
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

	void DxvkPresenter::AfterPresent(bool a_presented)
	{
		if (a_presented)
			DXVKInterop::GetSingleton()->RefreshPresenterSurfaceState();
	}

	void DxvkPresenter::SetColorSpace(IDXGISwapChain4* a_swapChain, bool a_hdr)
	{
		auto* dxvk = DXVKInterop::GetSingleton();
		if (dxvk->IsAvailable() || dxvk->Initialize())
			dxvk->BeginPresenterColorSpaceTransition(a_hdr);

		if (a_hdr) {
			HRESULT hr = a_swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
			if (SUCCEEDED(hr)) {
				logger::info("[HDR] Set swap chain color space to HDR10 (PQ/BT.2020)");
				// No HDR10 static metadata: it causes issues on some monitors. HGiG compresses highlights in the shader instead.
				a_swapChain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
			} else {
				logger::warn("[HDR] Failed to set HDR10 color space");
				dxvk->CancelPresenterColorSpaceTransition(true);
			}
		} else {
			HRESULT hr = a_swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
			if (SUCCEEDED(hr)) {
				logger::info("[HDR] Set swap chain color space to SDR (sRGB)");
				a_swapChain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
			} else {
				logger::warn("[HDR] Failed to set SDR color space");
				dxvk->CancelPresenterColorSpaceTransition(false);
			}
		}
	}
}
