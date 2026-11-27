// The step runner and the exit. Steps count game frames (OnGameUpdate); the exit and the live-image shots count Presents.
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"
#include "Testing/Internal.h"

#include "Features/ScreenshotFeature.h"
#include "Globals.h"
#include "State.h"
#include "Utils/SwitchFile.h"
#include "Vram/MemProbe.h"
#include "Vram/SpillLab.h"

#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Revision.h"

namespace Testing
{
	namespace
	{
		// The clocks. The step clock counts game updates, and Presents outside a game update (loading screens, movies: their
		// frame-only snapshots stand for no game update). Presents count rendered frames. Atomic: a loading screen's thread presents beside the main thread.
		std::atomic<std::uint32_t> stepClock{ 0 };
		std::atomic<std::uint32_t> presentFrame{ 0 };
		// A game update published since the last Present outside one: a loading screen's Presents beside the main thread's updates
		// count no game frame of their own.
		std::atomic<bool> updatedSincePresent{ false };
		// A game update moved the step clock and its first Present has not run the steps yet.
		std::atomic<bool> stepsDue{ false };
		// The steps run on the main thread's game updates and on a loading screen's Presents: one at a time.
		std::mutex stepsMutex;

		// The step runner's state, under stepsMutex.
		std::uint32_t lastRunFrame = 0;    // the step clock when the previous step ran
		std::uint32_t lastRunPresent = 0;  // the Present count when the previous step ran
		double waitStart = -1.0;         // when the current step's wait started; below zero: none
		double anchorWaitStart = -1.0;   // when the current step began to wait for its milestone in a ready world; below zero: none
		double shotWaitStart = -1.0;     // when the current step began to wait for the previous shot's file; below zero: none

		// An "adopted" await. The previous step's writes, and what the main thread does with them in the frame after, are taken
		// by the Begins of the next two frames. So the variables' serial is read two Presents after that step ran.
		constexpr std::uint32_t kAwaitReadFrames = 2;
		bool awaitRead = false;          // the serial below was read for the current step
		std::uint64_t awaitSerial = 0;   // the variables' change serial the adopted revision must carry
		bool awaitHeld = false;          // the current step's await held (its wait may still hold it)

		std::mutex queuedMutex;
		std::vector<std::string> queuedLines;

		std::mutex exitReportsMutex;
		std::vector<std::function<void()>> exitReports;

		void SetStepState(std::string a_state)
		{
			auto& run = Run();
			std::scoped_lock lock(run.mutex);
			run.stepState = std::move(a_state);
		}

		/** @brief The frame a step's anchor was reached at, if it was: "previous" is the previous step's run. */
		std::optional<std::uint32_t> Anchor(const Step& a_step)
		{
			if (a_step.at == "previous")
				return lastRunFrame;
			auto& run = Run();
			std::scoped_lock lock(run.mutex);
			// The first time the milestone is reached at or after the previous step: a step after a load (a save, a coc) waits for that
			// load's.
			for (const auto& occurrence : run.occurrences)
				if (occurrence.name == a_step.at && occurrence.frame >= lastRunFrame)
					return occurrence.frame;
			return std::nullopt;
		}
	}

	namespace
	{
		// The game setup before the steps (game.save, game.cell). Render thread only.
		bool saveHandled = false;
		bool cellHandled = false;

		// Frames after the main menu before a named save is loaded: the menu is up and taking messages by then.
		constexpr std::uint32_t kMenuSettleFrames = 60;

		/** @brief The save's file: <Documents>\My Games\Skyrim Special Edition\<sLocalSavePath>\<name>.ess. */
		std::filesystem::path SaveFile(const std::string& a_name)
		{
			std::string folder = "Saves\\";
			if (auto* setting = CS_ENGINE_READ(Ini, Other, RE::GetINISetting("sLocalSavePath:General")); setting && setting->GetType() == RE::Setting::Type::kString && setting->data.s && *setting->data.s)
				folder = setting->data.s;
			auto file = Util::SwitchFile::Directory().parent_path() / folder / a_name;
			if (file.extension() != ".ess")
				file += ".ess";
			return file;
		}

