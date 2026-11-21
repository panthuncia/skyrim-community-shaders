#pragma once

#include "RenderGraph/Frame/Declare.h"
#include "Utils/Setting.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

/**
* @brief Observed state with no owner yet, as a frame variable (cleanup plan B2, an interim).
*
* A feature that reads something outside CS each frame (the effects an engine manager made, a chain's order) writes it here
* where it sees it. A change raises one variable event, so the next revision carries it. A declaration depends on Var() and its
* key reads Get(a_in). So the key is a function of its Inputs, and no Invalidate is needed.
*
* The value can be any copyable type with operator==. The variable holds a serial, and this object keeps each value under its
* serial until the adopted revision is past it. So a revision being prepared reads the value it was raised with.
*
* Render thread only. The object must stay at one address: the serial is a bound setting. Names must be unique in the session.
*/
namespace RenderGraph::Frame
{
	template <class T>
	class Observed
	{
	public:
		/** @brief Binds the variable "Setting.Observed.<a_name>". */
		explicit Observed(std::string_view a_name)
		{
			std::string name("Observed.");
			name += a_name;
			serial.Bind(CS::SettingRegistry::Declare(name));
		}

		Observed(const Observed&) = delete;
		Observed& operator=(const Observed&) = delete;
		Observed(Observed&&) = delete;
		Observed& operator=(Observed&&) = delete;

		/** @brief Writes what the caller saw. An equal value does nothing. A new one is an event. Returns whether it changed. */
		bool Observe(const T& a_value)
		{
			if (!values.empty() && values.rbegin()->second == a_value)
				return false;
			const std::uint64_t next = serial.Get() + 1;
			values.emplace(next, a_value);
			Prune();
			serial.Set(next);
			return true;
		}

		/** @brief What a declaration depends on: DependsOn(observed.Var(), ...). */
		const CS::Setting<std::uint64_t>& Var() const { return serial; }

		/** @brief The value a revision carries, for a key or a description. Null before the first Observe. */
		const T* Get(const Inputs& a_in) const { return Find(a_in.Get(serial)); }

		/** @brief The value of the revision adopted last. Null before the first adoption that carried one. */
		const T* Adopted() const { return Find(serial.Adopted()); }

		/** @brief The newest value written. Null before the first Observe. Not for keys. */
		const T* Latest() const { return values.empty() ? nullptr : &values.rbegin()->second; }

	private:
		const T* Find(std::uint64_t a_serial) const
		{
			const auto found = values.find(a_serial);
			return found == values.end() ? nullptr : &found->second;
		}

		/** @brief Revisions carry the adopted serial or a newer one, so older values are never read again. */
		void Prune()
		{
			const std::uint64_t adopted = serial.Adopted();
			while (values.size() > 1 && values.begin()->first < adopted)
				values.erase(values.begin());
		}

		CS::Setting<std::uint64_t> serial;
		std::map<std::uint64_t, T> values;
	};
}
