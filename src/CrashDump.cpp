#include "CrashDump.h"
#include "CrashDumpShared.h"
#include "Testing/Testing.h"

// The dumps are written out of process, by CommunityShadersCrashDump.exe beside this DLL (tools/crash-dump-helper). MiniDumpWriteDump
// suspends every other thread and then allocates from the process heap and takes loader locks: called in here, a thread suspended
// while holding the heap's lock hung the game in the dump (2026-10-11). The crash handler only writes the faulting thread and its
// exception pointers into a shared section (CrashDumpShared.h), signals the helper and waits for it: no allocation, no lock.

#include <cstring>
#include <mutex>
#include <thread>

namespace CrashDump
{
	namespace
	{
		struct Helper
		{
			HANDLE request = nullptr;
			HANDLE done = nullptr;
			Shared* shared = nullptr;
		};
		Helper helper;

		// A hang request in flight: the crash path waits for it before it uses the section.
		std::atomic<bool> hangBusy{ false };
		std::mutex hangMutex;

		// How long the faulting thread waits for the helper's dump before the exception goes on.
		constexpr DWORD kCrashDumpWaitMs = 120000;

		// First-chance faults logged per session; a game or driver that probes memory in a loop would flood the log otherwise.
		constexpr std::uint32_t kFirstChanceLogLimit = 16;

		// The unhandled exception filter that was installed before ours; ours calls it after the dump.
		LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;

		bool IsFault(DWORD a_code)
		{
			return a_code == EXCEPTION_ACCESS_VIOLATION || a_code == EXCEPTION_ILLEGAL_INSTRUCTION || a_code == EXCEPTION_STACK_OVERFLOW ||
			       a_code == EXCEPTION_INT_DIVIDE_BY_ZERO || a_code == 0xC0000409 /* fast-fail / stack cookie */;
		}

