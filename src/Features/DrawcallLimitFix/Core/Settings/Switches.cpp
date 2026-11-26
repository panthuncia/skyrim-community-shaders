// Switches: Core's switches and the features' registered ones, read once from the switches file and environment, and the startup summary.
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "RenderGraph/Fatal.h"
#include "Testing/SwitchRegistry.h"
#include "Utils/SwitchFile.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <string_view>
#include <vector>

namespace DCLF
{
	namespace
	{
		using Kind = Testing::SwitchRegistry::Kind;

		// Which values narrow a feature, as its reader interprets them.
		enum class Reduced : std::uint8_t
		{
			Never,       // not a feature
			UnlessOne,   // on when unset or "1"
			Zero,        // off only with "0"
			Cull,        // "off" and "frustum" narrow it
		};

		struct Entry
		{
			Switch id;
			std::string_view name;  // a row of the switch table (src/Testing/SwitchRegistry.cpp), which has its kind and description
			Reduced reduced;
		};

		using enum Switch;
		// A feature's Api::SwitchKind is one of these.
		static_assert(static_cast<Kind>(Api::SwitchKind::Feature) == Kind::Feature && static_cast<Kind>(Api::SwitchKind::Parity) == Kind::Parity &&
					  static_cast<Kind>(Api::SwitchKind::Diagnostic) == Kind::Diagnostic && static_cast<Kind>(Api::SwitchKind::Test) == Kind::Test &&
					  static_cast<Kind>(Api::SwitchKind::Other) == Kind::Other);
		constexpr auto N = Reduced::Never;

		// One row per switch, in Switch order, checked below. The startup summary lists them in this order.
		constexpr Entry kCore[] = {
			// Features: unset is DCLF's full featureset; the startup log names a run that narrows one.
			{ Dclf, "CS_DCLF", Reduced::Zero },
			{ SceneCopies, "CS_DCLF_SCENE_COPIES", N },
			{ Shrink, "CS_DCLF_SHRINK", N },
			{ Cull, "CS_DCLF_CULL", Reduced::Cull },
			// The CPU-driven pipeline, an experiment, seeds the live toggles. See dclf-architecture.md, "The CPU-driven pipeline".
			{ CpuPipeline, "CS_DCLF_PIPELINE", N },
			{ CpuRecord, "CS_DCLF_CPU_RECORD", N },
			{ CpuBatch, "CS_DCLF_CPU_BATCH", N },
			{ PlainSnowSparkle, "CS_DCLF_PLAIN_SNOW_SPARKLE", Reduced::UnlessOne },
			// Parity checks: a live path against a from-scratch reference, logging each mismatch.
			{ Parity, "CS_DCLF_PARITY", N },
			{ PlayerParity, "CS_DCLF_PLAYER_PARITY", N },
			{ CaptureParity, "CS_DCLF_CAPTURE_PARITY", N },
			{ ParityBoth, "CS_DCLF_PARITY_BOTH", N },
			// Diagnostics. See dclf-validation.md, "Probes and diagnostics".
			{ Stats, "CS_DCLF_STATS", N },
			{ PassStats, "CS_DCLF_PASS_STATS", N },
			{ Profile, "CS_DCLF_PROFILE", N },
			{ ShaderDebug, "CS_DCLF_SHADER_DEBUG", N },
			{ ShaderSourceDir, "CS_DCLF_SHADER_SOURCE_DIR", N },
			{ StageDump, "CS_DCLF_STAGE_DUMP", N },
			{ TableStart, "CS_DCLF_TABLE_START", N },
			{ ReleaseGuard, "CS_DCLF_RELEASE_GUARD", N },
		};
		static_assert(std::size(kCore) == static_cast<std::size_t>(Switch::Count));
		static_assert([] {
			for (std::size_t i = 0; i < std::size(kCore); ++i)
				if (static_cast<std::size_t>(kCore[i].id) != i)
					return false;
			return true;
		}());

