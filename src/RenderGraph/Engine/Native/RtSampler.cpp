#include "RenderGraph/Engine/Native/RtSampler.h"

#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Switches.h"

#include <DbgHelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
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

		void Report(const std::vector<Sample>& a_samples, std::uint64_t a_offCpu, std::uint64_t a_frames, double a_cpuMsPerFrame)
		{
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
			if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
				SymRefreshModuleList(GetCurrentProcess());
			std::unordered_map<void*, std::string> cache;
			std::unordered_map<std::string, std::uint64_t> self, inclusive, ownLeaf, modules;
			for (const auto& sample : a_samples) {
				if (!sample.count)
					continue;
				const auto leaf = Name(sample.frames[0], cache);
				++self[leaf];
				++modules[ModuleName(ModuleOf(sample.frames[0]))];
				std::unordered_set<std::string> seen;
				bool ownFound = false;
				for (int i = 0; i < sample.count; ++i) {
					const auto name = Name(sample.frames[i], cache);
					if (seen.insert(name).second)
						++inclusive[name];
					if (!ownFound && (Tripwire::IsOwnCode(sample.frames[i]) || Tripwire::IsGameCode(sample.frames[i]))) {
						ownFound = true;
						if (i > 0)
							++ownLeaf[fmt::format("{} <- {}", leaf, name)];
					}
				}
			}
			const double total = static_cast<double>(a_samples.size());
			logger::info("[ORG native] render thread samples: {} on CPU, {} ticks off CPU, {} frames, {:.3f} ms/f thread CPU; ms/f below = share x that",
				a_samples.size(), a_offCpu, a_frames, a_cpuMsPerFrame);
			const auto print = [&](const char* a_title, const std::unordered_map<std::string, std::uint64_t>& a_map, std::size_t a_top) {
				std::vector<std::pair<std::string, std::uint64_t>> sorted(a_map.begin(), a_map.end());
				std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				logger::info("[ORG native] render thread samples, {}:", a_title);
				for (std::size_t i = 0; i < sorted.size() && i < a_top; ++i)
					logger::info("[ORG native]   {:5.1f}% {:6.3f} ms/f  {}", 100.0 * sorted[i].second / total, a_cpuMsPerFrame * sorted[i].second / total,
						sorted[i].first);
			};
			print("by module (leaf)", modules, 20);
			print("leaf functions", self, 60);
			print("inclusive", inclusive, 120);
			print("leaf outside CS and the game, with the first CS or game frame", ownLeaf, 60);
		}

		void Run(std::uint64_t a_seconds)
		{
			auto& s = S();
			const HANDLE thread = s.thread.load();
			timeBeginPeriod(1);
			std::vector<Sample> samples;
			samples.reserve(static_cast<std::size_t>(a_seconds) * 1100);
			std::uint64_t offCpu = 0;
			ULONG64 lastCycles = 0, startCycles = 0;
			QueryThreadCycleTime(thread, &startCycles);
			lastCycles = startCycles;
			const std::uint64_t frames0 = s.frames.load();
			const auto tsc0 = __rdtsc();
			const auto t0 = std::chrono::steady_clock::now();
			const auto end = t0 + std::chrono::seconds(a_seconds);
			while (std::chrono::steady_clock::now() < end && samples.size() + 1 < samples.capacity()) {
				Sleep(1);
				ULONG64 cycles = 0;
				if (QueryThreadCycleTime(thread, &cycles) && cycles == lastCycles) {
					++offCpu;
					continue;
				}
				lastCycles = cycles;
				Sample sample;
				if (SuspendThread(thread) == static_cast<DWORD>(-1))
					break;
				CONTEXT context{};
				context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				if (GetThreadContext(thread, &context))
					sample.count = static_cast<std::uint8_t>(Unwind(context, s.stackBase, s.stackLimit, sample.frames));
				ResumeThread(thread);
				samples.push_back(sample);
			}
			timeEndPeriod(1);
			ULONG64 endCycles = 0;
			QueryThreadCycleTime(thread, &endCycles);
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			const double cyclesPerMs = static_cast<double>(__rdtsc() - tsc0) / (seconds * 1000.0);
			const std::uint64_t frames = s.frames.load() - frames0;
			const double cpuMsPerFrame = frames && cyclesPerMs > 0.0 ? static_cast<double>(endCycles - startCycles) / cyclesPerMs / static_cast<double>(frames) : 0.0;
			FILETIME created{}, exited{}, kernel{}, user{};
			GetThreadTimes(thread, &created, &exited, &kernel, &user);
			logger::info("[ORG native] render thread sampler: thread {}, cycles {} to {}, user time {} ms, {} frames from another thread", GetThreadId(thread), startCycles,
				endCycles, (static_cast<std::uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime) / 10000, s.otherFrames.load());
			Report(samples, offCpu, frames, cpuMsPerFrame);
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
