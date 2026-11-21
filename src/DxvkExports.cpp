#include "DxvkExports.h"

#include "RenderGraph/Engine/Native/Native.h"

#include <atomic>

namespace
{
	template <class T>
	T ResolveDxvkExport(HMODULE a_module, const char* a_name)
	{
		return reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(a_module, a_name)));
	}
}

const DxvkExports& DxvkExports::Get()
{
	static const DxvkExports empty{};
	static std::atomic<const DxvkExports*> resolved{ nullptr };
	if (const auto* table = resolved.load(std::memory_order_acquire))
		return *table;
	// Native mode never loads DXVK. Some callers run per draw, so there is no module lookup.
	if (RenderGraph::Engine::Native::Requested())
		return empty;
	const HMODULE module = ::GetModuleHandleW(L"dxvk_d3d11.dll");
	if (!module)
		return empty;
	static const DxvkExports table = [module] {
		DxvkExports exports;
		exports.loaded = true;
#define CS_DXVK_EXPORT_RESOLVE(a_type, a_field, a_name) exports.a_field = ResolveDxvkExport<a_type>(module, #a_name);
		CS_DXVK_EXPORTS(CS_DXVK_EXPORT_RESOLVE)
#undef CS_DXVK_EXPORT_RESOLVE
		return exports;
	}();
	resolved.store(&table, std::memory_order_release);
	return table;
}

std::string DxvkExports::Missing() const
{
	std::string missing;
	const auto note = [&missing](const void* a_function, const char* a_name) {
		if (a_function)
			return;
		if (!missing.empty())
			missing += ", ";
		missing += a_name;
	};
#define CS_DXVK_EXPORT_MISSING(a_type, a_field, a_name) note(reinterpret_cast<const void*>(a_field), #a_name);
	CS_DXVK_EXPORTS(CS_DXVK_EXPORT_MISSING)
#undef CS_DXVK_EXPORT_MISSING
	return missing;
}
