#include "RenderGraph/Engine/Tripwire/TripwireHandles.h"

#include "RenderGraph/Switches.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Meters the render thread's handle calls when CS_ORG_D3D11_METER=1.
namespace RenderGraph::Engine::Tripwire
{
	namespace
	{
		constexpr std::size_t kMeterKinds = static_cast<std::size_t>(Kind::Count);

		enum class Family : std::uint8_t
		{
			StateSet,
			StateGet,
			Draw,
			MapUpdate,
			ClearCopy,
			Query,
			Create,
			ComPrivate,
			Desc,
			SwapChain,
			Other,
			Count
		};
		constexpr std::size_t kFamilies = static_cast<std::size_t>(Family::Count);

		constexpr const char* kFamilyNames[] = { "state sets", "state gets", "draws and dispatches", "maps and updates", "clears and copies", "queries",
			"creates", "COM and private data", "descriptions", "swap chain", "other" };
		// Tracy plots need names that live for the process.
		constexpr const char* kFamilyCallPlots[] = { "CS.D3D11.StateSet.calls", "CS.D3D11.StateGet.calls", "CS.D3D11.Draw.calls",
			"CS.D3D11.MapUpdate.calls", "CS.D3D11.ClearCopy.calls", "CS.D3D11.Query.calls", "CS.D3D11.Create.calls", "CS.D3D11.ComPrivate.calls",
			"CS.D3D11.Desc.calls", "CS.D3D11.SwapChain.calls", "CS.D3D11.Other.calls" };
		constexpr const char* kFamilyTimePlots[] = { "CS.D3D11.StateSet.us", "CS.D3D11.StateGet.us", "CS.D3D11.Draw.us", "CS.D3D11.MapUpdate.us",
			"CS.D3D11.ClearCopy.us", "CS.D3D11.Query.us", "CS.D3D11.Create.us", "CS.D3D11.ComPrivate.us", "CS.D3D11.Desc.us",
			"CS.D3D11.SwapChain.us", "CS.D3D11.Other.us" };
		static_assert(std::size(kFamilyNames) == kFamilies && std::size(kFamilyCallPlots) == kFamilies && std::size(kFamilyTimePlots) == kFamilies);

		Family FamilyOf(Kind a_kind, std::string_view a_method)
		{
			if (a_kind == Kind::SwapChain)
				return Family::SwapChain;
			if (a_method == "QueryInterface" || a_method == "GetDevice" || a_method.find("PrivateData") != std::string_view::npos)
				return Family::ComPrivate;
			if (a_kind == Kind::Device)
				return a_method.starts_with("Create") ? Family::Create : Family::Other;
			if (a_kind != Kind::DeviceContext)
				return Family::Desc;
			if (a_method.starts_with("Draw") || a_method.starts_with("Dispatch") || a_method == "ExecuteCommandList")
				return Family::Draw;
			if (a_method == "Map" || a_method == "Unmap" || a_method == "UpdateSubresource")
				return Family::MapUpdate;
			if ((a_method.starts_with("Clear") && a_method != "ClearState") || a_method.starts_with("Copy") || a_method == "GenerateMips" ||
				a_method == "ResolveSubresource")
				return Family::ClearCopy;
			if (a_method == "Begin" || a_method == "End" || a_method == "GetData" || a_method.ends_with("Predication"))
				return Family::Query;
			// The stage's two letters come first, then Set or Get.
			if (a_method.size() > 5 && a_method.substr(2, 3) == "Get")
				return Family::StateGet;
			if ((a_method.size() > 5 && a_method.substr(2, 3) == "Set") || a_method == "ClearState")
				return Family::StateSet;
			return Family::Other;
		}

		struct MeterState
		{
			Detail::MeterCell last[kMeterKinds][WatchSet::kMaxSlots]{};
			Detail::MeterCell window[kMeterKinds][WatchSet::kMaxSlots]{};
			Family family[kMeterKinds][WatchSet::kMaxSlots]{};
			std::uint32_t frames = 0;
			std::uint64_t windowThreadCycles = 0;
			ULONG64 lastThreadCycles = 0;
			std::uint64_t tsc0 = 0;
			LARGE_INTEGER qpc0{};
			double cyclesPerUs = 0.0;
		};
	}