		/** @brief The switch table's row for a Core switch. A missing row is a programming error. */
		const Testing::SwitchRegistry::Row& CoreSwitchRow(const Entry& a_entry)
		{
			const auto* row = Testing::SwitchRegistry::Find(a_entry.name);
			if (!row)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the switch {} has no row in src/Testing/SwitchRegistry.cpp", a_entry.name));
			return *row;
		}

		struct Values
		{
			std::array<std::string, static_cast<std::size_t>(Switch::Count)> value;
			std::array<bool, static_cast<std::size_t>(Switch::Count)> enabled{};
			std::array<Kind, static_cast<std::size_t>(Switch::Count)> kind{};  // from the switch table
			std::vector<std::string> unknown;  // CS_DCLF_* names set in the file or environment that no row names
		};

		const Values& Read()
		{
			static const Values values = [] {
				Values v;
				for (const auto& entry : kCore) {
					v.kind[static_cast<std::size_t>(entry.id)] = CoreSwitchRow(entry).kind;
					auto& value = v.value[static_cast<std::size_t>(entry.id)];
					value = Util::SwitchFile::Value(entry.name);
					v.enabled[static_cast<std::size_t>(entry.id)] = value == "1";
				}
				// Known: a row of the switch table, Core's or another reader's.
				auto known = [](std::string_view a_name) { return Testing::SwitchRegistry::Find(a_name) != nullptr; };
				for (const auto& name : Util::SwitchFile::FileNames())
					if (!known(name))
						v.unknown.push_back(name);
				if (auto* block = GetEnvironmentStringsW()) {
					for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
						const std::wstring_view pair(p);
						const auto wideName = pair.substr(0, pair.find(L'='));
						if (!wideName.starts_with(L"CS_DCLF"))
							continue;
						std::string name;  // switch names are ASCII
						for (const wchar_t c : wideName)
							name.push_back(static_cast<char>(c));
						if (!known(name) && std::ranges::find(v.unknown, name) == v.unknown.end())
							v.unknown.push_back(name);
					}
					FreeEnvironmentStringsW(block);
				}
				return v;
			}();
			return values;
		}

		// The features' switches, each read as it registered and listed next to the row its anchor names.
		// Written only at install, before DCLF's threads start. Read-only after.
		struct Registered
		{
			std::string name, description, after, before;
			Kind kind = Kind::Feature;
			Reduced reduced = Reduced::Never;
			std::string value;
			bool enabled = false;
		};
		std::deque<Registered>& RegisteredSwitches()
		{
			static std::deque<Registered> registered;
			return registered;
		}

		/** @brief Calls a_row for the features' rows anchored at a_name, each surrounded by the rows anchored at it in turn. a_listed marks those listed. */
		template <class Row>
		void ListAnchoredAt(Row& a_row, std::vector<std::uint8_t>& a_listed, std::string Registered::*a_anchor, std::string_view a_name)
		{
			const auto& registered = RegisteredSwitches();
			for (std::size_t i = 0; i < registered.size(); ++i)
				if (!a_listed[i] && registered[i].*a_anchor == a_name) {
					a_listed[i] = 1;
					const auto& entry = registered[i];
					ListAnchoredAt(a_row, a_listed, &Registered::before, entry.name);
					a_row(std::string_view(entry.name), entry.kind, entry.reduced, entry.value);
					ListAnchoredAt(a_row, a_listed, &Registered::after, entry.name);
				}
		}

		/** @brief Calls a_row for every switch row in listing order. */
		template <class Row>
		void ForEachRow(Row&& a_row)
		{
			const auto& registered = RegisteredSwitches();
			std::vector<std::uint8_t> listed(registered.size(), 0);
			for (const auto& entry : kCore) {
				ListAnchoredAt(a_row, listed, &Registered::before, entry.name);
				a_row(entry.name, Read().kind[static_cast<std::size_t>(entry.id)], entry.reduced, SwitchValue(entry.id));
				ListAnchoredAt(a_row, listed, &Registered::after, entry.name);
			}
			for (std::size_t i = 0; i < registered.size(); ++i)
				if (!listed[i])
					a_row(std::string_view(registered[i].name), registered[i].kind, registered[i].reduced, registered[i].value);
		}

