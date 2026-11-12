#include "HangWatchdog.h"

#include "CrashDump.h"
#include "Testing/Testing.h"
#ifdef CS_HAS_RENDER_GRAPH
#	include "RenderGraph/RenderGraphRuntime.h"
#	include <OpenRenderGraph/GraphHost.h>
#endif

#include <ctime>
#include <thread>

namespace HangWatchdog
{
	namespace
	{
		constexpr std::uint64_t kDefaultSeconds = 15;
		constexpr int kMaxReports = 3;
		constexpr std::size_t kKeptReports = 6;           // older CommunityShaders-Hang-* pairs are deleted when a new one is written
		constexpr std::uint32_t kHelperTimeoutMs = 180000;  // a dump of a large process can take a while

		std::atomic<bool> started{ false };   // Start ran
		std::atomic<bool> watching{ false };  // its settings are read: the beats count
		std::atomic<std::uint64_t> lastBeat{ 0 };  // GetTickCount64 at the last Present; 0 before the first after Start
		std::atomic<std::uint64_t> beats{ 0 };
		std::atomic<std::uint32_t> renderThread{ 0 };  // the thread of the last Present
		std::uint64_t thresholdMs = kDefaultSeconds * 1000;
		std::atomic<std::uint64_t> progressEvents{ 0 };  // loading progress: cells attached, NoteProgress calls

		/** @brief Cells attaching while a load runs: progress, even when nothing presents. */
		class CellAttachSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent*, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				progressEvents.fetch_add(1, std::memory_order_relaxed);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		/** @brief The progress count: shaders compiled and loading events. A change is a beat. */
		std::uint64_t Progress() { return progressEvents.load(std::memory_order_relaxed) + Testing::GetCompileProgress().done; }

		// CS_TEST_HANG=<seconds> (a development switch): the render thread sleeps past the threshold once, that long after the watch
		// starts, to exercise the watchdog and the helper end to end.
		std::uint64_t testHangAt = 0;

		std::uint64_t ReadSeconds(const char* a_name, std::uint64_t a_default)
		{
			const char* value = std::getenv(a_name);
			return value && *value ? std::strtoull(value, nullptr, 10) : a_default;
		}

		std::string Stamp()
		{
			const auto t = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &t);
			char text[32]{};
			std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
			return text;
		}

		bool GameHasForeground()
		{
			const auto window = GetForegroundWindow();
			DWORD process = 0;
			if (window)
				GetWindowThreadProcessId(window, &process);
			return process == GetCurrentProcessId() && !IsIconic(window);
		}