		/**
		 * @brief game.save and game.cell, before the steps. A named save is loaded from the main menu; a cell is a coc once the save's
		 * world is ready. Each counts as a step boundary, so a step at world-ready waits for the world after it. False until done.
		 */
		bool RunSetup(std::uint32_t a_frame)
		{
			auto& run = Run();
			std::string save, cell;
			std::optional<std::uint32_t> menuFrame;
			bool shadersReady = false;
			{
				std::scoped_lock lock(run.mutex);
				save = run.config.save;
				cell = run.config.cell;
				if (const auto* menu = FindMilestone("main-menu"))
					menuFrame = menu->frame;
				shadersReady = FindMilestone("shaders-ready") != nullptr;
			}
			if (!saveHandled) {
				if (save.empty() || save == "autoload") {
					saveHandled = true;
				} else {
					if (!menuFrame || !shadersReady || a_frame < *menuFrame + kMenuSettleFrames) {
						SetStepState(fmt::format("waiting to load save '{}'", save));
						return false;
					}
					std::error_code error;
					const auto file = SaveFile(save);
					auto* manager = RE::BGSSaveLoadManager::GetSingleton();
					if (!manager || !std::filesystem::exists(file, error)) {
						Finish("failed", fmt::format("save '{}' not found ({})", save, file.string()), 0u);
						return false;
					}
					logger::info("[TEST] loading save '{}' at frame {}", save, a_frame);
					manager->Load(file.stem().string().c_str());
					saveHandled = true;
					lastRunFrame = a_frame;
				}
			}
			if (!cellHandled) {
				if (cell.empty()) {
					cellHandled = true;
				} else {
					const Step worldReady{ .at = "world-ready" };
					if (!Anchor(worldReady)) {
						SetStepState(fmt::format("waiting for world-ready to coc {}", cell));
						return false;
					}
					logger::info("[TEST] coc {} at frame {}", cell, a_frame);
					std::string error;
					if (!RunCommand("console", "coc " + cell, error)) {
						Finish("failed", fmt::format("coc {}: {}", cell, error), 0u);
						return false;
					}
					cellHandled = true;
					lastRunFrame = a_frame;
					// The steps start from the next frame: the coc's world-ready comes after it.
					return false;
				}
			}
			return true;
		}
	}

	void RunSteps(std::uint32_t a_frame)
	{
		auto& run = Run();
		if (!Configured() || !RunSetup(a_frame))
			return;
		for (;;) {
			Step step;
			std::size_t index = 0;
			double stepWait = 0.0;
			{
				std::scoped_lock lock(run.mutex);
				if (!run.configured || !run.outcome.empty() || run.stepIndex >= run.config.steps.size())
					return;
				index = run.stepIndex;
				step = run.config.steps[index];
				stepWait = run.config.stepWait;
			}
			const auto anchor = Anchor(step);
			if (!anchor) {
				// The phase timeouts time the wait through boot, compiling, a load or a menu. In a ready world nothing would: a step
				// whose load never starts fails here.
				bool world = false;
				{
					std::scoped_lock lock(run.mutex);
					world = run.phase == "world";
				}
				if (!world) {
					anchorWaitStart = -1.0;
				} else {
					const double limit = step.waitSeconds > 0.0 ? step.waitSeconds : stepWait;
					if (anchorWaitStart < 0.0)
						anchorWaitStart = Now();
					if (Now() - anchorWaitStart > limit) {
						Finish("failed", fmt::format("step {}: waited {:.0f} s for {} after the previous step, and no load started", index, limit, step.at), 0u);
						return;
					}
				}
				SetStepState(fmt::format("waiting for {}", step.at));
				return;
			}
			anchorWaitStart = -1.0;
			if (a_frame < *anchor + step.frames) {
				SetStepState(fmt::format("waiting for frame {}", *anchor + step.frames));
				return;
			}
			// A shot is saved before the next step runs. A run's first shot waits for the revision that switches its copy in,
			// so a coc right after it would otherwise capture the loading screen.
			if (globals::features::screenshotFeature.ScriptedCapturesPending()) {
				const double limit = step.waitSeconds > 0.0 ? step.waitSeconds : stepWait;
				if (shotWaitStart < 0.0)
					shotWaitStart = Now();
				if (Now() - shotWaitStart > limit) {
					Finish("failed", fmt::format("step {}: the previous shot was not saved after {:.0f} s", index, limit), 0u);
					return;
				}
				SetStepState("waiting for the previous shot to be saved");
				return;
			}
			shotWaitStart = -1.0;
			if (!step.awaitCondition.empty() && !awaitHeld) {
				const double limit = step.waitSeconds > 0.0 ? step.waitSeconds : stepWait;
				if (waitStart < 0.0)
					waitStart = Now();
				if (!awaitRead) {
					if (presentFrame.load(std::memory_order_relaxed) < lastRunPresent + kAwaitReadFrames) {
						SetStepState("waiting to read the previous step's variable serial");
						return;
					}
					awaitSerial = RenderGraph::Frame::Revision::Mark().takenVariables;
					awaitRead = true;
				}
				const auto mark = RenderGraph::Frame::Revision::Mark();
				// Without the render graph no revision adopts anything: the write is in effect already.
				if (mark.running && mark.adoptedVariables < awaitSerial) {
					if (Now() - waitStart > limit) {
						Finish("failed", fmt::format("step {}: await 'adopted' timed out after {:.0f} s (variables {} not adopted; the last adopted revision {} carried {})", index, limit,
											 awaitSerial, mark.revision, mark.adoptedVariables),
							0u);
						return;
					}
					SetStepState(fmt::format("waiting for a revision that carries variables {} (adopted: {})", awaitSerial, mark.adoptedVariables));
					return;
				}
				logger::info("[TEST] step {}: await 'adopted' held at frame {}: variables {} adopted by revision {}{}", index, a_frame, awaitSerial, mark.revision,
					mark.running ? "" : " (no frame revisions in this run)");
				awaitHeld = true;
			}
			if (!step.wait.empty()) {
				if (waitStart < 0.0)
					waitStart = Now();
				bool known = true;
				const bool holds = WaitHolds(step.wait, known);
				if (!known) {
					Finish("failed", fmt::format("step {}: no wait condition '{}'", index, step.wait), 0u);
					return;
				}
				const double limit = step.waitSeconds > 0.0 ? step.waitSeconds : stepWait;
				const double waited = Now() - waitStart;
				if (!holds) {
					if (waited > limit) {
						Finish("failed", fmt::format("step {}: wait '{}' timed out after {:.0f} s", index, step.wait, limit), 0u);
						return;
					}
					SetStepState(fmt::format("waiting for '{}'", step.wait));
					return;
				}
				logger::info("[TEST] step {}: wait '{}' held after {:.1f} s", index, step.wait, waited);
			}
			if (!step.command.empty()) {
				SetStepState("running");
				logger::info("[TEST] step {} at frame {}: {}{}{}", index, a_frame, step.command, step.arg.empty() ? "" : " ", step.arg);
				std::string error;
				if (!RunCommand(step.command, step.arg, error)) {
					Finish("failed", fmt::format("step {} '{}': {}", index, step.command, error), 0u);
					return;
				}
			}
			lastRunFrame = a_frame;
			lastRunPresent = presentFrame.load(std::memory_order_relaxed);
			waitStart = -1.0;
			awaitRead = false;
			awaitHeld = false;
			{
				std::scoped_lock lock(run.mutex);
				++run.stepIndex;
				run.stepState.clear();
			}
		}
	}

