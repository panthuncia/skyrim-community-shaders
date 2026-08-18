#include "DxvkLoader.h"

#include "Globals.h"
#include "State.h"

#include <filesystem>
#include <utility>

namespace DxvkLoader
{
	namespace
	{
		bool g_attempted = false;
		bool g_loaded = false;
		Api g_api;
		decltype(&D3D11CreateDeviceAndSwapChain) g_d3d11Create = nullptr;
		decltype(&CreateDXGIFactory) g_createFactory = nullptr;

		template <class T>
		T Resolve(HMODULE a_module, const char* a_name)
		{
			return reinterpret_cast<T>(::GetProcAddress(a_module, a_name));
		}

		class UniqueModule
		{
		public:
			explicit UniqueModule(HMODULE a_module = nullptr) : module(a_module) {}
			~UniqueModule() { reset(); }
			UniqueModule(const UniqueModule&) = delete;
			UniqueModule& operator=(const UniqueModule&) = delete;
			HMODULE get() const { return module; }
			HMODULE release() { return std::exchange(module, nullptr); }
			void reset()
			{
				if (module)
					::FreeLibrary(std::exchange(module, nullptr));
			}

		private:
			HMODULE module;
		};
	}

	bool Api::HasFrameGenerationControl() const
	{
		return requestSwapchainRecreate && setSyncPresent && getPresenterSurfaceState &&
		       setSwapchainTornDownCallback && setFrameGenOwnershipQuery;
	}

	bool Api::HasPresentWaitInterop() const
	{
		return enqueueInteropCommandBuffer && getPresentWaitSemaphoreState && clearPresentWaitSemaphore &&
		       cancelPresentWaitSemaphore && releaseQueuedPresentWaitSemaphoresAfterIdle;
	}

	// Resolve relative to the plugin for mod-manager VFS compatibility.
	std::filesystem::path GetRuntimeDir()
	{
		HMODULE self = nullptr;
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetRuntimeDir),
				&self)) {
			return {};
		}
		wchar_t buf[MAX_PATH]{};
		const DWORD n = ::GetModuleFileNameW(self, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			return {};
		}
		return std::filesystem::path(buf).parent_path() / L"CommunityShaders" / L"bin";
	}

	bool NativeModeRequested()
	{
		// The environment variable remains an early diagnostic override. The
		// persisted setting is already loaded before InstallEarlyHooks runs.
		static const bool s_environmentOverride = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_NATIVE_D3D11", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return s_environmentOverride || !globals::state->enableDXVK;
	}

	bool Load()
	{
		if (g_attempted) {
			return g_loaded;
		}
		g_attempted = true;

		if (NativeModeRequested()) {
			logger::info("[DXVK] Disabled at boot -- using the system D3D11/DXGI runtime");
			return false;
		}

		if (!::SetEnvironmentVariableW(L"DXVK_HDR", L"1")) {
			logger::warn("[DXVK] Failed to enable HDR color-space support (error {})", ::GetLastError());
		}

		const auto dir = GetRuntimeDir();
		if (dir.empty()) {
			logger::error("[DXVK] Could not resolve plugin directory for DXVK DLLs");
			return false;
		}

		const auto dxgiPath = (dir / L"dxvk_dxgi.dll").wstring();
		const auto d3d11Path = (dir / L"dxvk_d3d11.dll").wstring();

		// dxvk_d3d11.dll imports dxvk_dxgi.dll by base name.
		UniqueModule dxgiMod(::LoadLibraryExW(dxgiPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
		if (!dxgiMod.get()) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_dxgi.dll from '{}' (error {})", dir.string(), err);
			return false;
		}
		UniqueModule d3d11Mod(::LoadLibraryExW(d3d11Path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
		if (!d3d11Mod.get()) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_d3d11.dll from '{}' (error {})", dir.string(), err);
			return false;
		}

		auto d3d11Create = Resolve<decltype(g_d3d11Create)>(d3d11Mod.get(), "D3D11CreateDeviceAndSwapChain");
		auto createFactory = Resolve<decltype(g_createFactory)>(dxgiMod.get(), "CreateDXGIFactory");
		auto getApiVersion = Resolve<PFN_csDxvkGetApiVersion>(d3d11Mod.get(), "dxvkGetCsApiVersion");

		if (!d3d11Create || !createFactory || !getApiVersion || getApiVersion() != CS_DXVK_API_VERSION) {
			logger::error("[DXVK] DLL validation failed (d3d11={}, dxgi={}, apiVersion={})",
				d3d11Create != nullptr, createFactory != nullptr, getApiVersion ? getApiVersion() : 0u);
			return false;
		}

		Api api{};
		api.d3d11Module = d3d11Mod.get();
		api.dxgiModule = dxgiMod.get();
#define CS_RESOLVE(member, name) api.member = Resolve<decltype(api.member)>(d3d11Mod.get(), name)
		CS_RESOLVE(setTearingPreference, "dxvkSetTearingPreference");
		CS_RESOLVE(getPresenterSurfaceState, "dxvkGetPresenterSurfaceState");
		CS_RESOLVE(setFrameGenOwnershipQuery, "dxvkSetFrameGenOwnershipQuery");
		CS_RESOLVE(setPresentBeginCallback, "dxvkSetPresentBeginCallback");
		CS_RESOLVE(setPresentCompletedCallback, "dxvkSetPresentCompletedCallback");
		CS_RESOLVE(requestSwapchainRecreate, "dxvkRequestSwapchainRecreate");
		CS_RESOLVE(setSwapchainTornDownCallback, "dxvkSetSwapchainTornDownCallback");
		CS_RESOLVE(setTargetFrameRate, "dxvkSetTargetFrameRate");
		CS_RESOLVE(setSyncPresent, "dxvkSetSyncPresent");
		CS_RESOLVE(setPresentQueueDepth, "dxvkSetPresentQueueDepth");
		CS_RESOLVE(enqueueInteropCommandBuffer, "dxvkEnqueueInteropCommandBuffer");
		CS_RESOLVE(getPresentWaitSemaphoreState, "dxvkGetPresentWaitSemaphoreState");
		CS_RESOLVE(clearPresentWaitSemaphore, "dxvkClearPresentWaitSemaphore");
		CS_RESOLVE(cancelPresentWaitSemaphore, "dxvkCancelPresentWaitSemaphore");
		CS_RESOLVE(releaseQueuedPresentWaitSemaphoresAfterIdle, "dxvkReleaseQueuedPresentWaitSemaphoresAfterIdle");
#undef CS_RESOLVE

		g_d3d11Create = d3d11Create;
		g_createFactory = createFactory;
		g_api = api;
		d3d11Mod.release();
		dxgiMod.release();

		// Frame generation enables synchronous present when it takes ownership.
		if (g_api.setSyncPresent)
			g_api.setSyncPresent(0u);
		else
			logger::warn("[DXVK] dxvkSetSyncPresent export missing -- present stays at the fork default (sync)");

		logger::info("[DXVK] Loaded DXVK from '{}' (dxvk_d3d11.dll + dxvk_dxgi.dll)", dir.string());
		g_loaded = true;
		return true;
	}

	bool IsLoaded() { return g_loaded; }
	const Api& GetApi() { return g_api; }
	decltype(&D3D11CreateDeviceAndSwapChain) GetD3D11CreateDeviceAndSwapChain() { return g_d3d11Create; }
	decltype(&CreateDXGIFactory) GetCreateDXGIFactory() { return g_createFactory; }
}