		/** @brief Deletes all but the newest kKeptReports hang reports beside the log. */
		void PruneReports(const std::filesystem::path& a_dir)
		{
			std::vector<std::filesystem::directory_entry> reports;
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(a_dir, error))
				if (entry.is_regular_file() && entry.path().filename().string().starts_with("CommunityShaders-Hang-"))
					reports.push_back(entry);
			std::ranges::sort(reports, [](const auto& a, const auto& b) { return a.path().filename() > b.path().filename(); });
			// Two files a report: the .dmp and the .txt.
			for (std::size_t i = kKeptReports * 2; i < reports.size(); ++i)
				std::filesystem::remove(reports[i].path(), error);
		}

		/** @brief dxvk_d3d11's DxvkViewTraceEntry (d3d11_view_srv.cpp, DXVK_VIEW_TRACE=1). */
		struct DestroyedView
		{
			std::uintptr_t view, viewEnd, stream, streamEnd;
			std::uint32_t kind, thread;
			std::uint64_t tick;
			std::uint32_t frameCount;
			void* frames[24];
		};

		std::string ModuleOffset(const void* a_address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(a_address), &module))
				return fmt::format("{}", a_address);
			char path[MAX_PATH]{};
			GetModuleFileNameA(module, path, MAX_PATH);
			return fmt::format("{}+{:#x}", std::filesystem::path(path).filename().string(), reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module));
		}

		/**
		 * @brief With DXVK_VIEW_TRACE=1: logs the views freed while the stuck thread's registers point into them, with the stacks that
		 * released and destroyed them. A thread spinning on a freed view's lock names who freed the view.
		 */
		void LogFreedViews(std::uint32_t a_thread)
		{
			const auto dxvk = GetModuleHandleW(L"dxvk_d3d11.dll");
			using Find = std::uint32_t(__stdcall*)(std::uintptr_t, DestroyedView*, std::uint32_t);
			const auto find = dxvk ? reinterpret_cast<Find>(GetProcAddress(dxvk, "dxvkFindDestroyedView")) : nullptr;
			if (!find)
				return;
			const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, a_thread);
			if (!thread)
				return;
			CONTEXT context{};
			context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
			bool read = false;
			if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
				read = GetThreadContext(thread, &context) != FALSE;
				ResumeThread(thread);
			}
			CloseHandle(thread);
			if (!read)
				return;
			// The registers, then the stack: a thread that sleeps in a spin keeps its caller's registers saved on the stack.
			std::vector<std::pair<std::string, std::uintptr_t>> values = { { "rax", context.Rax }, { "rbx", context.Rbx }, { "rcx", context.Rcx },
				{ "rdx", context.Rdx }, { "rsi", context.Rsi }, { "rdi", context.Rdi }, { "rbp", context.Rbp }, { "r8", context.R8 }, { "r9", context.R9 },
				{ "r10", context.R10 }, { "r11", context.R11 }, { "r12", context.R12 }, { "r13", context.R13 }, { "r14", context.R14 }, { "r15", context.R15 } };
			MEMORY_BASIC_INFORMATION region{};
			if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &region, sizeof(region)) && region.State == MEM_COMMIT) {
				const auto end = (std::min)(reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize, static_cast<std::uintptr_t>(context.Rsp) + 0x10000);
				for (auto at = static_cast<std::uintptr_t>(context.Rsp) & ~std::uintptr_t(7); at + 8 <= end; at += 8)
					values.emplace_back(fmt::format("[rsp+{:#x}]", at - context.Rsp), *reinterpret_cast<const std::uintptr_t*>(at));
			}
			std::unordered_set<std::uintptr_t> reported;
			for (const auto& [name, value] : values) {
				DestroyedView entries[4]{};
				const auto count = find(value, entries, 4);
				for (std::uint32_t i = 0; i < count; ++i) {
					const auto& e = entries[i];
					if (!reported.insert(e.view ^ (std::uintptr_t(e.kind) << 63)).second)
						continue;
					std::string stack;
					for (std::uint32_t f = 0; f < e.frameCount && f < 24; ++f)
						stack += fmt::format("\n    {}", ModuleOffset(e.frames[f]));
					logger::critical("[HANG] {} = {:#x} is in a freed view {:#x} (stream state {:#x}); its {} on thread {}, {} ms ago:{}", name, value, e.view, e.stream,
						e.kind ? "destruction" : "last public Release", e.thread, GetTickCount64() - e.tick, stack);
				}
			}
			if (reported.empty())
				logger::info("[HANG] no register or stack value of the stuck thread points into a recently freed shader resource view");
		}

		void Report(std::uint64_t a_stalledMs)
		{
			const auto dir = logger::log_directory();
			if (!dir)
				return;
			const auto base = *dir / ("CommunityShaders-Hang-" + Stamp());
			const auto text = std::filesystem::path(base).concat(".txt"), dump = std::filesystem::path(base).concat(".dmp");
			const auto thread = renderThread.load(std::memory_order_relaxed);
			logger::critical("[HANG] the render thread (thread {}) has not presented for {:.1f} s; writing every thread's stack to {}", thread,
				a_stalledMs / 1000.0, text.filename().string());
			LogFreedViews(thread);
#ifdef CS_HAS_RENDER_GRAPH
			// The render graph host's runs in flight and what each waits for (read racily): a stall waiting on the GPU names its points.
			if (auto* host = RenderGraphRuntime::Get().Host())
				logger::critical("[HANG] render graph host state: {}", host->DescribeAsyncState());
			// The order of the graph's work in DXVK's stream and on the compute and copy queues: a GPU wait cycle shows here.
			if (RenderGraphRuntime::Get().Host())
				logger::critical("[HANG] render graph stream order: {}", RenderGraphRuntime::Get().DescribeStreamOrder());
#endif
			spdlog::default_logger()->flush();
			PruneReports(*dir);
			const auto begin = GetTickCount64();
			std::uint32_t error = 0;
			switch (CrashDump::RequestHangDump(thread, dump, text, kHelperTimeoutMs, error)) {
			case CrashDump::HangDumpResult::Written:
				logger::critical("[HANG] wrote {} and {} in {:.1f} s", text.filename().string(), dump.filename().string(), (GetTickCount64() - begin) / 1000.0);
				break;
			case CrashDump::HangDumpResult::Failed:
				logger::error("[HANG] the crash dump helper could not write {} (Win32 error {}); see {} for the stacks, if written", dump.filename().string(), error,
					text.filename().string());
				break;
			case CrashDump::HangDumpResult::NoHelper:
				logger::error("[HANG] the crash dump helper is not running: no report");
				break;
			case CrashDump::HangDumpResult::NoAnswer:
				logger::error("[HANG] the crash dump helper did not answer within {} s: no report", kHelperTimeoutMs / 1000);
				break;
			}
			spdlog::default_logger()->flush();
		}

		void Watch()
		{
			int reports = 0;
			bool stalled = false;  // the current stall was reported (or found, past the report limit)
			bool rendersUnfocused = false;
			std::uint64_t lastBeats = 0, lastMinuteLogged = 0;
			std::uint64_t lastProgress = Progress(), lastProgressTick = 0;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				// Shader compiles and loading progress count as beats: a cold shader cache or a long load is not a hang. A load that
				// makes no progress and does not present still is.
				if (const auto progress = Progress(); progress != lastProgress) {
					lastProgress = progress;
					lastProgressTick = GetTickCount64();
				}
				const auto beat = (std::max)(lastBeat.load(std::memory_order_acquire), lastProgressTick);
				if (lastBeat.load(std::memory_order_acquire) == 0)
					continue;
				const auto count = beats.load(std::memory_order_relaxed);
				const bool foreground = GameHasForeground();
				// Presents while another window has the foreground: this game keeps rendering unfocused, so a stall then is a hang too.
				if (!foreground && count != lastBeats)
					rendersUnfocused = true;
				lastBeats = count;
				const auto now = GetTickCount64();
				const auto stalledMs = now > beat ? now - beat : 0;
				if (stalledMs < thresholdMs) {
					if (stalled) {
						logger::warn("[HANG] the render thread presented again; it had stopped for over {:.1f} s", static_cast<double>(thresholdMs) / 1000.0);
						stalled = false;
					}
					continue;
				}
				if ((!foreground && !rendersUnfocused) || IsDebuggerPresent())
					continue;
				if (stalled) {
					// Still stuck: one line a minute, so the log shows how long.
					if (const auto minute = stalledMs / 60000; minute != lastMinuteLogged) {
						lastMinuteLogged = minute;
						logger::warn("[HANG] the render thread has still not presented after {} s", stalledMs / 1000);
						spdlog::default_logger()->flush();
					}
					continue;
				}
				stalled = true;
				lastMinuteLogged = stalledMs / 60000;
				if (reports >= kMaxReports) {
					logger::warn("[HANG] the render thread has not presented for {:.1f} s; no report ({} written this session already)", stalledMs / 1000.0,
						kMaxReports);
					continue;
				}
				++reports;
				Report(stalledMs);
				// A test run ends here with the outcome hang.
				Testing::ReportHang(stalledMs / 1000.0);
			}
		}
	}

	void Start()
	{
		if (started.exchange(true))
			return;
		const auto seconds = ReadSeconds("CS_HANG_WATCHDOG", kDefaultSeconds);
		if (seconds == 0) {
			logger::info("[HANG] the render thread watchdog is off (CS_HANG_WATCHDOG=0)");
			return;
		}
		if (!CrashDump::Available()) {
			logger::warn("[HANG] the render thread watchdog is off: the crash dump helper is not running");
			return;
		}
		thresholdMs = seconds * 1000;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			static CellAttachSink sink;
			events->AddEventSink<RE::TESCellAttachDetachEvent>(&sink);
		}
		testHangAt = ReadSeconds("CS_TEST_HANG", 0);
		logger::info("[HANG] the render thread watchdog reports a Present stall of {} s or more", seconds);
		if (testHangAt)
			logger::warn("[HANG] CS_TEST_HANG: the render thread stalls for {} s, {} s after the watch starts", seconds + 10, testHangAt);
		std::thread(Watch).detach();
		watching.store(true, std::memory_order_release);
	}

	void OnPresent()
	{
		if (!watching.load(std::memory_order_acquire))
			return;
		const auto now = GetTickCount64();
		if (lastBeat.load(std::memory_order_relaxed) == 0 && testHangAt)
			testHangAt = now + testHangAt * 1000;
		// Every beat: a loading screen presents from one of the engine's job threads, the game from its main thread.
		renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		beats.fetch_add(1, std::memory_order_relaxed);
		lastBeat.store(now, std::memory_order_release);
		if (testHangAt && now >= testHangAt) {
			testHangAt = 0;
			logger::warn("[HANG] CS_TEST_HANG: the render thread sleeps now");
			std::this_thread::sleep_for(std::chrono::milliseconds(thresholdMs + 10000));
		}
	}
}