	void RegisterExitReport(std::function<void()> a_report)
	{
		std::scoped_lock lock(exitReportsMutex);
		exitReports.push_back(std::move(a_report));
	}

	void ExitRun()
	{
		auto& run = Run();
		std::vector<std::string> failures;
		bool configured = false;
		{
			std::scoped_lock lock(run.mutex);
			configured = run.configured;
			const auto& config = run.config;
			if (!config.expectReach.empty() && !FindMilestone(config.expectReach))
				failures.push_back(fmt::format("did not reach {}", config.expectReach));
			for (const auto& shot : config.expectShots) {
				std::error_code error;
				if (!std::filesystem::exists(config.runDir / "shots" / (shot + ".png"), error))
					failures.push_back(fmt::format("shot '{}' was not written", shot));
			}
		}
		for (const auto& [substring, line] : NoLogHits())
			failures.push_back(fmt::format("logged '{}': {}", substring, line));
		logger::info("[TEST] exit at frame {}", run.frame);
		{
			std::scoped_lock lock(exitReportsMutex);
			for (const auto& report : exitReports)
				report();
		}
		RenderGraph::Engine::Tripwire::Report("exit");
		std::string reason;
		for (const auto& failure : failures)
			reason += (reason.empty() ? "" : "; ") + failure;
		if (!configured)
			reason = "an exit outside a cs-test run";
		Finish(failures.empty() ? "passed" : "failed", std::move(reason), 0u);
	}

	bool RunCommandLine(const std::string& a_line, std::string& a_error)
	{
		// "<command>[=<arg>]" runs a registered command by its cs-test name. Any other line is a console command.
		const auto equals = a_line.find('=');
		const std::string name = a_line.substr(0, equals);
		if (!HasCommand(name))
			return RunCommand("console", a_line, a_error);
		const std::string arg = equals == std::string::npos ? std::string{} : a_line.substr(equals + 1);
		return RunCommand(name, arg, a_error);
	}

	void QueueCommandLine(std::string a_line)
	{
		std::scoped_lock lock(queuedMutex);
		queuedLines.push_back(std::move(a_line));
	}

	namespace
	{
		std::optional<std::chrono::steady_clock::time_point> exitRequested;  // render thread: the exit command's time

