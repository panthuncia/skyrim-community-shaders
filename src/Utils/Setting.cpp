#include "Utils/Setting.h"

#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace CS
{
	namespace
	{
		struct SettingBinding
		{
			const void* address = nullptr;
			std::size_t size = 0;
			std::vector<std::byte> raised;  // the bytes last raised
		};

		struct SettingTable
		{
			std::mutex mutex;
			std::deque<std::string> names;  // id n at n - 1. A deque, so names never move.
			std::unordered_map<std::string_view, SettingVariableId> idsByName;
			std::unordered_map<SettingVariableId, SettingBinding> bindings;
			std::unordered_map<const void*, SettingVariableId> idsByAddress;
			// One cell per id, kept for the process: a setting holds a pointer to it.
			std::unordered_map<SettingVariableId, std::unique_ptr<AdoptedSettingCell>> cells;
			SettingRegistry::RaiseHook hook = nullptr;
		};

		SettingTable& Settings()
		{
			static SettingTable s_table;
			return s_table;
		}

		bool SettingBytesRaised(const SettingBinding& a_binding)
		{
			return std::memcmp(a_binding.address, a_binding.raised.data(), a_binding.size) == 0;
		}

		void RecordSettingBytes(SettingBinding& a_binding)
		{
			std::memcpy(a_binding.raised.data(), a_binding.address, a_binding.size);
		}

		/** @brief Stores a_size bytes as a_cell's adopted value. Under the table's lock. */
		void StoreAdopted(AdoptedSettingCell& a_cell, const void* a_bytes, std::size_t a_size)
		{
			a_cell.size = a_size;
			if (a_size <= sizeof(std::uint64_t)) {
				std::uint64_t word = 0;
				std::memcpy(&word, a_bytes, a_size);
				a_cell.word.store(word, std::memory_order_release);
			} else
				std::memcpy(a_cell.bytes.data(), a_bytes, a_size);
		}

		/** @brief Copies a_cell's adopted value into a_out. Under the table's lock. */
		void LoadAdopted(const AdoptedSettingCell& a_cell, void* a_out)
		{
			if (a_cell.size <= sizeof(std::uint64_t)) {
				const std::uint64_t word = a_cell.word.load(std::memory_order_acquire);
				std::memcpy(a_out, &word, a_cell.size);
			} else
				std::memcpy(a_out, a_cell.bytes.data(), a_cell.size);
		}

		void CallSettingHook(SettingRegistry::RaiseHook a_hook, SettingVariableId a_id, std::string_view a_name, const std::vector<std::byte>& a_bytes)
		{
			if (a_hook)
				a_hook(a_id, a_name, a_bytes.empty() ? nullptr : a_bytes.data(), a_bytes.size());
		}
	}

	SettingVariableId SettingRegistry::Declare(std::string_view a_name)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		if (const auto found = table.idsByName.find(a_name); found != table.idsByName.end())
			return found->second;
		const auto& name = table.names.emplace_back(a_name);
		const auto id = static_cast<SettingVariableId>(table.names.size());
		table.idsByName.emplace(name, id);
		return id;
	}

	std::string_view SettingRegistry::Name(SettingVariableId a_id)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		if (a_id == 0 || a_id > table.names.size())
			return {};
		return table.names[a_id - 1];
	}

	SettingRegistry::RaiseHook SettingRegistry::SetRaiseHook(RaiseHook a_hook)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		const auto previous = table.hook;
		table.hook = a_hook;
		// When frame revisions start, every bound setting's adopted value is the one it raised last.
		if (a_hook && !previous)
			for (const auto& [id, binding] : table.bindings)
				if (const auto cell = table.cells.find(id); cell != table.cells.end() && binding.size <= kAdoptedSettingCapacity)
					StoreAdopted(*cell->second, binding.raised.data(), binding.size);
		adopting.store(a_hook != nullptr, std::memory_order_release);
		return previous;
	}

	const AdoptedSettingCell* SettingRegistry::Attach(SettingVariableId a_id, const void* a_address, std::size_t a_size)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		// One address per id. Binding an id again moves it.
		if (const auto old = table.bindings.find(a_id); old != table.bindings.end())
			table.idsByAddress.erase(old->second.address);
		if (const auto other = table.idsByAddress.find(a_address); other != table.idsByAddress.end())
			table.bindings.erase(other->second);
		auto& binding = table.bindings[a_id];
		binding.address = a_address;
		binding.size = a_size;
		binding.raised.resize(a_size);
		RecordSettingBytes(binding);
		table.idsByAddress[a_address] = a_id;
		auto& cell = table.cells[a_id];
		if (!cell)
			cell = std::make_unique<AdoptedSettingCell>();
		// Until a revision adopts another value, the bound value is the adopted one.
		if (a_size <= kAdoptedSettingCapacity)
			StoreAdopted(*cell, binding.raised.data(), a_size);
		return cell.get();
	}

	void SettingRegistry::Detach(const void* a_address)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		const auto found = table.idsByAddress.find(a_address);
		if (found == table.idsByAddress.end())
			return;
		table.bindings.erase(found->second);
		table.idsByAddress.erase(found);
	}

	void SettingRegistry::Raise(SettingVariableId a_id)
	{
		auto& table = Settings();
		RaiseHook hook = nullptr;
		std::string_view name;
		std::vector<std::byte> bytes;
		{
			std::scoped_lock lock(table.mutex);
			if (const auto found = table.bindings.find(a_id); found != table.bindings.end()) {
				RecordSettingBytes(found->second);
				bytes = found->second.raised;
			}
			if (a_id != 0 && a_id <= table.names.size())
				name = table.names[a_id - 1];
			hook = table.hook;
		}
		CallSettingHook(hook, a_id, name, bytes);
	}

	void SettingRegistry::RaiseAt(const void* a_address)
	{
		auto& table = Settings();
		RaiseHook hook = nullptr;
		SettingVariableId id = 0;
		std::string_view name;
		std::vector<std::byte> bytes;
		{
			std::scoped_lock lock(table.mutex);
			if (const auto found = table.idsByAddress.find(a_address); found != table.idsByAddress.end())
				id = found->second;
			else {
				// A member of a struct-valued setting: the binding whose bytes hold the address.
				const auto at = reinterpret_cast<std::uintptr_t>(a_address);
				for (const auto& [bound, binding] : table.bindings) {
					const auto begin = reinterpret_cast<std::uintptr_t>(binding.address);
					if (at > begin && at < begin + binding.size) {
						id = bound;
						break;
					}
				}
				if (id == 0)
					return;
			}
			auto& binding = table.bindings[id];
			if (SettingBytesRaised(binding))
				return;
			RecordSettingBytes(binding);
			bytes = binding.raised;
			name = table.names[id - 1];
			hook = table.hook;
		}
		CallSettingHook(hook, id, name, bytes);
	}

	bool SettingRegistry::RaisedBytes(SettingVariableId a_id, void* a_out, std::size_t a_size)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		const auto found = table.bindings.find(a_id);
		if (found == table.bindings.end() || found->second.size != a_size)
			return false;
		std::memcpy(a_out, found->second.raised.data(), a_size);
		return true;
	}

	std::vector<SettingVariableId> SettingRegistry::Unraised()
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		std::vector<SettingVariableId> ids;
		for (const auto& [id, binding] : table.bindings)
			if (!SettingBytesRaised(binding))
				ids.push_back(id);
		return ids;
	}

	bool SettingRegistry::PublishAdopted(SettingVariableId a_id, const void* a_bytes, std::size_t a_size, void* a_before)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		const auto binding = table.bindings.find(a_id);
		const auto cell = table.cells.find(a_id);
		if (binding == table.bindings.end() || cell == table.cells.end() || binding->second.size != a_size || a_size > kAdoptedSettingCapacity)
			return false;
		LoadAdopted(*cell->second, a_before);
		StoreAdopted(*cell->second, a_bytes, a_size);
		return true;
	}

	void SettingRegistry::ReadAdopted(const AdoptedSettingCell& a_cell, void* a_out, std::size_t a_size)
	{
		auto& table = Settings();
		std::scoped_lock lock(table.mutex);
		std::memcpy(a_out, a_cell.bytes.data(), a_size);
	}
}
