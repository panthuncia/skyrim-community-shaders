#include "Vram/DxvkMemory.h"

#include "DxvkExports.h"
#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"

#include <atomic>

namespace Vram
{
	namespace
	{
		struct Exports
		{
			PFN_dxvkPushResourceOwner push = nullptr;
			PFN_dxvkPopResourceOwner pop = nullptr;
			PFN_dxvkSetResourceOwner setOwner = nullptr;
			PFN_dxvkRequestMemorySnapshot requestSnapshot = nullptr;
			PFN_dxvkReadMemorySnapshot readSnapshot = nullptr;
			PFN_dxvkGetMemoryHeapReports heapReports = nullptr;

			bool Complete() const { return push && pop && setOwner && requestSnapshot && readSnapshot && heapReports; }
		};

		// From DXVK's table once dxvk_d3d11.dll is loaded (feature callbacks can run before DxvkLoader loads it). All or nothing:
		// plain D3D11 mode may run on a DXVK with only some of them, which counts as one without. Native mode has none.
		const Exports& Get()
		{
			static const Exports none;
			static std::atomic<const Exports*> resolved{ nullptr };
			if (const auto* exports = resolved.load(std::memory_order_acquire))
				return *exports;
			const auto& dxvk = DxvkExports::Get();
			if (!dxvk.Loaded())
				return none;
			static const Exports exports = [&dxvk] {
				Exports e{ dxvk.pushResourceOwner, dxvk.popResourceOwner, dxvk.setResourceOwner, dxvk.requestMemorySnapshot, dxvk.readMemorySnapshot,
					dxvk.getMemoryHeapReports };
				return e.Complete() ? e : Exports{};
			}();
			resolved.store(&exports, std::memory_order_release);
			return exports;
		}
	}

	bool DxvkAvailable()
	{
		return Get().Complete();
	}

	OwnerScope::OwnerScope(const char* a_owner)
	{
		if (const auto& e = Get(); e.push && a_owner) {
			e.push(a_owner);
			m_pushed = true;
		}
	}

	OwnerScope::~OwnerScope()
	{
		if (m_pushed)
			Get().pop();
	}

	void SetOwner(IUnknown* a_resource, const char* a_owner, bool a_overwrite)
	{
		if (const auto& e = Get(); e.setOwner && a_resource && globals::d3d::device)
			e.setOwner(globals::d3d::device, a_resource, a_owner, a_overwrite ? TRUE : FALSE);
	}

	void RequestDxvkSnapshot()
	{
		if (const auto& e = Get(); e.requestSnapshot && globals::d3d::device)
			e.requestSnapshot(globals::d3d::device);
	}

	std::uint64_t ReadDxvkSnapshot(std::vector<DxvkMemoryResourceRow>& a_rows)
	{
		a_rows.clear();
		const auto& e = Get();
		if (!e.readSnapshot || !globals::d3d::device)
			return 0;

		// The count can grow between the two reads; retry until the copy fits.
		std::uint32_t count = 0;
		std::uint64_t serial = 0;
		if (FAILED(e.readSnapshot(globals::d3d::device, nullptr, 0, &count, &serial)))
			return 0;
		for (int attempt = 0; attempt < 4; ++attempt) {
			a_rows.resize(count + count / 8 + 64);
			std::uint32_t total = 0;
			const HRESULT hr = e.readSnapshot(globals::d3d::device, a_rows.data(), static_cast<std::uint32_t>(a_rows.size()), &total, &serial);
			if (FAILED(hr))
				break;
			if (hr == S_OK) {
				a_rows.resize(total);
				return serial;
			}
			count = total;
		}
		a_rows.clear();
		return 0;
	}

	void SetDxvkDefragPressure(float a_percent)
	{
		const auto set = DxvkExports::Get().setDefragPressurePercent;
		if (set && globals::d3d::device)
			set(globals::d3d::device, static_cast<std::uint32_t>(a_percent));
	}

	std::vector<DxvkMemoryHeapReport> DxvkHeapReports()
	{
		std::vector<DxvkMemoryHeapReport> reports;
		const auto& e = Get();
		if (!e.heapReports || !globals::d3d::device)
			return reports;

		reports.resize(VK_MAX_MEMORY_HEAPS);
		for (auto& report : reports) {
			report = {};
			report.version = DXVK_MEMORY_REPORT_VERSION;
		}
		std::uint32_t count = 0;
		if (FAILED(e.heapReports(globals::d3d::device, reports.data(), static_cast<std::uint32_t>(reports.size()), &count)))
			count = 0;
		reports.resize(std::min<std::size_t>(count, reports.size()));
		return reports;
	}
}
