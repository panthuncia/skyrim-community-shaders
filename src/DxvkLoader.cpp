#include "DxvkLoader.h"

#include "DxvkExports.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"
#include "Vram/Debug.h"

#include <filesystem>

namespace DxvkLoader
{
	namespace
	{
		bool g_attempted = false;
		bool g_loaded = false;
		decltype(&D3D11CreateDeviceAndSwapChain) g_d3d11Create = nullptr;
		decltype(&CreateDXGIFactory) g_createFactory = nullptr;

		std::string ModuleNameAt(const void* a_address)
		{
			HMODULE owner = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &owner))
				return "<no module>";
			wchar_t buf[MAX_PATH]{};
			const DWORD n = ::GetModuleFileNameW(owner, buf, MAX_PATH);
			return n ? std::filesystem::path(std::wstring(buf, n)).filename().string() : "<unknown module>";
		}

		/**
		 * @brief An export's address read from the module's own export table, not through GetProcAddress.
		 *
		 * Capture tools that hook D3D11 (RenderDoc, loaded in-process by the RenderDoc feature) hook
		 * GetProcAddress too, and match the libraries they hook by name substring: `dxvk_d3d11.dll` counts
		 * as `d3d11.dll` and `dxvk_dxgi.dll` as `dxgi.dll`. GetProcAddress on the DXVK DLLs then returns the
		 * tool's D3D11 entry point, which forwards to the *system* runtime: the game silently runs on the
		 * native driver, with DXVK loaded but unused, and fails later on whatever only DXVK supports. The
		 * export table is data in the mapped image and cannot be redirected. A mismatch with GetProcAddress
		 * is logged with the module that answered, so an interposer is named in the log instead of guessed.
		 */
		FARPROC ResolveExport(HMODULE a_module, const char* a_name)
		{
			const auto base = reinterpret_cast<const std::byte*>(a_module);
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

			FARPROC direct = nullptr;
			if (dir.VirtualAddress && dir.Size) {
				const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
				const auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
				const auto ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
				const auto functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
				for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
					if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), a_name) != 0)
						continue;
					const DWORD rva = functions[ordinals[i]];
					// A forwarder's RVA points at a "dll.function" string inside the export directory.
					if (rva < dir.VirtualAddress || rva >= dir.VirtualAddress + dir.Size)
						direct = reinterpret_cast<FARPROC>(const_cast<std::byte*>(base + rva));
					break;
				}
			}

			const FARPROC viaLoader = ::GetProcAddress(a_module, a_name);
			if (!direct)
				return viaLoader;
			if (viaLoader != direct)
				logger::warn("[DXVK] GetProcAddress({}) is interposed: it returns {} in {} instead of DXVK's {}; calling DXVK directly",
					a_name, fmt::ptr(viaLoader), viaLoader ? ModuleNameAt(viaLoader) : "<null>", fmt::ptr(direct));
			return direct;
		}
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
		// Debug override for testing against the native D3D11 runtime.
		static const bool s_native = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_NATIVE_D3D11", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return s_native;
	}

	bool Load()
	{
		if (g_attempted) {
			return g_loaded;
		}
		g_attempted = true;

		if (NativeModeRequested()) {
			logger::info("[DXVK] CS_NATIVE_D3D11=1 -- skipping DXVK, using the system D3D11 runtime");
			return false;
		}

		// Native mode: CS makes its own Vulkan device and the engine's D3D11 objects are tripwire handles over it.
		if (RenderGraph::Engine::Native::Requested()) {
			logger::info("[DXVK] CS_ORG_NATIVE=1 -- DXVK is not loaded; the render graph owns the device and Present");
			return false;
		}

		if (!::SetEnvironmentVariableW(L"DXVK_HDR", L"1")) {
			logger::warn("[DXVK] Failed to enable HDR color-space support (error {})", ::GetLastError());
		}

		// Put DXVK's own output where the rest of a bug report already is. DXVK defaults to the
		// directory holding the exe, so SkyrimSE_d3d11.log, SkyrimSE_dxgi.log and -- on a device
		// loss with VK_EXT_device_fault -- SkyrimSE_device_fault.bin all land in the game folder,
		// somewhere separate from CommunityShaders.log that nobody thinks to send. Pointing DXVK at
		// the SKSE log directory means "zip your SKSE logs folder" collects every artifact we
		// produce, the GPU crash dumps included.
		if (const auto logDir = logger::log_directory()) {
			const auto path = logDir->wstring();
			if (!::SetEnvironmentVariableW(L"DXVK_LOG_PATH", path.c_str()))
				logger::warn("[DXVK] Failed to redirect DXVK logs to the SKSE log folder (error {})", ::GetLastError());
		}

		// DXVK_DEBUG=hang is the crash-diagnostics mode. DXVK reads it at instance creation: device faults and GPU checkpoints.
		// CS's crash markers and detailed checkpoints follow it. It serialises concurrent command recording, so it is opt-in.
		if (Switches::Equals("DXVK_DEBUG", "hang"))
			logger::info("[DXVK] DXVK_DEBUG=hang: device faults, GPU checkpoints and CS's crash markers on");

		const auto dir = GetRuntimeDir();
		if (dir.empty()) {
			logger::error("[DXVK] Could not resolve plugin directory for DXVK DLLs");
			return false;
		}

		const auto dxgiPath = (dir / L"dxvk_dxgi.dll").wstring();
		const auto d3d11Path = (dir / L"dxvk_d3d11.dll").wstring();

		// dxvk_d3d11.dll imports dxvk_dxgi.dll by base name.
		const HMODULE dxgiMod = ::LoadLibraryExW(dxgiPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!dxgiMod) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_dxgi.dll from '{}' (error {})", dir.string(), err);
			return false;
		}
		const HMODULE d3d11Mod = ::LoadLibraryExW(d3d11Path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!d3d11Mod) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_d3d11.dll from '{}' (error {})", dir.string(), err);
			return false;
		}

		g_d3d11Create = reinterpret_cast<decltype(g_d3d11Create)>(ResolveExport(d3d11Mod, "D3D11CreateDeviceAndSwapChain"));
		g_createFactory = reinterpret_cast<decltype(g_createFactory)>(ResolveExport(dxgiMod, "CreateDXGIFactory"));

		if (!g_d3d11Create || !g_createFactory) {
			logger::error("[DXVK] Resolved DXVK DLLs but missing exports (d3d11create={}, createfactory={})",
				g_d3d11Create != nullptr, g_createFactory != nullptr);
			return false;
		}

		// Graph mode calls every fork export without checks, so a DXVK build without one cannot run it.
		// Plain D3D11 mode runs on it, without what the missing exports do.
		const auto& exports = DxvkExports::Get();
		if (const auto missing = exports.Missing(); !missing.empty()) {
			if (RenderGraph::Engine::GraphMode()) {
				const auto message = fmt::format(
					"Community Shaders: the DXVK build in '{}' lacks exports the render graph needs ({}). Reinstall Community Shaders, "
					"or set CS_ORG=0 to play without the render graph.",
					dir.string(), missing);
				logger::critical("[DXVK] {}", message);
				stl::report_and_fail(message);
			}
			logger::warn("[DXVK] This DXVK build lacks {}; plain D3D11 mode runs without what they do", missing);
		}

		// The render graph adopts DXVK's device; the features it needs, and the descriptor
		// binding model negotiation callback, must be registered before the game creates that device.
		RenderGraphRuntime::RequestDeviceFeatures(d3d11Mod);

		// VRAM attribution's creation stacks are recorded as resources are created: before the device exists.
		Vram::Debug::ConfigureDxvk(d3d11Mod);

		// Frame generation enables synchronous present when it takes ownership.
		if (exports.setSyncPresent)
			exports.setSyncPresent(0u);

		logger::info("[DXVK] Loaded DXVK from '{}' (dxvk_d3d11.dll + dxvk_dxgi.dll)", dir.string());
		g_loaded = true;
		return true;
	}

	bool IsLoaded() { return g_loaded; }
	decltype(&D3D11CreateDeviceAndSwapChain) GetD3D11CreateDeviceAndSwapChain() { return g_d3d11Create; }
	decltype(&CreateDXGIFactory) GetCreateDXGIFactory() { return g_createFactory; }
}