		/** @brief The faulting code as module+offset, and an access violation's kind and address, for the log. */
		std::string Describe(const EXCEPTION_RECORD& a_record)
		{
			const auto* address = a_record.ExceptionAddress;
			HMODULE module = nullptr;
			char moduleName[MAX_PATH] = "no module";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) &&
				module) {
				char path[MAX_PATH]{};
				if (GetModuleFileNameA(module, path, MAX_PATH)) {
					const char* slash = std::strrchr(path, '\\');
					strncpy_s(moduleName, slash ? slash + 1 : path, _TRUNCATE);
				}
			}
			const auto offset = module ? reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module) : 0;
			// ExceptionInformation[0] of an access violation: 0 read, 1 write, 8 execute.
			std::string access;
			if (a_record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && a_record.NumberParameters >= 2) {
				const auto kind = a_record.ExceptionInformation[0];
				const char* verb = "reading";
				if (kind == 8)
					verb = "executing";
				else if (kind == 1)
					verb = "writing";
				access = fmt::format(", {} {:#x}", verb, a_record.ExceptionInformation[1]);
			}
			return fmt::format("{:#x} at {} ({}+{:#x}{}) on thread {}", a_record.ExceptionCode, fmt::ptr(address), moduleName, offset, access,
				GetCurrentThreadId());
		}

		/**
		 * @brief First chance: a vectored handler runs before the frame handlers of the code that raised the exception, so a handler
		 * further down may still catch it (a driver's probe, a __try). It only logs: the session's one crash dump is kept for the fault
		 * that ends the process (OnUnhandled).
		 */
		LONG CALLBACK OnFirstChance(EXCEPTION_POINTERS* a_exception)
		{
			if (!a_exception || !a_exception->ExceptionRecord)
				return EXCEPTION_CONTINUE_SEARCH;
			const DWORD code = a_exception->ExceptionRecord->ExceptionCode;
			// Ignore the benign codes the game raises constantly (C++ EH, debugger notifications). A stack overflow has no stack
			// left to format a line on; the unhandled path dumps it.
			if (!IsFault(code) || code == EXCEPTION_STACK_OVERFLOW)
				return EXCEPTION_CONTINUE_SEARCH;

			static std::atomic<std::uint32_t> logged{ 0 };
			const auto index = logged.fetch_add(1, std::memory_order_relaxed);
			if (index >= kFirstChanceLogLimit)
				return EXCEPTION_CONTINUE_SEARCH;
			logger::warn("[CRASH] first-chance exception {}; a handler may still catch it{}", Describe(*a_exception->ExceptionRecord),
				index + 1 == kFirstChanceLogLimit ? " (later first-chance faults are not logged)" : "");
			spdlog::default_logger()->flush();
			return EXCEPTION_CONTINUE_SEARCH;
		}

		/** @brief Asks the helper for the crash dump and waits for it. Calls nothing that needs much stack before the request. */
		void WriteCrashDump(EXCEPTION_POINTERS* a_exception)
		{
			// A hang dump being written holds the section; the helper answers it first.
			for (DWORD waited = 0; hangBusy.load(std::memory_order_acquire) && waited < kCrashDumpWaitMs; waited += 50)
				Sleep(50);
			helper.shared->threadId = GetCurrentThreadId();
			helper.shared->kind = RequestKind::kCrash;
			helper.shared->exceptionPointers = reinterpret_cast<std::uintptr_t>(a_exception);
			ResetEvent(helper.done);
			SetEvent(helper.request);
			if (WaitForSingleObject(helper.done, kCrashDumpWaitMs) != WAIT_OBJECT_0)
				logger::critical("[CRASH] the crash dump helper did not answer within {} s", kCrashDumpWaitMs / 1000);
			else if (helper.shared->written)
				logger::critical("[CRASH] wrote CommunityShaders.dmp ({} KB)", helper.shared->bytes / 1024);
			else
				logger::critical("[CRASH] CommunityShaders.dmp could not be written (error {})", helper.shared->error);
			spdlog::default_logger()->flush();
		}

		/**
		 * @brief Last chance: no frame handler took the exception, so the process is ending. Writes the session's crash dump for this
		 * fault, then hands the exception to the filter installed before this one (a crash logger, the CRT's).
		 */
		LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* a_exception)
		{
			static std::atomic<bool> written{ false };
			if (a_exception && a_exception->ExceptionRecord && !written.exchange(true)) {
				const auto& record = *a_exception->ExceptionRecord;
				if (record.ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
					// Little stack is left: the dump first, the log line after it.
					if (helper.shared)
						WriteCrashDump(a_exception);
					logger::critical("[CRASH] unhandled stack overflow on thread {}", GetCurrentThreadId());
					spdlog::default_logger()->flush();
				} else {
					// A test run's status first (best effort): the dump can take a while, and the harness may end the process.
					Testing::ReportCrash(static_cast<std::uint32_t>(record.ExceptionCode), record.ExceptionAddress);
					logger::critical("[CRASH] unhandled exception {}. {}", Describe(record),
						helper.shared ? "Writing CommunityShaders.dmp" : "No dump: the helper is not running");
					spdlog::default_logger()->flush();
					if (helper.shared)
						WriteCrashDump(a_exception);
				}
			}
			return previousFilter ? previousFilter(a_exception) : EXCEPTION_CONTINUE_SEARCH;
		}

		void StartHelper()
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&StartHelper), &module);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(module, modulePath, MAX_PATH);
			const auto exe = std::filesystem::path{ modulePath }.parent_path() / L"CommunityShadersCrashDump.exe";
			wchar_t gamePath[MAX_PATH]{};
			GetModuleFileNameW(nullptr, gamePath, MAX_PATH);
			const auto dump = std::filesystem::path{ gamePath }.parent_path() / L"CommunityShaders.dmp";
			if (!std::filesystem::exists(exe)) {
				logger::warn("[CRASH] {} is missing: crashes and hangs write no dump", exe.string());
				return;
			}

			SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
			HANDLE process = nullptr;
			DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
				PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, TRUE, 0);
			const HANDLE request = CreateEventW(&inherit, TRUE, FALSE, nullptr);
			const HANDLE done = CreateEventW(&inherit, TRUE, FALSE, nullptr);
			const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
			auto* shared = section ? static_cast<Shared*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(Shared))) : nullptr;
			if (!process || !request || !done || !shared) {
				logger::warn("[CRASH] the crash dump helper's handles could not be made (error {}): crashes and hangs write no dump", GetLastError());
				return;
			}

			// The helper inherits these four handles only, not every inheritable handle of the game.
			HANDLE inherited[4]{ process, request, done, section };
			SIZE_T attributeBytes = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
			std::vector<std::byte> attributeStorage(attributeBytes);
			auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
			InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes);
			UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
			STARTUPINFOEXW startup{};
			startup.StartupInfo.cb = sizeof(startup);
			startup.lpAttributeList = attributes;
			std::wstring command = std::format(L"\"{}\" {} {} {} {} \"{}\"", exe.wstring(),
				reinterpret_cast<std::uintptr_t>(process), reinterpret_cast<std::uintptr_t>(request), reinterpret_cast<std::uintptr_t>(done),
				reinterpret_cast<std::uintptr_t>(section), dump.wstring());
			PROCESS_INFORMATION info{};
			const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
			DeleteProcThreadAttributeList(attributes);
			if (!started) {
				logger::warn("[CRASH] {} could not be started (error {}): crashes and hangs write no dump", exe.string(), GetLastError());
				return;
			}
			CloseHandle(info.hThread);
			CloseHandle(info.hProcess);
			helper = { request, done, shared };
			logger::info("[CRASH] crash dumps are written by {} (pid {}) to {}", exe.filename().string(), info.dwProcessId, dump.string());

			// CS_TEST_CRASH=<seconds> (a development switch): a worker thread faults that long after load, to exercise the
			// handler and the helper end to end.
			if (const char* test = std::getenv("CS_TEST_CRASH"); test && *test) {
				const int seconds = std::max(1, std::atoi(test));
				logger::warn("[CRASH] CS_TEST_CRASH: a deliberate access violation in {} s", seconds);
				std::thread([seconds] {
					std::this_thread::sleep_for(std::chrono::seconds(seconds));
					*static_cast<volatile int*>(nullptr) = 0;
				}).detach();
			}
		}

		void CopyPath(wchar_t (&a_to)[1024], const std::filesystem::path& a_from)
		{
			wcsncpy_s(a_to, a_from.c_str(), _TRUNCATE);
		}
	}

	void Start()
	{
		StartHelper();
		AddVectoredExceptionHandler(1, &OnFirstChance);
		// A filter installed later (a crash logger) runs instead of this one unless it chains to it, as this one chains.
		previousFilter = SetUnhandledExceptionFilter(&OnUnhandled);
	}

	bool Available() { return helper.shared != nullptr; }

	HangDumpResult RequestHangDump(std::uint32_t a_threadId, const std::filesystem::path& a_dump, const std::filesystem::path& a_text, std::uint32_t a_timeoutMs,
		std::uint32_t& a_error)
	{
		a_error = 0;
		if (!helper.shared)
			return HangDumpResult::NoHelper;
		std::lock_guard lock(hangMutex);
		hangBusy.store(true, std::memory_order_release);
		helper.shared->threadId = a_threadId;
		helper.shared->kind = RequestKind::kHang;
		helper.shared->exceptionPointers = 0;
		CopyPath(helper.shared->dumpPath, a_dump);
		CopyPath(helper.shared->textPath, a_text);
		helper.shared->written = 0;
		helper.shared->error = 0;
		ResetEvent(helper.done);
		SetEvent(helper.request);
		const bool answered = WaitForSingleObject(helper.done, a_timeoutMs) == WAIT_OBJECT_0;
		hangBusy.store(false, std::memory_order_release);
		if (!answered)
			return HangDumpResult::NoAnswer;
		if (!helper.shared->written) {
			a_error = helper.shared->error;
			return HangDumpResult::Failed;
		}
		return HangDumpResult::Written;
	}
}
