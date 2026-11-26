#pragma once

// Feature settings that raise an event when their value changes.
// No ORG includes here. Feature headers include this file.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace CS
{
	/// Names the frame variable a setting raises. Zero means unbound.
	using SettingVariableId = std::uint32_t;

	/// The largest value a setting's adopted copy holds. A frame variable carries no more (Vars::Lane::SettingValue::kCapacity).
	inline constexpr std::size_t kAdoptedSettingCapacity = 32;

	/**
	 * @brief A bound setting's adopted value: the value of the frame revision adopted last. The registry owns it and never frees it.
	 * A value of 8 bytes or fewer is one atomic word. A larger one is read under the registry's lock (SettingRegistry::ReadAdopted).
	 */
	struct AdoptedSettingCell
	{
		std::atomic<std::uint64_t> word{ 0 };
		std::array<std::byte, kAdoptedSettingCapacity> bytes{};  // under the registry's lock
		std::size_t size = 0;
	};

	/**
	 * @brief Setting ids, the address of each bound setting, and the hook that raises their events.
	 *
	 * Writes come from the render thread and the menu and cs-test threads, so every call takes one lock.
	 * None of it runs on a frame path.
	 */
	class SettingRegistry
	{
	public:
		/// Receives a raise with the value's bytes (null for an unbound id). Wired to the frame variables. Called outside the registry lock.
		using RaiseHook = void (*)(std::uint32_t a_id, std::string_view a_name, const void* a_bytes, std::size_t a_size);

		/** @brief The id for a_name, made on first use. Never zero. The same name always gives the same id. */
		static SettingVariableId Declare(std::string_view a_name);

		/** @brief The declared name of a_id, or empty for an unknown id. The view stays valid for the process. */
		static std::string_view Name(SettingVariableId a_id);

		/**
		 * @brief Installs the hook every raise calls, or none with nullptr.
		 * @return The hook it replaces, so a test can chain to it and restore it.
		 */
		static RaiseHook SetRaiseHook(RaiseHook a_hook);

		/**
		 * @brief Records that a_id's value lives at a_address (a_size bytes). Its current bytes count as raised, and as adopted
		 * until a revision adopts another value. Setting::Bind calls it. Returns the id's adopted cell.
		 */
		static const AdoptedSettingCell* Attach(SettingVariableId a_id, const void* a_address, std::size_t a_size);

		/** @brief Forgets the binding at a_address, if any. A bound setting's destructor calls it. */
		static void Detach(const void* a_address);

		/** @brief Raises a_id. It records the current bytes as raised and calls the hook. Setting::Set calls it on a change. */
		static void Raise(SettingVariableId a_id);

		/**
		 * @brief Raises the setting bound at a_address if its bytes differ from the ones last raised.
		 *
		 * For code that wrote the value through a raw address, such as the scene widget interceptor.
		 * An address with no binding does nothing.
		 */
		static void RaiseAt(const void* a_address);

		/**
		 * @brief Copies the bytes a_id last raised (or had when bound) into a_out.
		 * @return False when a_id is not bound or its size is not a_size.
		 */
		static bool RaisedBytes(SettingVariableId a_id, void* a_out, std::size_t a_size);

		/** @brief The bound ids whose bytes differ from the ones last raised. For the missed-writer check. */
		static std::vector<SettingVariableId> Unraised();

		/**
		 * @brief Inside the adoption point. Stores a_bytes (a_size bytes) as a_id's adopted value and copies the value it replaces into
		 * a_before. False, storing nothing, when a_id is not bound or is bound at another size.
		 */
		static bool PublishAdopted(SettingVariableId a_id, const void* a_bytes, std::size_t a_size, void* a_before);

		/** @brief Any thread. Copies a_cell's adopted value (a_size bytes, more than 8) into a_out. */
		static void ReadAdopted(const AdoptedSettingCell& a_cell, void* a_out, std::size_t a_size);

		/**
		 * @brief Any thread. Whether frame revisions adopt settings: a raise hook is installed. Without them Setting::Adopted reads
		 * the value itself.
		 */
		static bool Adopting() noexcept { return adopting.load(std::memory_order_acquire); }

		/**
		 * @brief Any thread. Counts the raises so far: it moves whenever a bound setting's value changes, whoever wrote it. The frame
		 * snapshot's settingsRevision.
		 */
		static std::uint64_t Serial() noexcept { return serial.load(std::memory_order_acquire); }

	private:
		static inline std::atomic<bool> adopting{ false };
		static inline std::atomic<std::uint64_t> serial{ 0 };
	};

	/** @brief One element of an array setting. A write sets the whole value, so it raises the event. */
	template <class S, class E>
	class SettingElement
	{
	public:
		SettingElement(S& a_setting, std::size_t a_index) noexcept :
			setting(a_setting), index(a_index) {}

		operator const E&() const noexcept { return setting.Get()[index]; }
		const E& Get() const noexcept { return setting.Get()[index]; }

		SettingElement& operator=(const E& a_value)
		{
			auto value = setting.Get();
			value[index] = a_value;
			setting.Set(value);
			return *this;
		}

	private:
		S& setting;
		std::size_t index;
	};

	/**
	 * @brief A feature setting whose writes raise its frame variable's event.
	 *
	 * The value is the first member, so the address of a setting is the address of its value.
	 * A copy is unbound. Assigning keeps the binding and raises when the value changes.
	 */
	template <class T>
	class Setting
	{
		// The registry compares raw bytes to see whether a raw write changed the value.
		static_assert(std::is_trivially_copyable_v<T>, "CS::Setting holds trivially copyable values only");

	public:
		constexpr Setting(T a_initial = {}) :
			value(a_initial)
		{
			if constexpr (std::is_standard_layout_v<T>)
				static_assert(std::is_standard_layout_v<Setting>, "the value must stay at the setting's address");
		}

		/** @brief Copies the value only. The copy is unbound. */
		Setting(const Setting& a_other) :
			value(a_other.value)
		{}

		~Setting()
		{
			if (id != 0)
				SettingRegistry::Detach(&value);
		}

		/** @brief Takes the other's value and keeps this binding. */
		Setting& operator=(const Setting& a_other)
		{
			Set(a_other.value);
			return *this;
		}

		Setting& operator=(const T& a_value)
		{
			Set(a_value);
			return *this;
		}

		operator const T&() const noexcept { return value; }

		const T& Get() const noexcept { return value; }

		/** @brief An element of an array value, read only. A write sets the whole value, so it raises the event. */
		template <class U = T>
		auto operator[](std::size_t a_index) const noexcept -> decltype(std::declval<const U&>()[a_index])
		{
			return value[a_index];
		}

		/** @brief A writable element of an array value. A write sets the whole value, so it raises the event. */
		template <class U = T>
		auto operator[](std::size_t a_index) noexcept
			-> SettingElement<Setting, std::remove_cvref_t<decltype(std::declval<const U&>()[a_index])>>
		{
			return { *this, a_index };
		}

		/** @brief The element count of an array value. */
		template <class U = T>
		constexpr auto size() const noexcept -> decltype(std::declval<const U&>().size())
		{
			return value.size();
		}

		/** @brief Stores a_value. When it differs and the setting is bound, raises the event. */
		void Set(const T& a_value)
		{
			if (value == a_value)
				return;
			value = a_value;
			if (id != 0)
				SettingRegistry::Raise(id);
		}

		/**
		 * @brief Any thread. The value of the frame revision adopted last, published at its adoption. Before the first adoption that
		 * changed it, the value the setting had when frame revisions started. Without frame revisions, or unbound, the value itself.
		 */
		T Adopted() const
		{
			static_assert(sizeof(T) <= kAdoptedSettingCapacity, "a frame variable carries settings of 32 bytes or fewer");
			if (!cell || !SettingRegistry::Adopting())
				return value;
			T out = value;
			if constexpr (sizeof(T) <= sizeof(std::uint64_t)) {
				const std::uint64_t word = cell->word.load(std::memory_order_acquire);
				std::memcpy(&out, &word, sizeof(T));
			} else
				SettingRegistry::ReadAdopted(*cell, &out, sizeof(T));
			return out;
		}

		/** @brief The value's address, for code that must have one. Its writes raise nothing until RaiseAt sees them. */
		T* Raw() noexcept { return &value; }
		const T* Raw() const noexcept { return &value; }

		/** @brief Binds this setting to a_id, or unbinds it with zero. Done once, where the feature registers. */
		void Bind(SettingVariableId a_id)
		{
			if (id != 0)
				SettingRegistry::Detach(&value);
			id = a_id;
			cell = id != 0 ? SettingRegistry::Attach(id, &value, sizeof(T)) : nullptr;
		}

		SettingVariableId Id() const noexcept { return id; }

	private:
		T value;
		SettingVariableId id = 0;
		const AdoptedSettingCell* cell = nullptr;  // the registry's, while bound
	};
}

// The JSON holds the plain value, so saved settings keep their form.
namespace nlohmann
{
	template <class T>
	struct adl_serializer<CS::Setting<T>>
	{
		template <class BasicJson>
		static void to_json(BasicJson& a_json, const CS::Setting<T>& a_setting)
		{
			a_json = a_setting.Get();
		}

		template <class BasicJson>
		static void from_json(const BasicJson& a_json, CS::Setting<T>& a_setting)
		{
			a_setting = a_json.template get<T>();
		}
	};
}