		bool IsReduced(Reduced a_rule, const std::string& a_value)
		{
			switch (a_rule) {
			case Reduced::UnlessOne:
				return !a_value.empty() && a_value != "1";
			case Reduced::Zero:
				return a_value == "0";
			case Reduced::Cull:
				return a_value == "off" || a_value == "frustum";
			default:
				return false;
			}
		}
	}

	std::filesystem::path SwitchesDirectory() { return Util::SwitchFile::Directory(); }

	const std::string& SwitchValue(Switch a_switch)
	{
		return Read().value[static_cast<std::size_t>(a_switch)];
	}

	bool SwitchEnabled(Switch a_switch)
	{
		return Read().enabled[static_cast<std::size_t>(a_switch)];
	}

	Api::SwitchId RegisterSwitch(const Api::FeatureSwitch& a_switch, std::string_view a_feature)
	{
		auto& registered = RegisteredSwitches();
		if (a_switch.name.empty() || Testing::SwitchRegistry::Find(a_switch.name))
			::RenderGraph::Fatal("Drawcall Limit Fix",
				fmt::format("{} registered the switch '{}', which the switch table has (or no name)", a_feature, a_switch.name));
		for (const auto& entry : registered)
			if (entry.name == a_switch.name)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered the switch {} twice", a_feature, a_switch.name));
		// Its anchor may be a switch a later feature registers, so CheckSwitchAnchors checks it at the end.
		if (!a_switch.before.empty() && !a_switch.after.empty())
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s switch {} is listed before {} and after {}", a_feature, a_switch.name, a_switch.before,
				a_switch.after));
		auto& entry = registered.emplace_back();
		entry.name = a_switch.name;
		entry.description = a_switch.description;
		entry.after = a_switch.after;
		entry.before = a_switch.before;
		entry.kind = static_cast<Kind>(a_switch.kind);
		entry.reduced = a_switch.reduced == Api::SwitchReduced::UnlessOne ? Reduced::UnlessOne : a_switch.reduced == Api::SwitchReduced::Zero ? Reduced::Zero : Reduced::Never;
		entry.value = Util::SwitchFile::Value(entry.name, false);
		entry.enabled = entry.value == "1";
		return { static_cast<std::uint32_t>(registered.size() - 1) };
	}

	namespace
	{
		std::atomic<bool> featureSwitchesRegistered{ false };
	}

	void ForEachFeatureSwitch(const std::function<void(std::string_view, std::string_view, bool, std::string_view)>& a_row)
	{
		for (const auto& entry : RegisteredSwitches())
			a_row(entry.name, Testing::SwitchRegistry::KindName(entry.kind), entry.reduced == Reduced::UnlessOne || entry.reduced == Reduced::Zero,
				entry.description);
	}

	bool FeatureSwitchesRegistered() { return featureSwitchesRegistered.load(std::memory_order_acquire); }

	void CheckSwitchAnchors()
	{
		// A row no Core row lists has an anchor that names no switch, or anchors that loop back to it.
		const auto& registered = RegisteredSwitches();
		std::vector<std::uint8_t> listed(registered.size(), 0);
		auto none = [](std::string_view, Kind, Reduced, const std::string&) {};
		for (const auto& entry : kCore) {
			ListAnchoredAt(none, listed, &Registered::before, entry.name);
			ListAnchoredAt(none, listed, &Registered::after, entry.name);
		}
		for (std::size_t i = 0; i < registered.size(); ++i) {
			const auto& entry = registered[i];
			if (!listed[i] && (!entry.after.empty() || !entry.before.empty()))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the switch {} is listed {} {}, which no switch is (or its anchors loop)", entry.name,
					entry.after.empty() ? "before" : "after", entry.after.empty() ? entry.before : entry.after));
		}
		featureSwitchesRegistered.store(true, std::memory_order_release);
	}

	bool AnyObserverSwitch()
	{
		// Set to anything but 0. The shader build's switches do not count, since CS_DCLF_SHADER_DEBUG is on by default.
		bool any = false;
		ForEachRow([&](std::string_view a_name, Kind a_kind, Reduced, const std::string& a_value) {
			const bool observer = (a_kind == Kind::Parity || a_kind == Kind::Diagnostic) && a_name != "CS_DCLF_SHADER_DEBUG" && a_name != "CS_DCLF_SHADER_SOURCE_DIR";
			any = any || (observer && !a_value.empty() && a_value != "0");
		});
		return any;
	}

	std::string_view SwitchName(Switch a_switch)
	{
		return static_cast<std::size_t>(a_switch) < std::size(kCore) ? kCore[static_cast<std::size_t>(a_switch)].name : std::string_view{};
	}

	std::string_view SwitchName(Api::SwitchId a_switch)
	{
		const auto& registered = RegisteredSwitches();
		return a_switch.index < registered.size() ? std::string_view(registered[a_switch.index].name) : std::string_view{};
	}

	const std::string& SwitchValue(Api::SwitchId a_switch)
	{
		static const std::string kUnset;
		const auto& registered = RegisteredSwitches();
		return a_switch.index < registered.size() ? registered[a_switch.index].value : kUnset;
	}

	bool SwitchEnabled(Api::SwitchId a_switch)
	{
		const auto& registered = RegisteredSwitches();
		return a_switch.index < registered.size() && registered[a_switch.index].enabled;
	}

	const std::string& RegisteredSwitchValue(std::string_view a_name)
	{
		static const std::string kUnset;
		for (const auto& entry : RegisteredSwitches())
			if (entry.name == a_name)
				return entry.value;
		return kUnset;
	}

	std::string ReducedFeatures()
	{
		std::string text;
		ForEachRow([&](std::string_view a_name, Kind, Reduced a_reduced, const std::string& a_value) {
			if (!IsReduced(a_reduced, a_value))
				return;
			if (!text.empty())
				text += ", ";
			text += fmt::format("{}={}", a_name, a_value);
		});
		return text;
	}

	std::string SwitchSummary()
	{
		// Grouped by kind, so a run's features, checks and probes read apart at a glance.
		constexpr std::pair<Kind, std::string_view> kGroups[] = {
			{ Kind::Feature, "features" }, { Kind::Parity, "parity" }, { Kind::Diagnostic, "diagnostics" }, { Kind::Test, "test" }, { Kind::Other, "other" }
		};
		std::string text;
		for (const auto& [kind, label] : kGroups) {
			std::string group;
			const Kind wanted = kind;
			ForEachRow([&](std::string_view a_name, Kind a_kind, Reduced, const std::string& a_value) {
				if (a_kind == wanted && !a_value.empty())
					group += fmt::format("{}{}={}", group.empty() ? "" : ", ", a_name, a_value);
			});
			if (!group.empty())
				text += fmt::format("{}{}: {}", text.empty() ? "" : "; ", label, group);
		}
		// Read before the features registered theirs, so a name one of them registered counts as known.
		std::string unknown;
		for (const auto& name : Read().unknown)
			if (std::ranges::none_of(RegisteredSwitches(), [&](const Registered& a_entry) { return a_entry.name == name; }))
				unknown += fmt::format("{}{}", unknown.empty() ? "" : ", ", name);
		if (!unknown.empty())
			text += fmt::format("{}UNKNOWN (read by nothing): {}", text.empty() ? "" : "; ", unknown);
		return text.empty() ? std::string("none set") : text;
	}
}
