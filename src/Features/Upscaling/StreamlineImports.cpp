// The Streamline functions BasicRHI's D3D12 backend calls by name (slSetD3DDevice, slUpgradeInterface, slGetNativeInterface, and
// its own slInit and slShutdown, which CS never asks for). CS links no import library for sl.interposer.dll: the DLL is in CS's own
// folder, which the loader does not search, so a load-time import would keep CommunityShaders.dll from loading. These forwarders
// take each export from the interposer CS loaded (Streamline::Initialize) when they are first called.

#include <sl.h>

#include <atomic>

namespace
{
	template <class F>
	F* Export(const char* a_name)
	{
		const HMODULE interposer = ::GetModuleHandleW(L"sl.interposer.dll");
		return interposer ? reinterpret_cast<F*>(::GetProcAddress(interposer, a_name)) : nullptr;
	}

	template <class F>
	F* Cached(std::atomic<F*>& a_slot, const char* a_name)
	{
		F* function = a_slot.load(std::memory_order_acquire);
		if (!function) {
			function = Export<F>(a_name);
			a_slot.store(function, std::memory_order_release);
		}
		return function;
	}

	std::atomic<PFun_slInit*> g_init{ nullptr };
	std::atomic<PFun_slShutdown*> g_shutdown{ nullptr };
	std::atomic<PFun_slSetD3DDevice*> g_setD3DDevice{ nullptr };
	std::atomic<PFun_slUpgradeInterface*> g_upgradeInterface{ nullptr };
	std::atomic<PFun_slGetNativeInterface*> g_getNativeInterface{ nullptr };
}

extern "C" sl::Result slInit(const sl::Preferences& a_pref, uint64_t a_sdkVersion)
{
	auto* function = Cached(g_init, "slInit");
	return function ? function(a_pref, a_sdkVersion) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slShutdown()
{
	auto* function = Cached(g_shutdown, "slShutdown");
	return function ? function() : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slSetD3DDevice(void* a_device)
{
	auto* function = Cached(g_setD3DDevice, "slSetD3DDevice");
	return function ? function(a_device) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slUpgradeInterface(void** a_interface)
{
	auto* function = Cached(g_upgradeInterface, "slUpgradeInterface");
	return function ? function(a_interface) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slGetNativeInterface(void* a_proxy, void** a_native)
{
	auto* function = Cached(g_getNativeInterface, "slGetNativeInterface");
	return function ? function(a_proxy, a_native) : sl::Result::eErrorNotInitialized;
}
