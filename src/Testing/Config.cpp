// The test configuration: CommunityShaders-Test.json read at load, its switches checked and applied.
#include "Testing/Internal.h"
#include "Testing/SwitchRegistry.h"

#include "Feature.h"
#include "Utils/SwitchFile.h"

#include <fstream>

namespace Testing
{
	namespace
	{
		/** @brief Sets a variable in both copies of the environment: Win32's and the C runtime's (std::getenv reads its own). */
		void SetEnvironment(const std::string& a_name, const std::string& a_value)
		{
			SetEnvironmentVariableA(a_name.c_str(), a_value.c_str());
			_putenv_s(a_name.c_str(), a_value.c_str());
		}

		bool InEnvironment(const std::string& a_name) { return GetEnvironmentVariableA(a_name.c_str(), nullptr, 0) != 0; }

		/**
		 * @brief The old CommunityShaders.env beside the log, one NAME=value per line, '#' for comments. A launcher such as an
		 * already-running MO2 does not pass a new environment to the game. Kept for one migration step: cs-ab still writes it.
		 */
		void ApplyEnvFile()
		{
			const auto path = Util::SwitchFile::Directory() / "CommunityShaders.env";
			std::ifstream file(path);
			if (!file)
				return;
			std::string line;
			while (std::getline(file, line)) {
				const auto first = line.find_first_not_of(" \t");
				if (first == std::string::npos || line[first] == '#')
					continue;
				const auto equals = line.find('=', first);
				if (equals == std::string::npos)
					continue;
				std::string name = line.substr(first, equals - first);
				std::string value = line.substr(equals + 1);
				while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
					name.pop_back();
				while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
					value.pop_back();
				if (name.empty())
					continue;
				if (InEnvironment(name)) {
					logger::info("[DevEnv] {} is already set by the environment; ignoring the file's value", name);
					continue;
				}
				SetEnvironment(name, value);
				logger::info("[DevEnv] {}={} (from {})", name, value, path.filename().string());
			}
		}

		/** @brief A JSON value as a switch value: strings as they are, booleans as 1 or 0, numbers as written. */
		std::string SwitchText(const json& a_value)
		{
			if (a_value.is_string())
				return a_value.get<std::string>();
			if (a_value.is_boolean())
				return a_value.get<bool>() ? "1" : "0";
			if (a_value.is_number_integer())
				return std::to_string(a_value.get<std::int64_t>());
			if (a_value.is_number())
				return fmt::format("{}", a_value.get<double>());
			return a_value.dump();
		}

		std::vector<std::string> Strings(const json& a_value, std::string_view a_key, std::vector<std::string>& a_errors)
		{
			std::vector<std::string> items;
			if (a_value.is_null())
				return items;
			if (!a_value.is_array()) {
				a_errors.push_back(fmt::format("'{}' is not a list", a_key));
				return items;
			}
			for (const auto& item : a_value) {
				if (item.is_string())
					items.push_back(item.get<std::string>());
				else
					a_errors.push_back(fmt::format("'{}' holds a value that is not a string", a_key));
			}
			return items;
		}

		double Seconds(const json& a_object, const char* a_key, double a_default)
		{
			const auto found = a_object.find(a_key);
			return found != a_object.end() && found->is_number() ? found->get<double>() : a_default;
		}

