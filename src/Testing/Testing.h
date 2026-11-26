#pragma once

// The in-game test runner (docs/development/testing.md). It reads CommunityShaders-Test.json, tracks the load milestones, runs the
// configured steps and writes CommunityShaders-Test.status.json. It does not depend on any feature. Features add commands, waits
// and load holds through the functions below.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Testing
{
	/**
	 * @brief SKSEPlugin_Load, before anything reads a switch: reads the test configuration and applies its switches.
	 * Also applies CommunityShaders.env beside the log, where manual runs set switches.
	 */
	void LoadConfig();

	/** @brief SKSEPlugin_Load, after Util::LogTee::Install: the log watcher for the expectations and the last error line. */
	void InstallLogWatcher();

	/** @brief SKSE's message handler: the lifecycle's SKSE milestones (kDataLoaded, kPreLoadGame, kNewGame, kPostLoadGame). */
	void OnSkseMessage(std::uint32_t a_type);

	/**
	 * @brief Render thread, after every Present (beside State::Reset): milestones, the live-image shots, the exit and the status file.
	 * A Present outside a game update (the main menu, a loading screen's thread) is a game frame of its own and runs the steps.
	 */
	void OnPresent();

	/**
	 * @brief Game thread, once per game update, when its snapshot is published (Main::Update's call of Main::DrawFrame): the step
	 * clock moves one game frame. The steps and the commands spread over frames run at the update's first Present, inside its
	 * render half, as they always did. Their game work (a console command, a coc among them) is queued and runs on the main thread
	 * after the update's frame jobs, where the engine is on one thread and a coc shows its loading screen.
	 * Steps count game frames, so a cadence run takes its steps at the same game time as a normal one; with dropped renders a step
	 * runs at the first rendered update at or after its frame.
	 */
	void OnGameUpdate();

	/** @brief Whether a test configuration was read: this run is a test run. */
	bool Configured();

	/** @brief The run folder the configuration names; empty outside a test run. */
	std::filesystem::path RunDirectory();

	namespace Holds
	{
		/**
		 * @brief Any thread: a load hold starts (a_active) or ends. While any hold is active the world is not ready.
		 * DCLF's load hold reports here as "dclf:load-hold".
		 */
		void Set(std::string_view a_name, bool a_active);

		/** @brief Any thread: whether any hold is active. */
		bool Any();

		/** @brief Any thread: the active holds' names. */
		std::vector<std::string> List();
	}

	/**
	 * @brief A command's body. It runs on the render thread at Present. a_arg is the step's arg. It returns false with a_error set
	 * when it could not run, which fails the step.
	 */
	using Command = std::function<bool(const std::string& a_arg, std::string& a_error)>;

	/** @brief Any time before the steps run: a command by name. A later registration of the same name replaces it. */
	void RegisterCommand(std::string a_name, Command a_command);

	/**
	 * @brief Every command whose name starts with a_prefix that has no exact registration ("dclf:" for DCLF's). The command gets the
	 * whole name in a_name. It returns false with a_error set when it does not know the name.
	 */
	void RegisterCommandPrefix(std::string a_prefix, std::function<bool(const std::string& a_name, const std::string& a_arg, std::string& a_error)> a_command);

	/** @brief Whether a command of this name is registered, by name or by prefix. */
	bool HasCommand(std::string_view a_name);

	/**
	 * @brief Any time before the steps run: a feature's named test command, which a "call" step runs as
	 * "<owner>.<name>[=<arg>]". The render graph's commands are owner "org". A later registration of the same name replaces it.
	 */
	void RegisterFeatureCommand(std::string a_owner, std::string a_name, Command a_command);

	/** @brief Render thread: runs the feature command a_owner.a_name with a_arg. False with a_error set when none is registered. */
	bool RunFeatureCommand(std::string_view a_owner, std::string_view a_name, const std::string& a_arg, std::string& a_error);

	/**
	 * @brief Any thread: a_value under a_key in the status file's "report" object. The harness reads it after the run
	 * ("graph_frames" for expect.graph_frames). Latest wins. Does nothing outside a test run.
	 */
	void SetReport(std::string a_key, nlohmann::json a_value);

	/** @brief A wait condition by name (a step's wait). Render thread at Present. True once the condition holds. */
	void RegisterWait(std::string a_name, std::function<bool()> a_holds);

	/** @brief Something to run when the exit step ends the process, before the logs are flushed (a feature's final report). */
	void RegisterExitReport(std::function<void()> a_report);

	/**
	 * @brief Render thread: runs one command line. "<command>[=<arg>]" runs a registered command by its cs-test step name
	 * ("shot=a", "dclf:cull=off", "org:rebuild"), and any other line runs as a console command. Returns false with a_error set
	 * when the command fails.
	 */
	bool RunCommandLine(const std::string& a_line, std::string& a_error);

	/** @brief Any thread: a command line to run at the next Present (DevBench's communityshaders.dclf). */
	void QueueCommandLine(std::string a_line);

	/**
	 * @brief Any thread: the GPU device was lost. Writes the outcome device-lost with the fault, then ends the process two
	 * seconds later in a test run. a_where names the path that saw it.
	 */
	void ReportDeviceLost(std::string_view a_where, std::uint64_t a_faultAddress, std::string_view a_faultKind, std::string_view a_markers);

	/** @brief Any thread: BasicRHI reported a lost device (a fault, or a timeline that read UINT64_MAX), in any run. */
	bool DeviceLossSeen();

	/** @brief The unhandled exception filter: writes the outcome crashed. Best effort; it does not allocate much. */
	void ReportCrash(std::uint32_t a_code, const void* a_address);

	/** @brief The hang watchdog's thread, after its report: writes the outcome hang and ends the process in a test run. */
	void ReportHang(double a_seconds);

	/** @brief Any thread: the shader compiler's progress, for the status file and the watchdog. */
	struct CompileProgress
	{
		std::uint64_t done = 0;
		std::uint64_t total = 0;
		bool compiling = false;
	};
	CompileProgress GetCompileProgress();
}
