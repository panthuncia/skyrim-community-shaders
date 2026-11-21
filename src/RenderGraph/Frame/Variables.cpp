#include "RenderGraph/Frame/Variables.h"

#include "RenderGraph/Fatal.h"
#include "Utils/Game.h"

// The value types of kTable's rows. The generic functions are instantiated for each.
#define CS_FRAME_VARIABLE_TYPES(X)  \
	X(Extent2D)                     \
	X(Ratios)                       \
	X(Upscaler)                     \
	X(bool)                         \
	X(TargetIdentity)               \
	X(ShadowMapShape)               \
	X(CubemapShape)                 \
	X(HeightMapIdentity)            \
	X(SurfaceFormat)                \
	X(UpscalerOptions)              \
	X(ImageSpaceShape)              \
	X(FrameGen::PresentationConfig) \
	X(::RenderGraph::Engine::Targets::ShapeTable) \
	X(std::uint64_t)

#include "RenderGraph/Frame/VariablesLane.h"

#include <ORGModuleServices/Variables.h>

#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RenderGraph::Frame::Vars
{
	static_assert(Lane::SettingValue::kCapacity == CS::kAdoptedSettingCapacity, "a setting's adopted copy holds what its variable carries");

	namespace
	{
		using org::services::Variable;
		using org::services::VariableSnapshot;

		// Events after this many log one line in kVariableLogEvery.
		constexpr std::uint64_t kVariableLoggedFirst = 64;
		constexpr std::uint64_t kVariableLogEvery = 100;

		/** @brief One variable's events, for the census and the rate limit. */
		struct VariableEventCounts
		{
			std::uint64_t window = 0;
			std::uint64_t session = 0;
		};

		/** @brief A CS::Setting's variable and the name it was declared with. */
		struct SettingVariable
		{
			Variable<Lane::SettingValue> variable;
			std::string name;
			std::uint64_t counter = 0;  // for a setting too big to carry
		};

		/** @brief A row's listener. It reads the row from the snapshot adopted before (null at the first) and the one adopted now. */
		using RowListener = std::function<void(const VariableSnapshot* a_before, const VariableSnapshot& a_now)>;

		/** @brief A setting's listener, with the size of the value it takes. */
		struct SettingListener
		{
			std::size_t size = 0;
			std::function<void(const void* a_before, const void* a_now)> listener;
		};

		template <std::size_t... I>
		constexpr std::array<bool, kTableSize> RowSizes(std::index_sequence<I...>)
		{
			return { std::get<I>(kTable).sizes... };
		}
		constexpr auto kRowSizes = RowSizes(std::make_index_sequence<kTableSize>{});

		struct FrameVariables
		{
			org::services::VariableSet set;

			std::mutex eventsMutex;
			std::map<std::string, VariableEventCounts, std::less<>> events;

			// Serial bumps and the settings' variables, declared on first use.
			std::mutex serialMutex;
			std::map<std::uint32_t, SettingVariable> settings;
			std::map<std::uint32_t, std::uint32_t> settingIdsByIndex;  // a setting variable's index to its setting id

			std::mutex listenersMutex;
			std::array<std::vector<RowListener>, kTableSize> rowListeners;
			std::map<std::uint32_t, std::vector<SettingListener>> settingListeners;  // by setting id

			// Render thread.
			std::shared_ptr<const VariableSnapshot> adopted;

			FrameVariables() { DeclareRows(std::make_index_sequence<kTableSize>{}); }

		private:
			template <std::size_t... I>
			void DeclareRows(std::index_sequence<I...>)
			{
				(DeclareRow<I>(), ...);
			}

			/** @brief Declares row I at T{}. Its index in the set must be I. */
			template <std::size_t I>
			void DeclareRow()
			{
				using T = Detail::RowType<I>;
				const auto& row = std::get<I>(kTable);
				const auto variable = set.Declare(std::string(row.name), T{}, { .sizes = row.sizes });
				if (variable.index != I)
					RenderGraph::Fatal("frame variables", fmt::format("{} was declared at index {}, not its row {}", row.name, variable.index, I));
			}
		};

		FrameVariables& FrameVariableSet()
		{
			// Never destroyed: writers may run during process teardown.
			static auto* variables = new FrameVariables;
			return *variables;
		}

		/** @brief The set's handle of a row. A variable with no row is fatal. */
		template <class T>
		Variable<T> RowOf(Var<T> a_variable)
		{
			if (a_variable.index >= kTableSize)
				RenderGraph::Fatal("frame variables", fmt::format("{} is no row of the frame variable table", a_variable.name));
			return { a_variable.index };
		}

		/** @brief Counts an event and logs it, the first ones and then one in kVariableLogEvery. */
		template <class Describe>
		void NoteVariableEvent(std::string_view a_name, Describe&& a_describe, std::string_view a_writer)
		{
			auto& variables = FrameVariableSet();
			std::uint64_t session = 0;
			{
				const std::lock_guard lock(variables.eventsMutex);
				auto it = variables.events.find(a_name);
				if (it == variables.events.end())
					it = variables.events.emplace(std::string(a_name), VariableEventCounts{}).first;
				++it->second.window;
				session = ++it->second.session;
			}
			if (session <= kVariableLoggedFirst || session % kVariableLogEvery == 0)
				logger::info("[ORG] variable {}: {} ({}){}", a_name, a_describe(), a_writer, session > kVariableLoggedFirst ? fmt::format(" [event {}]", session) : "");
		}

		std::string VariableCountsText(const std::map<std::string, VariableEventCounts, std::less<>>& a_events, bool a_session, std::uint64_t& a_total)
		{
			std::string text;
			for (const auto& [name, counts] : a_events) {
				const auto count = a_session ? counts.session : counts.window;
				if (!count)
					continue;
				a_total += count;
				text += fmt::format("{}{} {}", text.empty() ? "" : ", ", name, count);
			}
			return text.empty() ? std::string("none") : text;
		}
	}

	template <class T>
	bool Set(Var<T> a_variable, const T& a_value, std::string_view a_writer)
	{
		auto& set = FrameVariableSet().set;
		const auto variable = RowOf(a_variable);
		const T old = set.Get(variable);
		if (old == a_value || !set.Set(variable, a_value))
			return false;
		NoteVariableEvent(a_variable.name, [&] { return std::format("{} -> {}", old, a_value); }, a_writer);
		return true;
	}

	void Raise(Var<std::uint64_t> a_variable, std::string_view a_writer)
	{
		auto& variables = FrameVariableSet();
		const auto variable = RowOf(a_variable);
		std::uint64_t serial = 0;
		{
			const std::lock_guard lock(variables.serialMutex);
			serial = variables.set.Get(variable) + 1;
			variables.set.Set(variable, serial);
		}
		NoteVariableEvent(a_variable.name, [&] { return std::format("{} -> {}", serial - 1, serial); }, a_writer);
	}

	template <class T>
	T Written(Var<T> a_variable)
	{
		return FrameVariableSet().set.Get(RowOf(a_variable));
	}

	template <class T>
	T Adopted(Var<T> a_variable)
	{
		// Every row is declared with T{}.
		const auto variable = RowOf(a_variable);
		const auto& adopted = FrameVariableSet().adopted;
		return adopted ? adopted->Get(variable) : T{};
	}

	template <class T>
	void OnAdopted(Var<T> a_variable, std::function<void(const T& a_before, const T& a_now)> a_listener)
	{
		const auto variable = RowOf(a_variable);
		auto& variables = FrameVariableSet();
		const std::lock_guard lock(variables.listenersMutex);
		variables.rowListeners[variable.index].push_back(
			[variable, listener = std::move(a_listener)](const VariableSnapshot* a_before, const VariableSnapshot& a_now) {
				const T before = a_before ? a_before->Get(variable) : T{};
				const T& now = a_now.Get(variable);
				if (!(before == now))
					listener(before, now);
			});
	}

	void Detail::OnSettingAdopted(CS::SettingVariableId a_id, std::size_t a_size, std::function<void(const void* a_before, const void* a_now)> a_listener)
	{
		if (!a_id)
			RenderGraph::Fatal("frame variables", "OnAdopted of a setting that is not bound (CS::Setting::Bind)");
		auto& variables = FrameVariableSet();
		const std::lock_guard lock(variables.listenersMutex);
		variables.settingListeners[a_id].push_back({ a_size, std::move(a_listener) });
	}

#define CS_FRAME_VARIABLE_INSTANTIATE(T)                        \
		template bool Set<T>(Var<T>, const T&, std::string_view); \
		template T Written<T>(Var<T>);                             \
		template T Adopted<T>(Var<T>);                             \
		template void OnAdopted<T>(Var<T>, std::function<void(const T&, const T&)>);
	CS_FRAME_VARIABLE_TYPES(CS_FRAME_VARIABLE_INSTANTIATE)
#undef CS_FRAME_VARIABLE_INSTANTIATE

	namespace
	{
		Lane::SettingValue MakeSettingValue(SettingVariable* a_variable, const void* a_bytes, std::size_t a_size)
		{
			Lane::SettingValue value;
			if (a_size > Lane::SettingValue::kCapacity || (!a_bytes && a_size)) {
				// The value cannot travel with the revision. The event still does.
				value.size = Lane::SettingValue::kTooBig;
				const std::uint64_t counter = a_variable ? ++a_variable->counter : 0;
				std::memcpy(value.bytes.data(), &counter, sizeof(counter));
				return value;
			}
			value.size = static_cast<std::uint32_t>(a_size);
			if (a_size)
				std::memcpy(value.bytes.data(), a_bytes, a_size);
			return value;
		}

		/** @brief The setting's variable, declared with a_initial when it has none. Under serialMutex. */
		SettingVariable& SettingVariableOf(std::uint32_t a_settingId, std::string_view a_name, const Lane::SettingValue& a_initial)
		{
			auto& variables = FrameVariableSet();
			auto it = variables.settings.find(a_settingId);
			if (it == variables.settings.end()) {
				std::string name = fmt::format("Setting.{}", a_name);
				// Another setting with this name keeps its variable. This one is told apart by its id.
				if (variables.set.Find(name) != org::services::kNoVariable)
					name = fmt::format("Setting.{}#{}", a_name, a_settingId);
				it = variables.settings.emplace(a_settingId, SettingVariable{ variables.set.Declare(name, a_initial), name }).first;
				variables.settingIdsByIndex[it->second.variable.index] = a_settingId;
			}
			return it->second;
		}

		/** @brief Publishes a setting variable's adopted value to its setting, and runs its listeners when the value changed. */
		void AdoptSetting(std::uint32_t a_settingId, const Lane::SettingValue& a_value, const std::vector<SettingListener>* a_listeners)
		{
			// A value too big to carry has no adopted copy.
			if (a_value.size == Lane::SettingValue::kTooBig)
				return;
			std::array<std::byte, Lane::SettingValue::kCapacity> before{};
			if (!CS::SettingRegistry::PublishAdopted(a_settingId, a_value.bytes.data(), a_value.size, before.data()))
				return;
			if (!a_listeners || std::memcmp(before.data(), a_value.bytes.data(), a_value.size) == 0)
				return;
			for (const auto& listener : *a_listeners) {
				if (listener.size != a_value.size)
					RenderGraph::Fatal("frame variables", fmt::format("a listener of the setting {} takes {} bytes, which holds {}", CS::SettingRegistry::Name(a_settingId), listener.size,
						a_value.size));
				listener.listener(before.data(), a_value.bytes.data());
			}
		}
	}

	void RaiseSetting(std::uint32_t a_settingId, std::string_view a_name, const void* a_bytes, std::size_t a_size)
	{
		auto& variables = FrameVariableSet();
		std::string name;
		bool changed = false;
		{
			const std::lock_guard lock(variables.serialMutex);
			const auto existing = variables.settings.find(a_settingId);
			const bool known = existing != variables.settings.end();
			auto value = MakeSettingValue(known ? &existing->second : nullptr, a_bytes, a_size);
			auto& setting = SettingVariableOf(a_settingId, a_name, value);
			// A new variable holds this value already. The raise still counts as its event.
			changed = variables.set.Set(setting.variable, value) || !known;
			name = setting.name;
		}
		if (changed)
			NoteVariableEvent(name, [] { return std::string("new value"); }, "setting write");
	}

	void LogStats()
	{
		auto& variables = FrameVariableSet();
		const std::lock_guard lock(variables.eventsMutex);
		std::uint64_t window = 0, session = 0;
		const auto windowText = VariableCountsText(variables.events, false, window);
		const auto sessionText = VariableCountsText(variables.events, true, session);
		logger::info("[ORG] variables: events {} [{}]; session {} [{}]", window, windowText, session, sessionText);
		for (auto& [name, counts] : variables.events)
			counts.window = 0;
	}

	namespace Lane
	{
		std::shared_ptr<const VariableSnapshot> Take()
		{
			return FrameVariableSet().set.Take();
		}

		bool ConstantsOnly(std::uint32_t a_index)
		{
			// The rows declared with sizes false. A derived variable of one is refused by the set unless it is constants only too,
			// and none is declared.
			return a_index < kTableSize && !kRowSizes[a_index];
		}

		const std::shared_ptr<const VariableSnapshot>& Adopted()
		{
			return FrameVariableSet().adopted;
		}

		void Adopt(std::shared_ptr<const VariableSnapshot> a_snapshot)
		{
			auto& variables = FrameVariableSet();
			const auto before = std::exchange(variables.adopted, std::move(a_snapshot));
			if (!variables.adopted || before == variables.adopted)
				return;
			const auto& now = *variables.adopted;
			std::vector<org::services::VariableIndex> changed;
			if (before)
				changed = now.ChangedSince(*before);
			else
				for (org::services::VariableIndex i = 0; i < now.Size(); ++i)
					changed.push_back(i);
			if (changed.empty())
				return;
			// Copied, so a listener may add listeners.
			std::array<std::vector<RowListener>, kTableSize> rowListeners;
			std::map<std::uint32_t, std::vector<SettingListener>> settingListeners;
			{
				const std::lock_guard lock(variables.listenersMutex);
				rowListeners = variables.rowListeners;
				settingListeners = variables.settingListeners;
			}
			std::map<std::uint32_t, std::uint32_t> settingIds;
			{
				const std::lock_guard lock(variables.serialMutex);
				settingIds = variables.settingIdsByIndex;
			}
			for (const auto index : changed) {
				if (index < kTableSize) {
					for (const auto& listener : rowListeners[index])
						listener(before.get(), now);
					continue;
				}
				// Past the rows: the settings' variables, and variables derived from them, which nothing listens to.
				const auto id = settingIds.find(index);
				if (id == settingIds.end())
					continue;
				const auto listeners = settingListeners.find(id->second);
				AdoptSetting(id->second, now.Get(Variable<SettingValue>{ index }), listeners == settingListeners.end() ? nullptr : &listeners->second);
			}
		}

		const org::services::VariableSet& Set()
		{
			return FrameVariableSet().set;
		}

		std::uint32_t DeriveUint(std::string a_name, std::uint32_t a_dependency, std::function<std::uint32_t(const VariableSnapshot&)> a_fn)
		{
			return FrameVariableSet().set.Derive<std::uint32_t>(std::move(a_name), { a_dependency }, std::move(a_fn)).index;
		}

		std::uint32_t IndexOf(std::string_view a_name)
		{
			return FrameVariableSet().set.Find(a_name);
		}

		std::uint32_t SettingIndex(std::uint32_t a_settingId, std::string_view a_name, const void* a_bytes, std::size_t a_size)
		{
			auto& variables = FrameVariableSet();
			const std::lock_guard lock(variables.serialMutex);
			const bool known = variables.settings.contains(a_settingId);
			if (!known && !a_bytes)
				return UINT32_MAX;
			auto& setting = SettingVariableOf(a_settingId, a_name, known ? Lane::SettingValue{} : MakeSettingValue(nullptr, a_bytes, a_size));
			return setting.variable.index;
		}

		template <class T>
		T ValueIn(const VariableSnapshot& a_values, Var<T> a_variable)
		{
			return a_values.Get(RowOf(a_variable));
		}

#define CS_FRAME_VARIABLE_INSTANTIATE(T) template T ValueIn<T>(const VariableSnapshot&, Var<T>);
		CS_FRAME_VARIABLE_TYPES(CS_FRAME_VARIABLE_INSTANTIATE)
#undef CS_FRAME_VARIABLE_INSTANTIATE

		std::string ChangedNames(const VariableSnapshot& a_newer, const VariableSnapshot* a_older)
		{
			std::string names;
			const auto add = [&](org::services::VariableIndex a_index) {
				names += fmt::format("{}{}", names.empty() ? "" : ", ", a_newer.Name(a_index));
			};
			if (a_older) {
				for (const auto index : a_newer.ChangedSince(*a_older))
					add(index);
				return names;
			}
			// A variable's first serial is 1, so a higher one was written.
			for (org::services::VariableIndex i = 0; i < a_newer.Size(); ++i)
				if (a_newer.Serial(i) > 1)
					add(i);
			return names;
		}
	}
}

namespace RenderGraph::Frame::Vars
{
	float2 DynamicArea(bool a_ignoreLock)
	{
		const auto display = Adopted(DisplaySize());
		return Util::ConvertToDynamic(float2{ static_cast<float>(display.width), static_cast<float>(display.height) }, a_ignoreLock);
	}
}

#undef CS_FRAME_VARIABLE_TYPES
