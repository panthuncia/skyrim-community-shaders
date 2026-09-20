#include "Switches.h"

#include <ShlObj.h>
#include <Windows.h>

#include <fstream>
#include <mutex>

#include <ankerl/unordered_dense.h>

namespace DCLF
{
	namespace
	{
		constexpr const char* kFileName = "CommunityShaders-DCLF.ini";

		std::filesystem::path SwitchesPath()
		{
			PWSTR documents = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)))
				return {};
			std::filesystem::path path(documents);
			CoTaskMemFree(documents);
			return path / L"My Games" / L"Skyrim Special Edition" / L"SKSE" / kFileName;
		}

		// NAME=value per line; blank lines and lines starting with ';' or '#' are ignored.
		ankerl::unordered_dense::map<std::string, std::string> LoadFile()
		{
			ankerl::unordered_dense::map<std::string, std::string> values;
			const auto path = SwitchesPath();
			if (path.empty())
				return values;
			std::ifstream file(path);
			if (!file)
				return values;
			std::string line;
			while (std::getline(file, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
					line.pop_back();
				const auto first = line.find_first_not_of(" \t");
				if (first == std::string::npos || line[first] == ';' || line[first] == '#')
					continue;
				const auto equals = line.find('=', first);
				if (equals == std::string::npos)
					continue;
				auto name = line.substr(first, equals - first);
				while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
					name.pop_back();
				values[name] = line.substr(equals + 1);
			}
			return values;
		}

		const ankerl::unordered_dense::map<std::string, std::string>& FileValues()
		{
			static const auto values = LoadFile();
			return values;
		}

		ankerl::unordered_dense::map<std::string, std::string>& Resolved()
		{
			static ankerl::unordered_dense::map<std::string, std::string> resolved;
			return resolved;
		}

		std::mutex& ResolvedMutex()
		{
			static std::mutex mutex;
			return mutex;
		}
	}

	std::string SwitchValue(const char* a_name)
	{
		const std::lock_guard lock(ResolvedMutex());
		auto& resolved = Resolved();
		if (const auto it = resolved.find(a_name); it != resolved.end())
			return it->second;
		std::string value;
		char buffer[1024] = {};
		if (const auto length = GetEnvironmentVariableA(a_name, buffer, sizeof(buffer)); length && length < sizeof(buffer)) {
			value.assign(buffer, length);
		} else if (const auto it = FileValues().find(a_name); it != FileValues().end()) {
			value = it->second;
		}
		resolved.emplace(a_name, value);
		return value;
	}

	bool SwitchEnabled(const char* a_name)
	{
		return SwitchValue(a_name) == "1";
	}

	std::string SwitchSummary()
	{
		std::string text;
		// The file's own entries, plus whatever the environment adds on top of them.
		auto add = [&](const std::string& a_name) {
			const auto value = SwitchValue(a_name.c_str());
			if (value.empty())
				return;
			if (!text.empty())
				text += ", ";
			text += a_name + "=" + value;
		};
		for (const auto& [name, value] : FileValues())
			add(name);
		for (const char* name : { "CS_DCLF", "CS_DCLF_HYBRID", "CS_DCLF_CULL", "CS_DCLF_STATS", "CS_DCLF_DEBUG_VIEW",
				 "CS_DCLF_BUILD_PARITY", "CS_DCLF_CAPTURE_PARITY", "CS_DCLF_HYBRID_NOSKIP", "CS_DCLF_ONLY_ELIGIBLE" }) {
			if (!FileValues().contains(name))
				add(name);
		}
		return text.empty() ? std::string("none set") : text;
	}
}