		/** @brief The configuration's non-switch parts: game, timeouts, steps, expect. a_errors collects what is malformed. */
		void ParseRun(const json& a_document, Config& a_config, std::vector<std::string>& a_errors)
		{
			a_config.name = a_document.value("name", std::string{});
			a_config.runId = a_document.value("run_id", std::string{});
			a_config.runDir = std::filesystem::path(a_document.value("run_dir", std::string{}));
			if (const auto game = a_document.find("game"); game != a_document.end() && game->is_object()) {
				a_config.save = game->value("save", std::string{});
				a_config.cell = game->value("cell", std::string{});
			}
			if (const auto timeouts = a_document.find("timeouts"); timeouts != a_document.end() && timeouts->is_object()) {
				a_config.launchTimeout = Seconds(*timeouts, "launch", a_config.launchTimeout);
				a_config.compileTimeout = Seconds(*timeouts, "compile", a_config.compileTimeout);
				a_config.loadTimeout = Seconds(*timeouts, "load", a_config.loadTimeout);
				a_config.stepWait = Seconds(*timeouts, "step_wait", a_config.stepWait);
			}
			if (const auto steps = a_document.find("steps"); steps != a_document.end()) {
				if (!steps->is_array())
					a_errors.push_back("'steps' is not a list");
				else
					for (const auto& item : *steps) {
						if (!item.is_object()) {
							a_errors.push_back("a step is not a table");
							continue;
						}
						Step step;
						step.at = item.value("at", std::string("previous"));
						step.frames = item.value("frames", 0u);
						step.command = item.value("do", std::string{});
						step.arg = item.contains("arg") ? SwitchText(item["arg"]) : std::string{};
						step.wait = item.value("wait", std::string{});
						step.waitSeconds = Seconds(item, "wait_s", 0.0);
						if (step.at != "previous" && !IsMilestone(step.at))
							a_errors.push_back(fmt::format("step {}: 'at' = '{}' is no milestone", a_config.steps.size(), step.at));
						if (step.command.empty() && step.wait.empty())
							a_errors.push_back(fmt::format("step {}: neither 'do' nor 'wait'", a_config.steps.size()));
						a_config.steps.push_back(std::move(step));
					}
			}
			if (const auto expect = a_document.find("expect"); expect != a_document.end() && expect->is_object()) {
				a_config.expectReach = expect->value("reach", std::string{});
				if (!a_config.expectReach.empty() && !IsMilestone(a_config.expectReach))
					a_errors.push_back(fmt::format("expect.reach = '{}' is no milestone", a_config.expectReach));
				a_config.expectShots = Strings(expect->value("shots", json()), "expect.shots", a_errors);
				a_config.expectNoLog = Strings(expect->value("no_log", json()), "expect.no_log", a_errors);
			}
		}

		std::string Join(const std::vector<std::string>& a_items)
		{
			std::string text;
			for (const auto& item : a_items)
				text += (text.empty() ? "" : ", ") + item;
			return text;
		}

		// The names that could not be checked at load (DCLF's feature switches), with what they were checked as.
		std::vector<std::string> uncheckedAtLoad;
	}

	void ApplySwitches(const json& a_document)
	{
		auto& run = Run();
		std::vector<std::string> errors;

		// env: the real environment wins.
		std::size_t applied = 0;
		if (const auto env = a_document.find("env"); env != a_document.end()) {
			if (!env->is_object())
				errors.push_back("'env' is not a table");
			else
				for (const auto& [name, value] : env->items()) {
					switch (SwitchRegistry::Check(name)) {
					case SwitchRegistry::Known::No:
						run.unknown.push_back(name);
						logger::error("[TEST] config: unknown switch {}; not applied", name);
						continue;
					case SwitchRegistry::Known::Unchecked:
						uncheckedAtLoad.push_back(name);
						break;
					default:
						break;
					}
					const auto text = SwitchText(value);
					if (InEnvironment(name)) {
						logger::info("[TEST] config: {} is already set by the environment; the config's '{}' is ignored", name, text);
						continue;
					}
					SetEnvironment(name, text);
					logger::info("[TEST] config: {}={}", name, text);
					++applied;
				}
		}

		// dclf_ini: served as though CommunityShaders-DCLF.ini set it.
		if (const auto ini = a_document.find("dclf_ini"); ini != a_document.end()) {
			if (!ini->is_object())
				errors.push_back("'dclf_ini' is not a table");
			else
				for (const auto& [name, value] : ini->items()) {
					const auto known = SwitchRegistry::Check(name);
					if (known == SwitchRegistry::Known::No) {
						run.unknown.push_back(name);
						logger::error("[TEST] config: unknown switch {} in dclf_ini; not applied", name);
						continue;
					}
					if (known == SwitchRegistry::Known::Unchecked)
						uncheckedAtLoad.push_back(name);
					Util::SwitchFile::SetFileValue(name, SwitchText(value));
					logger::info("[TEST] config: {}={} (dclf_ini)", name, SwitchText(value));
					++applied;
				}
		}

		// features_disabled: joins CS_DISABLE_FEATURES, which State reads at its first feature check.
		const auto features = Strings(a_document.value("features_disabled", json()), "features_disabled", errors);
		std::vector<std::string> featuresOff;
		for (const auto& feature : features) {
			const auto& list = Feature::GetFeatureList();
			const bool known = std::ranges::any_of(list, [&](Feature* a_feature) { return a_feature && a_feature->GetShortName() == feature; });
			if (!known) {
				run.unknown.push_back("features." + feature);
				logger::error("[TEST] config: no feature has the short name {}", feature);
				continue;
			}
			featuresOff.push_back(feature);
		}
		if (!featuresOff.empty()) {
			char existing[4096]{};
			const DWORD length = GetEnvironmentVariableA("CS_DISABLE_FEATURES", existing, sizeof(existing));
			std::string list = length && length < sizeof(existing) ? std::string(existing, length) : std::string{};
			for (const auto& feature : featuresOff)
				list += (list.empty() ? "" : ",") + feature;
			SetEnvironment("CS_DISABLE_FEATURES", list);
		}

		run.applied = applied;
		run.config.featuresDisabled = featuresOff;
		run.unchecked = uncheckedAtLoad;
		for (const auto& error : errors)
			logger::error("[TEST] config: {}", error);
		logger::info("[TEST] config '{}' applied: {} switches, {} features off{}{}", run.config.name, applied, featuresOff.size(),
			run.unknown.empty() ? "" : fmt::format(", {} unknown ({})", run.unknown.size(), Join(run.unknown)),
			uncheckedAtLoad.empty() ? "" : fmt::format(", {} checked once DCLF installs ({})", uncheckedAtLoad.size(), Join(uncheckedAtLoad)));
		if (!run.unknown.empty())
			errors.push_back(fmt::format("unknown names: {}", Join(run.unknown)));
		if (!errors.empty())
			NoteConfigError(Join(errors));
	}

