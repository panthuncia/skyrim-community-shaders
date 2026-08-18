#include "FrameGenWatchdog.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <string>

namespace
{
	uint64_t ClockNs()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool IsForegroundProcess()
	{
		const HWND window = GetForegroundWindow();
		if (!window)
			return false;
		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		return processId == GetCurrentProcessId();
	}

	void RequestGpuRecovery()
	{
		wchar_t systemDir[MAX_PATH]{};
		if (!GetSystemDirectoryW(systemDir, MAX_PATH))
			return;
		std::wstring command = L"\"" + std::wstring(systemDir) +
			L"\\schtasks.exe\" /Run /TN \"CommunityShaders GPU Recovery\"";
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION process{};
		if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
				CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			logger::critical("[DLSSG-Watchdog] requested forced TDR through scheduled recovery task");
		} else {
			logger::critical("[DLSSG-Watchdog] recovery task launch failed (Win32 error {}); run community_shaders_recover_hung_gpu.ps1 manually",
				GetLastError());
		}
	}
}

void FrameGenWatchdog::Start(const std::atomic<bool>& a_enabled,
	const std::atomic<uint64_t>& a_renderHeartbeat, const std::atomic<uint64_t>& a_presentHeartbeat)
{
	if (worker.joinable())
		return;
	worker = std::jthread([this, &a_enabled, &a_renderHeartbeat, &a_presentHeartbeat](std::stop_token a_stop) {
		SetThreadDescription(GetCurrentThread(), L"CS DLSS-G hang watchdog");
		while (!a_stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			if (!a_enabled.load(std::memory_order_acquire)) {
				triggered.store(false, std::memory_order_release);
				continue;
			}
			const uint64_t now = ClockNs();
			const uint64_t render = a_renderHeartbeat.load(std::memory_order_acquire);
			const uint64_t present = a_presentHeartbeat.load(std::memory_order_acquire);
			constexpr uint64_t timeoutNs = 8'000'000'000ull;
			if (!render || !present || now - render < timeoutNs || now - present < timeoutNs || !IsForegroundProcess())
				continue;
			bool expected = false;
			if (triggered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				logger::critical("[DLSSG-Watchdog] render and present stalled for {} ms; forcing WDDM recovery",
					(now - std::max(render, present)) / 1'000'000ull);
				RequestGpuRecovery();
			}
		}
	});
}
