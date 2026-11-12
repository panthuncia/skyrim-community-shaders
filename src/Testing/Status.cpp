// The run's state, the status file, the final outcomes and the log watcher.
#include "Testing/Internal.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "Utils/LogTee.h"
#include "Utils/SwitchFile.h"

#include <spdlog/sinks/base_sink.h>

#if defined(CS_HAS_RENDER_GRAPH)
#	include <rhi.h>
#endif

#include <fstream>
#include <set>
#include <thread>

namespace Testing
{
	namespace
	{
		std::atomic<bool> finished{ false };
		std::atomic<bool> crashing{ false };
		std::atomic<bool> dirty{ false };

		/**
		 * @brief Watches every log line: the last error line, and the expectations' no_log substrings. It has its own lock and never
		 * takes the run's, so logging while the run's lock is held is safe.
		 */
		class WatchSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			void SetNoLog(std::vector<std::string> a_substrings)
			{
				std::scoped_lock lock(dataMutex);
				noLog = std::move(a_substrings);
			}

			std::string LastError()
			{
				std::unique_lock lock(dataMutex, std::defer_lock);
				if (!Lock(lock))
					return {};
				return lastError;
			}

			std::vector<std::pair<std::string, std::string>> Hits()
			{
				std::unique_lock lock(dataMutex, std::defer_lock);
				if (!Lock(lock))
					return {};
				return hits;
			}

		protected:
			void sink_it_(const spdlog::details::log_msg& a_message) override
			{
				const std::string_view text(a_message.payload.data(), a_message.payload.size());
				std::scoped_lock lock(dataMutex);
				if (a_message.level >= spdlog::level::err)
					lastError = fmt::format("[{}] {}", a_message.level == spdlog::level::critical ? 'C' : 'E', text.substr(0, 400));
				for (const auto& substring : noLog)
					if (text.find(substring) != std::string_view::npos && std::ranges::none_of(hits, [&](const auto& a_hit) { return a_hit.first == substring; }))
						hits.emplace_back(substring, std::string(text.substr(0, 400)));
			}

			void flush_() override {}

		private:
			/** @brief In the crash filter the crashing thread may hold the lock: try, never wait. */
			static bool Lock(std::unique_lock<std::mutex>& a_lock)
			{
				if (!crashing.load(std::memory_order_relaxed)) {
					a_lock.lock();
					return true;
				}
				return a_lock.try_lock();
			}

			std::mutex dataMutex;
			std::string lastError;
			std::vector<std::string> noLog;
			std::vector<std::pair<std::string, std::string>> hits;
		};

		std::shared_ptr<WatchSink>& Watcher()
		{
			static std::shared_ptr<WatchSink> sink;
			return sink;
		}

		std::mutex holdsMutex;
		std::set<std::string, std::less<>> holds;

		std::filesystem::path StatusPath() { return Util::SwitchFile::Directory() / "CommunityShaders-Test.status.json"; }

		std::string Timestamp()
		{
			SYSTEMTIME time{};
			GetLocalTime(&time);
			return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
		}

		json MilestoneJson(const Milestone& a_milestone) { return json{ { "t", std::round(a_milestone.t * 1000.0) / 1000.0 }, { "frame", a_milestone.frame } }; }

		/** @brief The status document. Caller holds the run's lock (or, in the crash filter, may not). */
		json BuildStatus(RunState& a_run)
		{
			json milestones = json::object();
			json loads = json::array();
			for (const auto& occurrence : a_run.occurrences) {
				if (!milestones.contains(occurrence.name)) {
					milestones[occurrence.name] = MilestoneJson(occurrence);
					continue;
				}
				// A repeat: load-start opens a new entry in loads.
				if (loads.empty() || occurrence.name == "load-start" || loads.back().contains(occurrence.name))
					loads.push_back(json::object());
				loads.back()[occurrence.name] = MilestoneJson(occurrence);
			}
			const auto compile = GetCompileProgress();
			json step = nullptr;
			if (a_run.stepIndex < a_run.config.steps.size())
				step = json{ { "index", a_run.stepIndex }, { "do", a_run.config.steps[a_run.stepIndex].command }, { "state", a_run.stepState } };
			auto& watcher = Watcher();
			const auto lastError = watcher ? watcher->LastError() : std::string{};
			return json{
				{ "version", 1 },
				{ "run_id", a_run.config.runId },
				{ "pid", GetCurrentProcessId() },
				{ "updated", Timestamp() },
				{ "phase", a_run.phase },
				{ "frame", a_run.frame },
				{ "fps", std::round(a_run.fps * 10.0) / 10.0 },
				{ "milestones", std::move(milestones) },
				{ "loads", std::move(loads) },
				{ "config", { { "applied", a_run.applied }, { "features_off", a_run.config.featuresDisabled }, { "unknown", a_run.unknown }, { "unchecked", a_run.unchecked } } },
				{ "step", std::move(step) },
				{ "compile", { { "done", compile.done }, { "total", compile.total } } },
				{ "holds", Holds::List() },
				{ "last_error", lastError },
				{ "outcome", a_run.outcome },
				{ "reason", a_run.reason },
				{ "fault", { { "address", a_run.faultAddress }, { "markers", a_run.faultMarkers } } },
				{ "shots", a_run.shots },
			};
		}