	void RecheckSwitches()
	{
		auto& run = Run();
		std::vector<std::string> unknown, unchecked;
		for (const auto& name : uncheckedAtLoad) {
			const auto known = SwitchRegistry::Check(name);
			if (known == SwitchRegistry::Known::No)
				unknown.push_back(name);
			else if (known == SwitchRegistry::Known::Unchecked)
				unchecked.push_back(name);
		}
		{
			std::scoped_lock lock(run.mutex);
			run.unchecked = unchecked;
			for (const auto& name : unknown)
				run.unknown.push_back(name);
		}
		if (!unchecked.empty())
			logger::warn("[TEST] config: DCLF did not install, so these were not checked: {}", Join(unchecked));
		if (!unknown.empty()) {
			logger::error("[TEST] config: unknown switches: {}", Join(unknown));
			NoteConfigError(fmt::format("unknown names: {}", Join(unknown)));
		}
		uncheckedAtLoad.clear();
	}

	void LoadConfig()
	{
		auto& run = Run();
		run.start = std::chrono::steady_clock::now();
		ApplyEnvFile();
		const auto path = Util::SwitchFile::Directory() / "CommunityShaders-Test.json";
		std::ifstream file(path);
		if (!file)
			return;
		{
			std::scoped_lock lock(run.mutex);
			run.configured = true;
			RecordMilestone("plugin", 0);
		}
		json document;
		try {
			document = json::parse(file);
		} catch (const std::exception& e) {
			logger::error("[TEST] {} is not valid JSON: {}", path.filename().string(), e.what());
			NoteConfigError(fmt::format("malformed configuration: {}", e.what()));
			StartStatusThread();
			return;
		}
		if (!document.is_object() || document.value("version", 0) != 1) {
			logger::error("[TEST] {}: not a version 1 configuration", path.filename().string());
			NoteConfigError("malformed configuration: not version 1");
			StartStatusThread();
			return;
		}
		std::vector<std::string> errors;
		{
			std::scoped_lock lock(run.mutex);
			ParseRun(document, run.config, errors);
		}
		logger::info("[TEST] test run '{}' (run {}), {} steps, results in {}", run.config.name, run.config.runId, run.config.steps.size(), run.config.runDir.string());
		ApplySwitches(document);
		for (const auto& error : errors)
			logger::error("[TEST] config: {}", error);
		if (!errors.empty())
			NoteConfigError(fmt::format("malformed configuration: {}", Join(errors)));
		if (!run.config.runDir.empty()) {
			std::error_code error;
			std::filesystem::create_directories(run.config.runDir / "shots", error);
		}
		StartStatusThread();
	}
}
