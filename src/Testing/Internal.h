#pragma once

// The test runner's shared state, for its own files only. Everything here is guarded by Run().mutex unless it says otherwise.

#include "Testing/Testing.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Testing
{
	/** @brief One step of the configuration. */
	struct Step
	{
		std::string at;  // a milestone, or "previous"
		std::uint32_t frames = 0;
		std::string command;  // "do"
		std::string arg;
		std::string wait;
		double waitSeconds = 0.0;  // 0: timeouts.step_wait
	};

	/** @brief The resolved configuration (CommunityShaders-Test.json). */
	struct Config
	{
		std::string name, runId;
		std::filesystem::path runDir;
		std::vector<std::string> featuresDisabled;
		std::string save, cell;
		double launchTimeout = 60, compileTimeout = 600, loadTimeout = 120, stepWait = 60;
		std::vector<Step> steps;
		std::string expectReach;
		std::vector<std::string> expectShots;
		std::vector<std::string> expectNoLog;
	};

	/** @brief A milestone's time (seconds since the plugin loaded) and frame. */
	struct Milestone
	{
		std::string name;
		double t = 0.0;
		std::uint32_t frame = 0;
	};

	/** @brief The milestones in the order they are reached. */
	inline constexpr const char* kMilestones[] = { "plugin", "data-loaded", "shaders-ready", "main-menu", "load-start", "loading-screen", "world-ready" };

	/** @brief Whether a_name is a milestone. */
	bool IsMilestone(std::string_view a_name);

	struct RunState
	{
		std::mutex mutex;
		bool configured = false;
		Config config;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

		// Config echo.
		std::size_t applied = 0;
		std::vector<std::string> unknown;    // names no registry knows: config-error
		std::vector<std::string> unchecked;  // DCLF feature switches that could not be checked (DCLF did not install)

		// Lifecycle.
		std::vector<Milestone> occurrences;  // every milestone reached, repeats included
		std::string phase = "boot";
		std::uint32_t frame = 0;
		double fps = 0.0;

		// Steps.
		std::size_t stepIndex = 0;
		std::string stepState;
		std::vector<std::string> shots;

		std::string lastError;
		std::string outcome, reason;
		std::string faultAddress, faultMarkers;
	};

	/** @brief The run's state. */
	RunState& Run();

	/** @brief Seconds since the plugin loaded. */
	double Now();

	/** @brief Records a milestone now at a_frame. Caller holds the mutex. Writes the status file after unlocking is the caller's job. */
	void RecordMilestone(std::string_view a_name, std::uint32_t a_frame);

	/** @brief The first occurrence of a milestone, if reached. Caller holds the mutex. */
	const Milestone* FindMilestone(std::string_view a_name);

	/** @brief Writes the status file now (temporary file, then rename). Takes the mutex. Does nothing outside a test run. */
	void WriteStatus();

	/** @brief The status file writer's thread: about once a second, and the phase timeouts. */
	void StartStatusThread();

	/**
	 * @brief A final outcome. The first one wins. It is logged, written to the status file and the logs are flushed. Then, when
	 * a_exitAfterMs is set, the process ends that much later (from another thread when a_exitAfterMs is above zero).
	 */
	void Finish(std::string_view a_outcome, std::string a_reason, std::optional<std::uint32_t> a_exitAfterMs);

	/** @brief Sets a non-final outcome field early (config-error found at load). The run goes on so the harness sees the status. */
	void NoteConfigError(std::string a_reason);

	/** @brief The no_log substrings the log watcher found so far, each with its first line. Takes its own lock. */
	std::vector<std::pair<std::string, std::string>> NoLogHits();

	/** @brief Lifecycle: per Present. Render thread. */
	void UpdateLifecycle(std::uint32_t a_frame);

	/** @brief Lifecycle: the phase timeouts, from the status thread and from Present. */
	void CheckPhaseTimeouts();

	/** @brief Steps: per Present. Render thread. */
	void RunSteps(std::uint32_t a_frame);

	/** @brief The old CS_DCLF_TEST_* switches' list, per Present. Render thread. */
	void RunLegacy(std::uint32_t a_frame);

	/** @brief Commands: runs a registered command. Render thread. */
	bool RunCommand(const std::string& a_name, const std::string& a_arg, std::string& a_error);

	/** @brief Commands: the generic commands (Commands.cpp), registered at load. */
	void RegisterGenericCommands();

	/** @brief Commands: the work some commands spread over frames (move, turn, cast). Render thread, every Present. */
	void TickCommands();

	/** @brief Turns the player's heading by a_radians, on the main thread. */
	void TurnPlayer(float a_radians);

	/** @brief Carries the player a_units forward along its heading, on the main thread. */
	void MovePlayer(float a_units);

	/** @brief Waits: whether a_condition holds. a_known is false for a name no one registered. Render thread. */
	bool WaitHolds(const std::string& a_condition, bool& a_known);

	/** @brief The exit command's work: checks the expectations, runs the exit reports, writes the outcome and ends the process. */
	void ExitRun();

	/** @brief Validates and applies the environment, features and DCLF ini of a parsed configuration (Config.cpp). */
	void ApplySwitches(const nlohmann::json& a_config);

	/** @brief Re-checks the names that could not be checked at load, once DCLF's feature switches are registered. */
	void RecheckSwitches();
}
