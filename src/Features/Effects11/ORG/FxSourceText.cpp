#include "FxSourceText.h"

#include <cctype>
#include <optional>
#include <sstream>

namespace OrgFx::SourceText
{
	namespace
	{
		bool IsIdentChar(char a_c)
		{
			return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_';
		}

		void Trim(std::string& a_s, const char* a_chars = " \t")
		{
			a_s.erase(0, a_s.find_first_not_of(a_chars));
			a_s.erase(a_s.find_last_not_of(a_chars) + 1);
		}

		std::optional<std::string> ParseStringifyDefine(const std::string& a_line)
		{
			std::string trimmed = a_line;
			if (!trimmed.empty() && trimmed.back() == '\r')
				trimmed.pop_back();

			auto hash = trimmed.find_first_not_of(" \t");
			if (hash == std::string::npos || trimmed[hash] != '#')
				return std::nullopt;

			auto keyword = trimmed.find_first_not_of(" \t", hash + 1);
			if (keyword == std::string::npos || trimmed.compare(keyword, 6, "define") != 0)
				return std::nullopt;

			auto nameStart = trimmed.find_first_not_of(" \t", keyword + 6);
			if (nameStart == std::string::npos)
				return std::nullopt;

			auto nameEnd = trimmed.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", nameStart);
			if (nameEnd == std::string::npos || nameEnd == nameStart)
				return std::nullopt;

			auto openParen = trimmed.find('(', nameEnd);
			if (openParen == std::string::npos || trimmed.find_first_not_of(" \t", nameEnd) != openParen)
				return std::nullopt;

			auto closeParen = trimmed.find(')', openParen);
			if (closeParen == std::string::npos)
				return std::nullopt;

			std::string param = trimmed.substr(openParen + 1, closeParen - openParen - 1);
			Trim(param);

			auto bodyStart = trimmed.find_first_not_of(" \t", closeParen + 1);
			if (bodyStart == std::string::npos || trimmed[bodyStart] != '#')
				return std::nullopt;

			std::string body = trimmed.substr(bodyStart + 1);
			Trim(body, " \t\r");

			if (body != param)
				return std::nullopt;

			return trimmed.substr(nameStart, nameEnd - nameStart);
		}

		std::string ReplaceStringifyInvocations(const std::string& a_source, const std::string& a_macroName)
		{
			std::string result;
			result.reserve(a_source.size());
			size_t pos = 0;

			while (pos < a_source.size()) {
				size_t found = a_source.find(a_macroName, pos);
				if (found == std::string::npos) {
					result.append(a_source, pos);
					break;
				}

				bool partOfLargerIdent =
					(found > 0 && IsIdentChar(a_source[found - 1])) ||
					(found + a_macroName.size() < a_source.size() && IsIdentChar(a_source[found + a_macroName.size()]));

				if (partOfLargerIdent) {
					result.append(a_source, pos, found + a_macroName.size() - pos);
					pos = found + a_macroName.size();
					continue;
				}

				size_t afterName = found + a_macroName.size();
				while (afterName < a_source.size() && (a_source[afterName] == ' ' || a_source[afterName] == '\t'))
					++afterName;

				if (afterName >= a_source.size() || a_source[afterName] != '(') {
					result.append(a_source, pos, found + a_macroName.size() - pos);
					pos = found + a_macroName.size();
					continue;
				}

				int depth = 1;
				size_t argEnd = afterName + 1;
				while (argEnd < a_source.size() && depth > 0) {
					if (a_source[argEnd] == '(')
						++depth;
					else if (a_source[argEnd] == ')')
						--depth;
					++argEnd;
				}

				if (depth != 0) {
					result.append(a_source, pos, found + a_macroName.size() - pos);
					pos = found + a_macroName.size();
					continue;
				}

				std::string arg = a_source.substr(afterName + 1, argEnd - afterName - 2);
				Trim(arg);

				// D3DPreprocess puts spaces between tokens. Collapse them to what ENB's #x writes:
				//   "| -" to "|-" (UI name prefix)
				//   " . " to "." (group path separator)
				std::string collapsed;
				collapsed.reserve(arg.size());
				for (size_t ci = 0; ci < arg.size(); ++ci) {
					if (ci + 2 < arg.size() && arg[ci] == '|' && arg[ci + 1] == ' ' && arg[ci + 2] == '-') {
						collapsed += "|-";
						ci += 2;
					} else if (arg[ci] == ' ' && ci + 1 < arg.size() && arg[ci + 1] == '.') {
						continue;
					} else if (arg[ci] == '.' && ci + 1 < arg.size() && arg[ci + 1] == ' ') {
						collapsed += '.';
						++ci;
					} else {
						collapsed += arg[ci];
					}
				}

				result.append(a_source, pos, found - pos);
				result += "\"" + collapsed + "\"";
				pos = argEnd;
			}

			return result;
		}
	}

	void StripStringifyDefines(std::string& a_source, std::vector<std::string>& a_macroNames)
	{
		std::string stripped;
		stripped.reserve(a_source.size());

		std::istringstream stream(a_source);
		std::string line;
		while (std::getline(stream, line)) {
			if (auto name = ParseStringifyDefine(line)) {
				a_macroNames.push_back(*name);
				stripped += "\n";
			} else {
				stripped += line + "\n";
			}
		}

		a_source = std::move(stripped);
	}

	void ExpandStringifyMacros(std::string& a_source, const std::vector<std::string>& a_macroNames)
	{
		for (auto& name : a_macroNames)
			a_source = ReplaceStringifyInvocations(a_source, name);
	}

	void StripLineDirectives(std::string& a_source)
	{
		std::string result;
		result.reserve(a_source.size());
		std::istringstream stream(a_source);
		std::string line;
		while (std::getline(stream, line)) {
			size_t firstNonSpace = line.find_first_not_of(" \t");
			if (firstNonSpace != std::string::npos && line[firstNonSpace] == '#') {
				auto directive = line.substr(firstNonSpace + 1);
				size_t dirStart = directive.find_first_not_of(" \t");
				if (dirStart != std::string::npos && directive.compare(dirStart, 4, "line") == 0) {
					result += "\n";
					continue;
				}
			}
			result += line;
			result += "\n";
		}
		a_source = std::move(result);
	}

	bool IsEncrypted(std::string_view a_source)
	{
		return a_source.starts_with("KIEFX");
	}
}
