#include "RenderGraph/Switches.h"

#include <cctype>
#include <charconv>
#include <map>
#include <mutex>
#include <ranges>

namespace Switches
{
	namespace
	{
		/** @brief The variable's whole value. A list switch can be long, so nothing is cut: the buffer grows to the value's size. */
		std::string ReadEnvironment(const std::string& a_name)
		{
			std::string value(256, '\0');
			for (int attempt = 0; attempt < 4; ++attempt) {
				const DWORD length = GetEnvironmentVariableA(a_name.c_str(), value.data(), static_cast<DWORD>(value.size()));
				if (length == 0)
					return {};
				if (length < value.size()) {
					value.resize(length);
					return value;
				}
				// Too small: length is the size it needs, with the terminator. The value may change between the calls.
				value.assign(length, '\0');
			}
			logger::warn("[Switches] {} kept changing size while it was read; it is treated as unset", a_name);
			return {};
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

	std::vector<std::string> List(std::string_view a_name)
	{
		std::vector<std::string> items;
		const auto value = Value(a_name);
		for (const auto part : std::views::split(value, ',')) {
			std::string_view item(part.begin(), part.end());
			while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
				item.remove_prefix(1);
			while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
				item.remove_suffix(1);
			if (!item.empty())
				items.emplace_back(item);
		}
		return items;
	}
}