		// The live-image shots (expect.live_frames). Render thread only.
		enum class LiveProbe : std::uint8_t
		{
			None,   // not started
			First,  // _live-a asked for at liveFirst
			Done,   // both asked for, or skipped
		};
		LiveProbe liveProbe = LiveProbe::None;
		std::uint32_t liveFirst = 0;

		/**
		 * @brief expect.live_frames: before the exit, two shots of the final image that many frames apart. A frozen final target gives
		 * two identical files, which the harness fails. Skipped outside the world phase (a run that exits at a menu or at boot).
		 * Returns true once the exit may go on.
		 */
		bool LiveProbeDone(std::uint32_t a_frame)
		{
			if (liveProbe == LiveProbe::Done)
				return true;
			auto& run = Run();
			std::uint32_t frames = 0;
			std::filesystem::path directory;
			bool world = false;
			{
				std::scoped_lock lock(run.mutex);
				frames = run.config.expectLiveFrames;
				directory = run.config.runDir;
				world = run.phase == "world";
			}
			if (!frames || directory.empty()) {
				liveProbe = LiveProbe::Done;
				return true;
			}
			directory /= "shots";
			if (liveProbe == LiveProbe::None) {
				if (!world) {
					logger::info("[TEST] live image: skipped, the exit is not in the world phase");
					liveProbe = LiveProbe::Done;
					return true;
				}
				std::error_code error;
				std::filesystem::create_directories(directory, error);
				globals::features::screenshotFeature.RequestCapture(directory / "_live-a.png");
				liveFirst = a_frame;
				liveProbe = LiveProbe::First;
				return false;
			}
			if (a_frame - liveFirst < frames)
				return false;
			globals::features::screenshotFeature.RequestCapture(directory / "_live-b.png");
			logger::info("[TEST] live image: shots at frames {} and {}", liveFirst, a_frame);
			liveProbe = LiveProbe::Done;
			// The exit's wait for the shots starts now.
			exitRequested = std::chrono::steady_clock::now();
			return true;
		}
	}

	void RequestExit()
	{
		if (!exitRequested)
			exitRequested = std::chrono::steady_clock::now();
	}

	void OnGameUpdate()
	{
		stepClock.fetch_add(1, std::memory_order_relaxed);
		updatedSincePresent.store(true, std::memory_order_relaxed);
		stepsDue.store(true, std::memory_order_relaxed);
	}

	void OnPresent()
	{
		const auto present = presentFrame.fetch_add(1, std::memory_order_relaxed) + 1;
		// A Present outside a game update is a game frame of its own when no update ran since the last one: a loading screen or a movie
		// publishes only frame-only snapshots. Inside an update, its first Present runs the steps.
		const bool inUpdate = Snapshot::InGameUpdate();
		const bool ownFrame = !inUpdate && !updatedSincePresent.exchange(false, std::memory_order_relaxed);
		const bool runSteps = ownFrame || (inUpdate && stepsDue.exchange(false, std::memory_order_relaxed));
		const auto frame = ownFrame ? stepClock.fetch_add(1, std::memory_order_relaxed) + 1 : stepClock.load(std::memory_order_relaxed);
		// The memory probe's CSV, once: the log directory is a known-folder query, which every Present would pay.
		static const bool memProbeStarted = (MemProbe::Start(logger::log_directory().value_or(std::filesystem::path{})), true);
		(void)memProbeStarted;
		Vram::SpillLab::OnPresent();

		UpdateLifecycle(frame, present);
		CheckPhaseTimeouts();
		{
			std::vector<std::string> lines;
			{
				std::scoped_lock lock(queuedMutex);
				lines.swap(queuedLines);
			}
			for (const auto& line : lines) {
				logger::info("[TEST] queued command: {}", line);
				std::string error;
				if (!RunCommandLine(line, error))
					logger::warn("[TEST] queued command '{}': {}", line, error);
			}
		}
		// A loading screen's thread never waits for the main thread's steps: it skips them while those run.
		if (std::unique_lock lock(stepsMutex, std::try_to_lock); runSteps && lock) {
			RunSteps(frame);
			TickCommands();
		}
		// The exit waits for the shots still being read back or encoded: a process ended mid-write leaves a broken PNG.
		if (exitRequested && LiveProbeDone(present)) {
			const bool pending = globals::features::screenshotFeature.ScriptedCapturesPending();
			const bool late = std::chrono::steady_clock::now() - *exitRequested > std::chrono::seconds(10);
			if (!pending || late) {
				if (pending)
					logger::warn("[TEST] exit: a shot was still being saved after 10 s");
				ExitRun();
			}
		}
	}
}