		void WriteDocument(const json& a_document)
		{
			const auto path = StatusPath();
			const auto temporary = std::filesystem::path(path).concat(".tmp");
			{
				std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
				if (!file)
					return;
				file << a_document.dump(1, '\t');
			}
			// The harness may have the file open for a moment: a few tries.
			for (int attempt = 0; attempt < 10; ++attempt) {
				if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					return;
				Sleep(10);
			}
		}

		void FlushLogs()
		{
			spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& a_logger) { a_logger->flush(); });
		}

		[[noreturn]] void EndProcess(std::string_view a_outcome)
		{
			FlushLogs();
			::TerminateProcess(::GetCurrentProcess(), a_outcome == "passed" ? 0u : 1u);
			for (;;)
				Sleep(1000);
		}
	}

	RunState& Run()
	{
		static RunState state;
		return state;
	}

	double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - Run().start).count(); }

	bool Configured()
	{
		auto& run = Run();
		std::scoped_lock lock(run.mutex);
		return run.configured;
	}

	std::filesystem::path RunDirectory()
	{
		auto& run = Run();
		std::scoped_lock lock(run.mutex);
		return run.config.runDir;
	}

	bool IsMilestone(std::string_view a_name) { return std::ranges::find(kMilestones, a_name) != std::end(kMilestones); }

	void RecordMilestone(std::string_view a_name, std::uint32_t a_frame)
	{
		auto& run = Run();
		const bool first = FindMilestone(a_name) == nullptr;
		run.occurrences.push_back({ std::string(a_name), Now(), a_frame });
		dirty.store(true, std::memory_order_release);
		logger::info("[TEST] milestone {}{} at {:.1f} s, frame {}", a_name, first ? "" : " (again)", run.occurrences.back().t, a_frame);
	}

	const Milestone* FindMilestone(std::string_view a_name)
	{
		for (const auto& occurrence : Run().occurrences)
			if (occurrence.name == a_name)
				return &occurrence;
		return nullptr;
	}

	void WriteStatus()
	{
		auto& run = Run();
		json document;
		if (crashing.load(std::memory_order_relaxed)) {
			// The crashing thread may hold the lock: try for a moment, then read without it.
			std::unique_lock lock(run.mutex, std::defer_lock);
			for (int attempt = 0; attempt < 20 && !lock.try_lock(); ++attempt)
				Sleep(5);
			if (!run.configured)
				return;
			document = BuildStatus(run);
		} else {
			std::scoped_lock lock(run.mutex);
			if (!run.configured)
				return;
			document = BuildStatus(run);
		}
		dirty.store(false, std::memory_order_relaxed);
		WriteDocument(document);
	}

	void StartStatusThread()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			if (auto& watcher = Watcher()) {
				auto& run = Run();
				std::scoped_lock lock(run.mutex);
				watcher->SetNoLog(run.config.expectNoLog);
			}
			WriteStatus();
			std::thread([] {
				auto lastWrite = std::chrono::steady_clock::now();
				for (;;) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					if (finished.load(std::memory_order_acquire))
						continue;
					CheckPhaseTimeouts();
					const auto now = std::chrono::steady_clock::now();
					if (dirty.load(std::memory_order_acquire) || now - lastWrite >= std::chrono::seconds(1)) {
						WriteStatus();
						lastWrite = now;
					}
				}
			}).detach();
		});
	}

	void Finish(std::string_view a_outcome, std::string a_reason, std::optional<std::uint32_t> a_exitAfterMs)
	{
		if (finished.exchange(true, std::memory_order_acq_rel))
			return;
		auto& run = Run();
		bool configured = false;
		{
			std::scoped_lock lock(run.mutex);
			configured = run.configured;
			run.outcome = std::string(a_outcome);
			run.reason = a_reason;
			run.phase = "exiting";
		}
		logger::info("[TEST] outcome {}{}{}", a_outcome, a_reason.empty() ? "" : ": ", a_reason);
		if (configured)
			WriteStatus();
		FlushLogs();
		if (!a_exitAfterMs)
			return;
		if (*a_exitAfterMs == 0)
			EndProcess(a_outcome);
		std::thread([outcome = std::string(a_outcome), ms = *a_exitAfterMs] {
			Sleep(ms);
			logger::info("[TEST] ending the process ({})", outcome);
			EndProcess(outcome);
		}).detach();
	}

	void NoteConfigError(std::string a_reason)
	{
		// A config error is final: the run would test something else than asked. The game ends at once.
		Finish("config-error", std::move(a_reason), 0u);
	}

	std::vector<std::pair<std::string, std::string>> NoLogHits()
	{
		auto& watcher = Watcher();
		return watcher ? watcher->Hits() : std::vector<std::pair<std::string, std::string>>{};
	}

	void InstallLogWatcher()
	{
		auto& watcher = Watcher();
		if (watcher)
			return;
#if defined(CS_HAS_RENDER_GRAPH)
		// BasicRHI's lost devices (Vulkan's device fault and crash markers, D3D12's DRED report) end a test run.
		rhi::SetDeviceLossCallback([](const rhi::DeviceLossInfo& a_info) {
			ReportDeviceLost(fmt::format("{}: {}", a_info.backend, a_info.where), a_info.faultAddress, a_info.faultKind ? a_info.faultKind : "",
				a_info.markers ? a_info.markers : "");
		});
#endif
		watcher = std::make_shared<WatchSink>();
		{
			auto& run = Run();
			std::scoped_lock lock(run.mutex);
			watcher->SetNoLog(run.config.expectNoLog);
		}
		Util::LogTee::AddSink(watcher);
	}

	CompileProgress GetCompileProgress()
	{
		CompileProgress progress;
		if (auto* cache = globals::shaderCache) {
			progress.done = cache->GetCompletedTasks() + cache->GetFailedTasks();
			progress.total = cache->GetTotalTasks();
			progress.compiling = cache->IsCompiling();
		}
		return progress;
	}

	void ReportDeviceLost(std::string_view a_where, std::uint64_t a_faultAddress, std::string_view a_faultKind, std::string_view a_markers)
	{
		auto& run = Run();
		{
			std::scoped_lock lock(run.mutex);
			if (!run.configured || !run.outcome.empty())
				return;
			if (a_faultAddress && run.faultAddress.empty())
				run.faultAddress = fmt::format("0x{:x}", a_faultAddress);
			if (!a_markers.empty() && run.faultMarkers.empty())
				run.faultMarkers = std::string(a_markers.substr(0, 2000));
		}
		std::string reason = fmt::format("the GPU device was lost ({})", a_where);
		if (a_faultAddress)
			reason += fmt::format(", fault {} at 0x{:x}", a_faultKind.empty() ? "address" : a_faultKind, a_faultAddress);
		Finish("device-lost", std::move(reason), 2000u);
	}

	void ReportCrash(std::uint32_t a_code, const void* a_address)
	{
		crashing.store(true, std::memory_order_relaxed);
		auto& run = Run();
		if (!run.configured || finished.exchange(true, std::memory_order_acq_rel))
			return;
		// No lock and no log here: the crashing thread may hold either.
		run.outcome = "crashed";
		run.reason = fmt::format("unhandled exception 0x{:08X} at {}", a_code, a_address);
		run.phase = "exiting";
		WriteStatus();
	}

	void ReportHang(double a_seconds)
	{
		if (!Configured())
			return;
		Finish("hang", fmt::format("the render thread did not present for {:.1f} s", a_seconds), 0u);
	}

	namespace Holds
	{
		void Set(std::string_view a_name, bool a_active)
		{
			bool changed = false;
			{
				std::scoped_lock lock(holdsMutex);
				if (a_active)
					changed = holds.emplace(a_name).second;
				else if (const auto found = holds.find(a_name); found != holds.end()) {
					holds.erase(found);
					changed = true;
				}
			}
			if (changed)
				dirty.store(true, std::memory_order_release);
		}

		bool Any()
		{
			std::scoped_lock lock(holdsMutex);
			return !holds.empty();
		}

		std::vector<std::string> List()
		{
			std::unique_lock lock(holdsMutex, std::defer_lock);
			if (!crashing.load(std::memory_order_relaxed))
				lock.lock();
			else if (!lock.try_lock())
				return {};
			return { holds.begin(), holds.end() };
		}
	}
}
