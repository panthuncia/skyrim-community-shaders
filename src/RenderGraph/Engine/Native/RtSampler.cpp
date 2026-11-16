#include "RenderGraph/Engine/Native/RtSampler.h"

#include "RenderGraph/Engine/Native/AllocCounter.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

#include <DbgHelp.h>
#include <TlHelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace RenderGraph::Engine::Native::RtSampler
{
	namespace
	{
		constexpr int kMaxFrames = 48;

		struct Sample
		{
			std::uint8_t count = 0;
			void* frames[kMaxFrames]{};
		};

		struct State
		{
			std::atomic<HANDLE> thread{ nullptr };
			std::uintptr_t stackBase = 0;
			std::uintptr_t stackLimit = 0;
			std::atomic<std::uint64_t> frames{ 0 };
			std::atomic<bool> started{ false };
			DWORD threadId = 0;
			std::atomic<std::uint64_t> otherFrames{ 0 };
		};

		State& S()
		{
			static State s_state;
			return s_state;
		}

		std::uint64_t Seconds()
		{
			static const std::uint64_t s_seconds = Switches::Number("CS_ORG_RT_SAMPLE", 0);
			return s_seconds;
		}

		/**
		 * @brief The suspended thread's stack, unwound from a_context. No allocation and no lock of ours: the thread may hold the heap's.
		 * A fault while reading its stack ends the walk.
		 */
		int Unwind(CONTEXT a_context, std::uintptr_t a_base, std::uintptr_t a_limit, void** a_out)
		{
			int n = 0;
			__try {
				while (n < kMaxFrames && a_context.Rip) {
					a_out[n++] = reinterpret_cast<void*>(a_context.Rip);
					DWORD64 imageBase = 0;
					const auto* entry = RtlLookupFunctionEntry(a_context.Rip, &imageBase, nullptr);
					if (entry) {
						void* handler = nullptr;
						DWORD64 frame = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, a_context.Rip, const_cast<PRUNTIME_FUNCTION>(entry), &a_context, &handler, &frame,
							nullptr);
					} else {
						// A leaf function: the return address is at the stack pointer.
						if (a_context.Rsp < a_limit || a_context.Rsp + 8 > a_base)
							break;
						a_context.Rip = *reinterpret_cast<DWORD64*>(a_context.Rsp);
						a_context.Rsp += 8;
					}
					if (a_context.Rsp < a_limit || a_context.Rsp >= a_base)
						break;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return n;
		}

		HMODULE ModuleOf(const void* a_address)
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_address),
				&module);
			return module;
		}

		std::string ModuleName(HMODULE a_module)
		{
			if (!a_module)
				return "?";
			char path[MAX_PATH]{};
			GetModuleFileNameA(a_module, path, MAX_PATH);
			const char* slash = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
			return slash ? slash + 1 : path;
		}

		/** @brief The function holding a_address: the game's by Address Library ID, others by DbgHelp, else module+offset. */
		std::string Name(void* a_address, std::unordered_map<void*, std::string>& a_cache)
		{
			if (const auto it = a_cache.find(a_address); it != a_cache.end())
				return it->second;
			std::string name;
			const HMODULE module = ModuleOf(a_address);
			std::uint64_t id = 0, within = 0;
			if (Tripwire::IsGameCode(a_address) && Tripwire::GameFunction(a_address, id, within)) {
				name = fmt::format("game ID {}", id);
			} else {
				alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
				auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
				symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
				symbol->MaxNameLen = 511;
				DWORD64 displacement = 0;
				if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(a_address), &displacement, symbol))
					name = fmt::format("{}!{}", ModuleName(module), symbol->Name);
				else if (module)
					name = fmt::format("{}+{:#x}", ModuleName(module), reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module));
				else
					name = fmt::format("{:#x}", reinterpret_cast<std::uintptr_t>(a_address));
			}
			return a_cache.emplace(a_address, std::move(name)).first->second;
		}

		bool Has(const std::string& a_text, const char* a_part) { return a_text.find(a_part) != std::string::npos; }

		std::string Lower(std::string a_text)
		{
			for (auto& c : a_text)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return a_text;
		}

		/** @brief What kind of work a leaf function does: heap, lock or wait, memory copies, hashing, and so on; else "code". */
		const char* Kind(const std::string& a_leaf)
		{
			static const char* const kHeap[] = { "RtlAllocateHeap", "RtlFreeHeap", "RtlpAlloc", "RtlpFree", "RtlpLowFrag", "!malloc", "!free",
				"_aligned_", "operator new", "operator delete", "RtlSizeHeap", "HeapAlloc", "HeapFree", "_free_base", "_malloc_base", "RtlpInterlocked" };
			static const char* const kLock[] = { "SRWLock", "CriticalSection", "WaitOnAddress", "_Mtx_", "RtlpWait", "NtWaitFor", "ZwWaitFor",
				"RtlWakeAddress", "_Cnd_", "ConditionVariable", "NtDelayExecution", "SwitchToThread", "NtYield" };
			for (const char* part : kHeap)
				if (Has(a_leaf, part))
					return "heap";
			for (const char* part : kLock)
				if (Has(a_leaf, part))
					return "lock/wait";
			if (Has(a_leaf, "memcpy") || Has(a_leaf, "memset") || Has(a_leaf, "memmove") || Has(a_leaf, "memcmp"))
				return "mem";
			if (Has(a_leaf, "std::_Hash") || Has(a_leaf, "unordered_") || Has(a_leaf, "_Hash_representation") || Has(a_leaf, "_Fnv1a"))
				return "hash";
			if (Has(a_leaf, "std::_Func_impl") || Has(a_leaf, "std::_Func_class") || Has(a_leaf, "std::function"))
				return "std::function";
			if (Has(a_leaf, "basic_string") || Has(a_leaf, "strcmp") || Has(a_leaf, "strlen") || Has(a_leaf, "char_traits"))
				return "string";
			if (Has(a_leaf, "_Ref_count") || Has(a_leaf, "shared_ptr") || Has(a_leaf, "_Incref") || Has(a_leaf, "_Decref"))
				return "shared_ptr";
			if (Has(a_leaf, "std::_Tree") || Has(a_leaf, "std::map<") || Has(a_leaf, "std::set<"))
				return "tree map";
			return "code";
		}

		/** @brief Whether a leaf is a blocking wait in the kernel: the thread was going off the CPU, not using it. */
		bool IsWait(const std::string& a_leaf)
		{
			static const char* const kWaits[] = { "ZwWaitForAlertByThreadId", "NtWaitForSingleObject", "NtWaitForMultipleObjects", "ZwWaitForSingleObject",
				"ZwWaitForMultipleObjects", "ZwDelayExecution", "NtDelayExecution", "NtGdiDdDDIWaitForSynchronizationObjectFromCpu", "ZwRemoveIoCompletion",
				"NtWaitForWorkViaWorkerFactory", "ZwWaitForWorkViaWorkerFactory", "NtYieldExecution", "ZwYieldExecution" };
			for (const char* wait : kWaits)
				if (Has(a_leaf, wait))
					return true;
			return false;
		}

		/**
		 * @brief Who owns a sample's CPU time, and what kind of work its leaf does. A driver or API module met before the first CS frame
		 * (walking from the leaf up) owns the sample: the driver's own heap and locks are the driver's time. Else the first CS frame
		 * decides, by its namespace: BasicRHI (rhi::), ORG (org::), telemetry, CS.
		 */
		std::string Category(const Sample& a_sample, std::unordered_map<void*, std::string>& a_cache)
		{
			static const char* const kDriver[] = { "nvoglv64.dll", "vulkan-1.dll", "d3d12.dll", "d3d12core.dll", "dxgi.dll", "nvwgf2umx.dll",
				"nvgpucomp64.dll", "amdvlk64.dll", "amdxc64.dll", "amdxx64.dll", "d3d11.dll", "dxvk_d3d11.dll", "dxvk_dxgi.dll", "nvldumdx.dll",
				"win32u.dll", "gdi32full.dll", "nvapi64.dll", "sl.interposer.dll", "sl.common.dll", "sl.reflex.dll", "sl.pcl.dll" };
			const char* kind = Kind(Name(a_sample.frames[0], a_cache));
			for (int i = 0; i < a_sample.count; ++i) {
				void* frame = a_sample.frames[i];
				if (Tripwire::IsGameCode(frame))
					return fmt::format("game / {}", kind);
				if (Tripwire::IsOwnCode(frame)) {
					const std::string name = Name(frame, a_cache);
					const auto bang = name.find('!');
					const std::string symbol = bang == std::string::npos ? name : name.substr(bang + 1);
					const char* owner = "CS";
					if (symbol.starts_with("rhi::"))
						owner = "BasicRHI";
					else if (symbol.starts_with("org::"))
						owner = "ORG";
					else if (symbol.starts_with("basic_telemetry::") || symbol.starts_with("tracy::"))
						owner = "telemetry";
					else if (symbol.starts_with("DCLF::"))
						owner = "CS DCLF";
					return fmt::format("{} / {}", owner, kind);
				}
				const std::string module = Lower(ModuleName(ModuleOf(frame)));
				for (const char* driver : kDriver)
					if (module == driver)
						return fmt::format("driver/API ({}) / {}", module, kind);
			}
			return fmt::format("other ({}) / {}", Lower(ModuleName(ModuleOf(a_sample.frames[0]))), kind);
		}

		void Report(const std::string& a_thread, const std::vector<Sample>& a_samples, std::uint64_t a_offCpu, std::uint64_t a_frames,
			double a_cpuMsPerFrame, const AllocCounter::Totals& a_allocs, std::unordered_map<void*, std::string>& a_cache)
		{
			std::unordered_map<std::string, std::uint64_t> self, inclusive, ownLeaf, modules, categories, waits;
			std::size_t running = 0;
			for (const auto& sample : a_samples) {
				if (!sample.count)
					continue;
				const auto leaf = Name(sample.frames[0], a_cache);
				// A sample in a kernel wait: the thread ran a little in the tick and then blocked. Counted apart, by its first own caller,
				// and left out of the shares: the thread's CPU time is spread over the samples that were running.
				if (IsWait(leaf)) {
					std::string caller = "?";
					for (int i = 1; i < sample.count; ++i)
						if (Tripwire::IsOwnCode(sample.frames[i]) || Tripwire::IsGameCode(sample.frames[i])) {
							caller = Name(sample.frames[i], a_cache);
							break;
						}
					++waits[fmt::format("{} <- {}", leaf, caller)];
					continue;
				}
				++running;
				++self[leaf];
				++modules[ModuleName(ModuleOf(sample.frames[0]))];
				++categories[Category(sample, a_cache)];
				std::unordered_set<std::string> seen;
				bool ownFound = false;
				for (int i = 0; i < sample.count; ++i) {
					const auto name = Name(sample.frames[i], a_cache);
					if (seen.insert(name).second)
						++inclusive[name];
					if (!ownFound && (Tripwire::IsOwnCode(sample.frames[i]) || Tripwire::IsGameCode(sample.frames[i]))) {
						ownFound = true;
						if (i > 0)
							++ownLeaf[fmt::format("{} <- {}", leaf, name)];
					}
				}
			}
			const double total = static_cast<double>(std::max<std::size_t>(running, 1));
			const double frames = static_cast<double>(std::max<std::uint64_t>(a_frames, 1));
			logger::info("[ORG native] {} thread samples: {} running, {} in a kernel wait, {} ticks off CPU, {} frames, {:.3f} ms/f thread CPU; ms/f below = "
						 "share of the running samples x that",
				a_thread, running, a_samples.size() - running, a_offCpu, a_frames, a_cpuMsPerFrame);
			logger::info("[ORG native] {} thread heap (CS, ORG and BasicRHI operator new and delete): {:.1f} allocations, {:.0f} bytes, {:.1f} frees a frame",
				a_thread, static_cast<double>(a_allocs.allocs) / frames, static_cast<double>(a_allocs.bytes) / frames,
				static_cast<double>(a_allocs.frees) / frames);
			const auto print = [&](const char* a_title, const std::unordered_map<std::string, std::uint64_t>& a_map, std::size_t a_top) {
				std::vector<std::pair<std::string, std::uint64_t>> sorted(a_map.begin(), a_map.end());
				std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				logger::info("[ORG native] {} thread samples, {}:", a_thread, a_title);
				for (std::size_t i = 0; i < sorted.size() && i < a_top; ++i)
					logger::info("[ORG native]   {:5.1f}% {:6.3f} ms/f  {}", 100.0 * sorted[i].second / total, a_cpuMsPerFrame * sorted[i].second / total,
						sorted[i].first);
			};
			print("by owner and kind", categories, 40);
			{
				std::vector<std::pair<std::string, std::uint64_t>> sorted(waits.begin(), waits.end());
				std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				logger::info("[ORG native] {} thread samples, kernel waits (share of all samples):", a_thread);
				for (std::size_t i = 0; i < sorted.size() && i < 12; ++i)
					logger::info("[ORG native]   {:5.1f}%  {}", 100.0 * sorted[i].second / static_cast<double>(std::max<std::size_t>(a_samples.size(), 1)),
						sorted[i].first);
			}
			print("by module (leaf)", modules, 20);
			print("leaf functions", self, 60);
			print("inclusive", inclusive, 120);
			print("leaf outside CS and the game, with the first CS or game frame", ownLeaf, 60);
		}

		/** @brief A tracked thread's allocation sites: the first frame outside the allocator and the standard library, and its caller. */
		void ReportSites(const std::string& a_thread, std::uint32_t a_ring, std::uint64_t a_frames, std::unordered_map<void*, std::string>& a_cache)
		{
			const std::uint32_t count = AllocCounter::SiteCount(a_ring);
			if (!count)
				return;
			std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> sites;  // count, bytes
			const auto skipped = [](const std::string& a_name) {
				const auto bang = a_name.find('!');
				const std::string_view symbol = bang == std::string::npos ? std::string_view(a_name) : std::string_view(a_name).substr(bang + 1);
				return symbol.starts_with("std::") || symbol.find("operator new") != std::string_view::npos ||
				       symbol.find("AllocCounter") != std::string_view::npos || symbol.starts_with("ankerl::") || symbol.starts_with("fmt::") ||
				       symbol.find("_Allocate") != std::string_view::npos || symbol.starts_with("tbb::");
			};
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& site = AllocCounter::SiteAt(a_ring, i);
				// The first three frames outside the allocator and the standard library.
				std::string chain;
				int found = 0;
				for (std::uint32_t k = 0; k < site.count && found < 3; ++k) {
					std::string name = Name(site.frames[k], a_cache);
					if (skipped(name))
						continue;
					if (const auto bang = name.find('!'); bang != std::string::npos)
						name = name.substr(bang + 1);
					chain += found++ ? " <- " + name : name;
				}
				auto& entry = sites[chain.empty() ? std::string("?") : chain];
				entry.first += 1;
				entry.second += site.bytes;
			}
			std::vector<std::pair<std::string, std::pair<std::uint64_t, std::uint64_t>>> sorted(sites.begin(), sites.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
			// Past the ring's size the oldest sites were overwritten: the kept ones stand for all that were taken.
			const double kept = static_cast<double>(AllocCounter::SiteWritten(a_ring)) / static_cast<double>(count);
			const double scale = kept * static_cast<double>(AllocCounter::kSiteEvery) / static_cast<double>(std::max<std::uint64_t>(a_frames, 1));
			logger::info("[ORG native] {} thread allocation sites (every {}th allocation, {} kept; a frame = count x {} / frames):", a_thread,
				AllocCounter::kSiteEvery, count, AllocCounter::kSiteEvery);
			for (std::size_t i = 0; i < sorted.size() && i < 60; ++i)
				logger::info("[ORG native]   {:8.1f} allocations {:9.0f} bytes  {}", static_cast<double>(sorted[i].second.first) * scale,
					static_cast<double>(sorted[i].second.second) * scale, sorted[i].first);
		}

		struct Target
		{
			std::string name;
			HANDLE handle = nullptr;
			DWORD id = 0;
			// The render thread's stack bounds come from its thread block; the others' from each sample's stack pointer.
			bool tibBounds = false;
			std::vector<Sample> samples;
			std::uint64_t offCpu = 0;
			ULONG64 lastCycles = 0;
			ULONG64 startCycles = 0;
			AllocCounter::Totals allocs0;
		};

		std::string ThreadDescription(DWORD a_id)
		{
			std::string out;
			if (const HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, a_id)) {
				PWSTR text = nullptr;
				if (SUCCEEDED(GetThreadDescription(handle, &text)) && text) {
					for (const wchar_t* c = text; *c; ++c)
						out.push_back(*c < 128 ? static_cast<char>(*c) : '?');
					LocalFree(text);
				}
				CloseHandle(handle);
			}
			return out;
		}

		/** @brief Where a thread started (its Win32 start address), named: tells unnamed threads apart (a driver's, the game's). */
		std::string StartOf(HANDLE a_thread, std::unordered_map<void*, std::string>& a_cache)
		{
			using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
			static const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
			void* start = nullptr;
			constexpr ULONG kThreadQuerySetWin32StartAddress = 9;
			if (!query || query(a_thread, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) != 0 || !start)
				return {};
			return Name(start, a_cache);
		}

		/** @brief Every thread of this process, with an open handle (query rights) and its cycle time now. */
		struct ThreadCycles
		{
			DWORD id = 0;
			HANDLE handle = nullptr;
			ULONG64 cycles = 0;
		};

		std::vector<ThreadCycles> ProcessThreads()
		{
			std::vector<ThreadCycles> out;
			const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE)
				return out;
			THREADENTRY32 entry{ sizeof(entry) };
			for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
				if (entry.th32OwnerProcessID != GetCurrentProcessId())
					continue;
				ThreadCycles thread;
				thread.id = entry.th32ThreadID;
				thread.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
					entry.th32ThreadID);
				if (thread.handle && QueryThreadCycleTime(thread.handle, &thread.cycles))
					out.push_back(thread);
				else if (thread.handle)
					CloseHandle(thread.handle);
			}
			CloseHandle(snapshot);
			return out;
		}

		void Run(std::uint64_t a_seconds)
		{
			auto& s = S();
			std::vector<Target> targets;
			{
				Target render;
				render.name = "render";
				render.handle = s.thread.load();
				render.id = s.threadId;
				render.tibBounds = true;
				targets.push_back(std::move(render));
			}
			void* submit = nullptr;
			void* host = nullptr;
			RenderGraphRuntime::Get().HostThreadHandles(submit, host);
			const std::pair<const char*, void*> others[] = { { "submission", submit }, { "host", host } };
			for (const auto& [name, raw] : others) {
				HANDLE own = nullptr;
				if (raw && DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(raw), GetCurrentProcess(), &own,
							   THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
					Target target;
					target.name = name;
					target.handle = own;
					target.id = GetThreadId(own);
					targets.push_back(std::move(target));
				} else {
					logger::info("[ORG native] sampler: no {} thread to sample", name);
				}
			}
			// Every thread's CPU over the window (a table at the end), and DXVK's command stream thread sampled too in guest mode.
			std::vector<ThreadCycles> threadsAtStart = ProcessThreads();
			for (const auto& thread : threadsAtStart) {
				const std::string description = ThreadDescription(thread.id);
				if (description != "dxvk-cs" && description != "dxvk-submit")
					continue;
				HANDLE own = nullptr;
				if (DuplicateHandle(GetCurrentProcess(), thread.handle, GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
					Target target;
					target.name = description;
					target.handle = own;
					target.id = thread.id;
					targets.push_back(std::move(target));
				}
			}
			for (std::uint32_t i = 0; i < targets.size() && i < AllocCounter::kSiteRings; ++i)
				AllocCounter::TrackSites(i, targets[i].id);
			AllocCounter::Enable();
			std::unordered_map<std::uint32_t, AllocCounter::Totals> allocsBefore;
			AllocCounter::ForEach([&](std::uint32_t a_id, const AllocCounter::Totals& a_totals) { allocsBefore[a_id] = a_totals; });
			timeBeginPeriod(1);
			for (auto& target : targets) {
				target.samples.reserve(static_cast<std::size_t>(a_seconds) * 1100);
				QueryThreadCycleTime(target.handle, &target.startCycles);
				target.lastCycles = target.startCycles;
				target.allocs0 = AllocCounter::Of(target.id);
			}
			const std::uint64_t frames0 = s.frames.load();
			const auto tsc0 = __rdtsc();
			const auto t0 = std::chrono::steady_clock::now();
			const auto end = t0 + std::chrono::seconds(a_seconds);
			bool full = false;
			while (std::chrono::steady_clock::now() < end && !full) {
				Sleep(1);
				for (auto& target : targets) {
					ULONG64 cycles = 0;
					if (QueryThreadCycleTime(target.handle, &cycles) && cycles == target.lastCycles) {
						++target.offCpu;
						continue;
					}
					target.lastCycles = cycles;
					Sample sample;
					if (SuspendThread(target.handle) == static_cast<DWORD>(-1))
						continue;
					CONTEXT context{};
					context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
					if (GetThreadContext(target.handle, &context)) {
						std::uintptr_t base = s.stackBase;
						std::uintptr_t limit = s.stackLimit;
						MEMORY_BASIC_INFORMATION info{};
						if (!target.tibBounds && VirtualQuery(reinterpret_cast<void*>(context.Rsp), &info, sizeof(info))) {
							// The committed part of a stack runs from the stack pointer's page to the stack's top.
							limit = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
							base = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
						}
						sample.count = static_cast<std::uint8_t>(Unwind(context, base, limit, sample.frames));
					}
					ResumeThread(target.handle);
					target.samples.push_back(sample);
					full = full || target.samples.size() + 1 >= target.samples.capacity();
				}
			}
			timeEndPeriod(1);
			AllocCounter::StopSites();
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			const double cyclesPerMs = static_cast<double>(__rdtsc() - tsc0) / (seconds * 1000.0);
			const std::uint64_t frames = s.frames.load() - frames0;
			logger::info("[ORG native] sampler: {:.2f} s, {} frames ({:.3f} ms a frame), {} frames from another thread", seconds, frames,
				frames ? seconds * 1000.0 / static_cast<double>(frames) : 0.0, s.otherFrames.load());
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
			if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
				SymRefreshModuleList(GetCurrentProcess());
			std::unordered_map<void*, std::string> cache;
			for (auto& target : targets) {
				ULONG64 endCycles = 0;
				QueryThreadCycleTime(target.handle, &endCycles);
				const double cpuMsPerFrame = frames && cyclesPerMs > 0.0 ?
				                                 static_cast<double>(endCycles - target.startCycles) / cyclesPerMs / static_cast<double>(frames) :
				                                 0.0;
				const auto now = AllocCounter::Of(target.id);
				const AllocCounter::Totals allocs{ now.allocs - target.allocs0.allocs, now.bytes - target.allocs0.bytes, now.frees - target.allocs0.frees };
				logger::info("[ORG native] {} thread sampler: thread {}", target.name, target.id);
				Report(target.name, target.samples, target.offCpu, frames, cpuMsPerFrame, allocs, cache);
				const auto ring = static_cast<std::uint32_t>(&target - targets.data());
				if (ring < AllocCounter::kSiteRings)
					ReportSites(target.name, ring, frames, cache);
				if (!target.tibBounds)
					CloseHandle(target.handle);
			}
			// Every thread's CPU in the window, most first.
			{
				std::vector<std::pair<double, std::string>> rows;
				for (const auto& thread : threadsAtStart) {
					ULONG64 cycles = 0;
					if (QueryThreadCycleTime(thread.handle, &cycles) && cycles > thread.cycles && frames && cyclesPerMs > 0.0) {
						std::string label;
						for (const auto& target : targets)
							if (target.id == thread.id)
								label = target.name;
						if (label.empty())
							label = ThreadDescription(thread.id);
						if (label.empty())
							label = "start " + StartOf(thread.handle, cache);
						rows.push_back({ static_cast<double>(cycles - thread.cycles) / cyclesPerMs / static_cast<double>(frames),
							fmt::format("thread {:6} {}", thread.id, label.empty() ? "?" : label) });
					}
					CloseHandle(thread.handle);
				}
				std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				logger::info("[ORG native] CPU by thread in the window, ms a frame (thread cycle time):");
				for (std::size_t i = 0; i < rows.size() && i < 32; ++i)
					logger::info("[ORG native]   {:7.3f}  {}", rows[i].first, rows[i].second);
			}
			// Every thread's allocations in the window, most first.
			std::vector<std::pair<std::uint32_t, AllocCounter::Totals>> threads;
			AllocCounter::ForEach([&](std::uint32_t a_id, const AllocCounter::Totals& a_totals) {
				const auto found = allocsBefore.find(a_id);
				const AllocCounter::Totals before = found != allocsBefore.end() ? found->second : AllocCounter::Totals{};
				if (a_totals.allocs > before.allocs)
					threads.push_back({ a_id, { a_totals.allocs - before.allocs, a_totals.bytes - before.bytes, a_totals.frees - before.frees } });
			});
			std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a.second.allocs > b.second.allocs; });
			const double perFrame = 1.0 / static_cast<double>(std::max<std::uint64_t>(frames, 1));
			logger::info("[ORG native] heap by thread (CS, ORG and BasicRHI operator new and delete), a frame:");
			for (std::size_t i = 0; i < threads.size() && i < 24; ++i) {
				const auto& [id, totals] = threads[i];
				std::string label;
				for (const auto& target : targets)
					if (target.id == id)
						label = target.name;
				if (label.empty())
					label = ThreadDescription(id);
				logger::info("[ORG native]   thread {:6} {:28} {:8.1f} allocations {:10.0f} bytes {:8.1f} frees", id, label.empty() ? "?" : label,
					static_cast<double>(totals.allocs) * perFrame, static_cast<double>(totals.bytes) * perFrame, static_cast<double>(totals.frees) * perFrame);
			}
		}
	}

	void Frame()
	{
		if (!Seconds())
			return;
		auto& s = S();
		// The thread that ends frames now: the first frames can end on another (the loading screen's).
		if ((!s.thread.load() || GetCurrentThreadId() != s.threadId) && !s.started.load()) {
			if (const HANDLE old = s.thread.exchange(nullptr))
				CloseHandle(old);
			const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
			ULONG64 probe = 0;
			if (!handle || !QueryThreadCycleTime(handle, &probe)) {
				logger::warn("[ORG native] render thread sampler: the thread handle does not work (error {}); no samples", GetLastError());
				return;
			}
			// The stack's top from the thread block, its reserved bottom from the allocation that holds it.
			const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
			MEMORY_BASIC_INFORMATION info{};
			VirtualQuery(&info, &info, sizeof(info));
			s.stackLimit = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
			s.stackBase = reinterpret_cast<std::uintptr_t>(tib->StackBase);
			s.threadId = GetCurrentThreadId();
			s.thread.store(handle);
			logger::info("[ORG native] render thread sampler armed on thread {} (CS_ORG_RT_SAMPLE={} s): starts at the test's \"mark start\"", s.threadId, Seconds());
		}
		if (GetCurrentThreadId() != s.threadId)
			s.otherFrames.fetch_add(1, std::memory_order_relaxed);
		s.frames.fetch_add(1, std::memory_order_relaxed);
	}

	void Begin()
	{
		auto& s = S();
		if (!Seconds() || !s.thread.load() || s.started.exchange(true))
			return;
		std::thread([seconds = Seconds()] { Run(seconds); }).detach();
	}
}