	void MeterFrame()
	{
		static const bool on = Switches::On("CS_ORG_D3D11_METER");
		if (!on)
			return;
		static MeterState s_state;
		auto& st = s_state;
		// Frames can end on another thread, such as the loading screen's.
		if (Detail::g_meterThread.load(std::memory_order_relaxed) != Detail::t_threadId) {
			Detail::g_meterThread.store(Detail::t_threadId, std::memory_order_relaxed);
			std::memcpy(st.last, Detail::g_meter, sizeof(st.last));
			std::memset(st.window, 0, sizeof(st.window));
			st.frames = 0;
			st.windowThreadCycles = 0;
			for (std::size_t k = 0; k < kMeterKinds; ++k) {
				const auto names = SlotNames(static_cast<Kind>(k));
				for (std::size_t i = 0; i < WatchSet::kMaxSlots; ++i)
					st.family[k][i] = i < names.size() ? FamilyOf(static_cast<Kind>(k), names[i]) : Family::Other;
			}
			st.tsc0 = __rdtsc();
			QueryPerformanceCounter(&st.qpc0);
			QueryThreadCycleTime(GetCurrentThread(), &st.lastThreadCycles);
			logger::info("[ORG native] D3D11 meter on thread {} (CS_ORG_D3D11_METER=1): the render thread's handle calls by family, every 600 frames",
				Detail::t_threadId);
			return;
		}
		// Calibrate the TSC rate against the performance counter once a second has passed.
		LARGE_INTEGER qpc{}, freq{};
		QueryPerformanceCounter(&qpc);
		QueryPerformanceFrequency(&freq);
		const double us = static_cast<double>(qpc.QuadPart - st.qpc0.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
		if (us > 1e6)
			st.cyclesPerUs = static_cast<double>(__rdtsc() - st.tsc0) / us;
		std::uint64_t familyCalls[kFamilies]{};
		std::uint64_t familyCycles[kFamilies]{};
		for (std::size_t k = 0; k < kMeterKinds; ++k) {
			for (std::size_t i = 0; i < WatchSet::kMaxSlots; ++i) {
				const auto& now = Detail::g_meter[k][i];
				auto& last = st.last[k][i];
				const std::uint64_t calls = now.calls - last.calls, cycles = now.cycles - last.cycles;
				if (!calls)
					continue;
				last = now;
				st.window[k][i].calls += calls;
				st.window[k][i].cycles += cycles;
				familyCalls[static_cast<std::size_t>(st.family[k][i])] += calls;
				familyCycles[static_cast<std::size_t>(st.family[k][i])] += cycles;
			}
		}
		ULONG64 threadCycles = 0;
		QueryThreadCycleTime(GetCurrentThread(), &threadCycles);
		st.windowThreadCycles += threadCycles - st.lastThreadCycles;
		st.lastThreadCycles = threadCycles;
		if (st.cyclesPerUs <= 0.0)
			return;
		for (std::size_t f = 0; f < kFamilies; ++f) {
			TracyPlot(kFamilyCallPlots[f], static_cast<int64_t>(familyCalls[f]));
			TracyPlot(kFamilyTimePlots[f], static_cast<double>(familyCycles[f]) / st.cyclesPerUs);
		}
		if (++st.frames < 600)
			return;
		const double frames = st.frames;
		const auto ms = [&](std::uint64_t a_cycles) { return static_cast<double>(a_cycles) / st.cyclesPerUs / 1000.0 / frames; };
		std::uint64_t sumCalls[kFamilies]{}, sumCycles[kFamilies]{};
		struct Entry
		{
			std::size_t kind, slot;
			std::uint64_t calls, cycles;
		};
		std::vector<Entry> entries;
		std::uint64_t totalCycles = 0, totalCalls = 0;
		for (std::size_t k = 0; k < kMeterKinds; ++k)
			for (std::size_t i = 0; i < WatchSet::kMaxSlots; ++i)
				if (const auto& w = st.window[k][i]; w.calls) {
					sumCalls[static_cast<std::size_t>(st.family[k][i])] += w.calls;
					sumCycles[static_cast<std::size_t>(st.family[k][i])] += w.cycles;
					totalCycles += w.cycles;
					totalCalls += w.calls;
					entries.push_back({ k, i, w.calls, w.cycles });
				}
		std::string line;
		for (std::size_t f = 0; f < kFamilies; ++f)
			if (sumCalls[f])
				line += fmt::format("; {} {:.0f}/f {:.3f} ms/f", kFamilyNames[f], static_cast<double>(sumCalls[f]) / frames, ms(sumCycles[f]));
		logger::info("[ORG native] D3D11 meter ({} frames, render thread): {:.0f} calls/f, {:.3f} ms/f of {:.3f} ms/f thread CPU{}", st.frames,
			static_cast<double>(totalCalls) / frames, ms(totalCycles), ms(st.windowThreadCycles), line);
		std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.cycles > b.cycles; });
		line.clear();
		for (std::size_t e = 0; e < entries.size() && e < 20; ++e) {
			const auto names = SlotNames(static_cast<Kind>(entries[e].kind));
			std::string_view iface = InterfaceName(static_cast<Kind>(entries[e].kind));
			if (iface.starts_with("ID3D11"))
				iface.remove_prefix(6);
			line += fmt::format("{}{}::{} {:.0f}/f {:.3f} ms/f", e ? "; " : "", iface,
				entries[e].slot < names.size() ? names[entries[e].slot] : "?", static_cast<double>(entries[e].calls) / frames, ms(entries[e].cycles));
		}
		logger::info("[ORG native] D3D11 meter, largest methods: {}", line);
		std::memset(st.window, 0, sizeof(st.window));
		st.windowThreadCycles = 0;
		st.frames = 0;
	}
}
