#include "RenderGraph/Switches.h"

#include <cctype>
#include <charconv>
#include <map>
#include <mutex>

namespace Switches
{
	namespace
	{
		std::string ReadEnvironment(const std::string& a_name)
		{
			char value[512]{};
			const DWORD length = GetEnvironmentVariableA(a_name.c_str(), value, sizeof(value));
			return length && length < sizeof(value) ? std::string(value, length) : std::string();
		}
	}

	std::string Value(std::string_view a_name)
	{
		static std::mutex s_mutex;
		static std::map<std::string, std::string, std::less<>> s_values;
		std::scoped_lock lock(s_mutex);
		if (const auto found = s_values.find(a_name); found != s_values.end())
			return found->second;
		std::string name(a_name);
		auto value = ReadEnvironment(name);
		s_values.emplace(std::move(name), value);
		return value;
	}

	bool IsSet(std::string_view a_name)
	{
		return !Value(a_name).empty();
	}

	bool Equals(std::string_view a_name, std::string_view a_value)
	{
		return Value(a_name) == a_value;
	}

	bool EqualsNoCase(std::string_view a_name, std::string_view a_value)
	{
		const auto value = Value(a_name);
		if (value.size() != a_value.size())
			return false;
		for (std::size_t i = 0; i < value.size(); ++i)
			if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(a_value[i])))
				return false;
		return true;
	}

	std::uint64_t Number(std::string_view a_name, std::uint64_t a_default)
	{
		const auto value = Value(a_name);
		std::uint64_t number = 0;
		const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
		return error == std::errc() && end != value.data() ? number : a_default;
	}
}
