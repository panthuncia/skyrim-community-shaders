#pragma once

// The in-game test runner (docs/development/testing.md). It reads CommunityShaders-Test.json, tracks the load milestones, runs the
// configured steps and writes CommunityShaders-Test.status.json. It does not depend on any feature. Features add commands, waits
// and load holds through the functions below.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Testing
{
	/**
	 * @brief SKSEPlugin_Load, before anything reads a switch: reads the test configuration and applies its switches.
	 * Also applies the old CommunityShaders.env file, which is kept for one migration step.
	 */
	void LoadConfig();

	/** @brief SKSEPlugin_Load, after Util::LogTee::Install: the log watcher for the expectations and the last error line. */
	void InstallLogWatcher();

	/** @brief SKSE's message handler: the lifecycle's SKSE milestones (kDataLoaded, kPreLoadGame, kNewGame, kPostLoadGame). */
	void OnSkseMessage(std::uint32_t a_type);

	/** @brief Render thread, after every Present (beside State::Reset): milestones, steps and the status file. */
	void OnPresent();

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

	/** @brief A wait condition by name (a step's wait). Render thread at Present. True once the condition holds. */
	void RegisterWait(std::string a_name, std::function<bool()> a_holds);

	/** @brief Something to run when the exit step ends the process, before the logs are flushed (a feature's final report). */
	void RegisterExitReport(std::function<void()> a_report);

	/**
	 * @brief Render thread: runs one old-style command line. "dclf:<name>=<arg>" runs <name> (or "dclf:<name>") with <arg>, and any
	 * other line runs as a console command. Returns false with a_error set when no command takes it.
	 */
	bool RunCommandLine(const std::string& a_line, std::string& a_error);

	/** @brief Any thread: a command line to run at the next Present (DevBench's communityshaders.dclf). */
	void QueueCommandLine(std::string a_line);

	/** @brief The old command list's progress (CS_DCLF_TEST_COMMANDS), as of the last Present. */
	struct LegacyProgress
	{
		std::uint32_t frame = 0;
		std::uint32_t shift = 0;      // frames the list's waits held so far
		std::uint32_t next = 0;       // the next command's index
		std::uint32_t commands = 0;   // the list's length
		std::uint32_t nextFrame = 0;  // the frame the next command runs at, as of now
		std::string nextCommand;
		std::string waiting;  // the condition the list holds on; empty when none
	};

	/** @brief Any thread: the old command list's progress. */
	LegacyProgress GetLegacyProgress();

	/** @brief Render thread: frames counted as world frames (no loading menu, no hold). The old command list counts these. */
	bool WorldFrame();

	/**
	 * @brief Any thread: the GPU device was lost. Writes the outcome device-lost with the fault, then ends the process two
	 * seconds later in a test run. a_where names the path that saw it.
	 */
	void ReportDeviceLost(std::string_view a_where, std::uint64_t a_faultAddress, std::string_view a_faultKind, std::string_view a_markers);

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
