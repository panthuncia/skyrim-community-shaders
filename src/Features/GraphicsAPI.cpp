// Graphics API: the menu's choice between DXVK Vulkan, native Vulkan and native D3D12, applied at the next start.

#include "Features/GraphicsAPI.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "State.h"
#include "Testing/Testing.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

#include <array>
#include <cstdlib>
#include <imgui.h>

#define I18N_KEY_PREFIX "feature.graphics_api."

namespace
{
	struct Boot
	{
		GraphicsAPI::Mode mode = GraphicsAPI::Mode::DXVK;
		GraphicsAPI::Source source = GraphicsAPI::Source::Default;
	};

	Boot g_boot;

	/**
	 * @brief An environment variable's value, read from the process environment. Not through Switches, which caches: ApplyAtBoot
	 * sets CS_ORG_NATIVE and CS_ORG_NATIVE_BACKEND after reading them, and a cached read would hide that from Native::Requested.
	 */
	std::string Environment(const char* a_name)
	{
		char value[64]{};
		const DWORD length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
		if (length == 0 || length >= sizeof(value))
			return {};
		return value;
	}

	void SetEnvironment(const char* a_name, const char* a_value)
	{
		::SetEnvironmentVariableA(a_name, a_value);
		// std::getenv reads the C runtime's own copy, which SetEnvironmentVariable does not change.
		_putenv_s(a_name, a_value);
	}

	/** @brief The mode the environment asks for, or Mode::Count when it does not set CS_ORG_NATIVE. */
	GraphicsAPI::Mode FromEnvironment()
	{
		const auto native = Environment("CS_ORG_NATIVE");
		if (native.empty())
			return GraphicsAPI::Mode::Count;
		if (native != "1")
			return GraphicsAPI::Mode::DXVK;
		return _stricmp(Environment("CS_ORG_NATIVE_BACKEND").c_str(), "d3d12") == 0 ? GraphicsAPI::Mode::NativeD3D12 : GraphicsAPI::Mode::NativeVulkan;
	}

	/** @brief The ini override ([Override] Mode), or Mode::Count when the ini sets none. */
	GraphicsAPI::Mode FromIni()
	{
		const auto path = Util::PathHelpers::GetFeatureIniPath("GraphicsAPI");
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(path.c_str()) < 0)
			return GraphicsAPI::Mode::Count;
		const char* value = ini.GetValue("Override", "Mode", "");
		if (!value || !*value)
			return GraphicsAPI::Mode::Count;
		const auto mode = GraphicsAPI::FromKey(value);
		if (mode == GraphicsAPI::Mode::Count)
			logger::error("[Graphics API] {}: [Override] Mode = {} is not a mode (dxvk, vulkan or d3d12); ignored", path.filename().string(), value);
		return mode;
	}

	/** @brief The menu's saved choice, read from SettingsUser.json, or Mode::Count when it holds none. */
	GraphicsAPI::Mode FromSettings()
	{
		const auto settings = Util::FileHelpers::ReadJsonObject(Util::PathHelpers::GetSettingsUserPath(), "user config (Graphics API)");
		if (!settings)
			return GraphicsAPI::Mode::Count;
		const auto feature = settings->find("Graphics API");
		if (feature == settings->end() || !feature->is_object())
			return GraphicsAPI::Mode::Count;
		const auto mode = feature->find("Mode");
		if (mode == feature->end() || !mode->is_string())
			return GraphicsAPI::Mode::Count;
		return GraphicsAPI::FromKey(mode->get<std::string>());
	}

	const char* SourceName(GraphicsAPI::Source a_source)
	{
		switch (a_source) {
		case GraphicsAPI::Source::Menu:
			return "the menu";
		case GraphicsAPI::Source::Ini:
			return "GraphicsAPI.ini";
		case GraphicsAPI::Source::TestRun:
			return "the test run";
		case GraphicsAPI::Source::Environment:
			return "the environment";
		default:
			return "the default";
		}
	}

	const char* ModeName(GraphicsAPI::Mode a_mode)
	{
		switch (a_mode) {
		case GraphicsAPI::Mode::NativeVulkan:
			return "Native Vulkan";
		case GraphicsAPI::Mode::NativeD3D12:
			return "Native D3D12";
		default:
			return "DXVK Vulkan";
		}
	}
}

GraphicsAPI* GraphicsAPI::GetSingleton()
{
	return &globals::features::graphicsAPI;
}

const char* GraphicsAPI::Key(Mode a_mode)
{
	switch (a_mode) {
	case Mode::NativeVulkan:
		return "vulkan";
	case Mode::NativeD3D12:
		return "d3d12";
	default:
		return "dxvk";
	}
}

GraphicsAPI::Mode GraphicsAPI::FromKey(std::string_view a_key)
{
	for (int i = 0; i < static_cast<int>(Mode::Count); ++i) {
		const auto mode = static_cast<Mode>(i);
		const std::string_view key = Key(mode);
		if (a_key.size() == key.size() && _strnicmp(a_key.data(), key.data(), key.size()) == 0)
			return mode;
	}
	return Mode::Count;
}

bool GraphicsAPI::Available(Mode a_mode)
{
	switch (a_mode) {
	case Mode::DXVK:
		return true;
	case Mode::NativeVulkan:
#if defined(CS_HAS_RENDER_GRAPH)
		return true;
#else
		return false;
#endif
	case Mode::NativeD3D12:
#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_ORG_HAS_D3D12)
		return true;
#else
		return false;
#endif
	default:
		return false;
	}
}

void GraphicsAPI::ApplyAtBoot()
{
	Boot boot;
	if (const auto mode = FromEnvironment(); mode != Mode::Count) {
		// Set by the user or a test configuration: left exactly as it is.
		boot = { mode, Source::Environment };
		g_boot = boot;
		logger::info("[Graphics API] {} (set by the environment: CS_ORG_NATIVE={}, CS_ORG_NATIVE_BACKEND={})", ModeName(mode),
			Environment("CS_ORG_NATIVE"), Environment("CS_ORG_NATIVE_BACKEND"));
		return;
	}
	if (Testing::Configured()) {
		boot = { Mode::DXVK, Source::TestRun };
	} else if (const auto ini = FromIni(); ini != Mode::Count) {
		boot = { ini, Source::Ini };
	} else if (const auto menu = FromSettings(); menu != Mode::Count) {
		boot = { menu, Source::Menu };
	}
	if (!Available(boot.mode)) {
		logger::error("[Graphics API] {} (set by {}) is not in this build; running on DXVK Vulkan", ModeName(boot.mode), SourceName(boot.source));
		boot.mode = Mode::DXVK;
	}
	g_boot = boot;
	if (boot.mode != Mode::DXVK) {
		SetEnvironment("CS_ORG_NATIVE", "1");
		SetEnvironment("CS_ORG_NATIVE_BACKEND", boot.mode == Mode::NativeD3D12 ? "d3d12" : "vulkan");
	}
	logger::info("[Graphics API] {} (set by {})", ModeName(boot.mode), SourceName(boot.source));
}

GraphicsAPI::Mode GraphicsAPI::BootMode()
{
	return g_boot.mode;
}

GraphicsAPI::Source GraphicsAPI::BootSource()
{
	return g_boot.source;
}

std::string GraphicsAPI::GetDisplayName()
{
	return T(TKEY("name"), "Graphics API");
}

std::pair<std::string, std::vector<std::string>> GraphicsAPI::GetFeatureSummary()
{
	return { T(TKEY("description"), "Chooses the graphics API Community Shaders runs the game on. Takes effect after a restart."),
		{ T(TKEY("key_feature_1"), "DXVK Vulkan: the game's Direct3D 11 translated to Vulkan (default)"),
			T(TKEY("key_feature_2"), "Native Vulkan: no DXVK, the render graph owns the device (experimental)"),
			T(TKEY("key_feature_3"), "Native Direct3D 12: as native Vulkan, on Direct3D 12 (experimental)") } };
}

void GraphicsAPI::DrawSettings()
{
	const auto& theme = Menu::GetSingleton()->GetTheme();
	const std::array<const char*, static_cast<size_t>(Mode::Count)> labels = {
		T(TKEY("mode_dxvk"), "DXVK Vulkan"),
		T(TKEY("mode_vulkan"), "Native Vulkan (experimental)"),
		T(TKEY("mode_d3d12"), "Native Direct3D 12 (experimental)"),
	};
	const std::array<const char*, static_cast<size_t>(Mode::Count)> tooltips = {
		T(TKEY("mode_dxvk_tooltip"), "DXVK translates the game's Direct3D 11 to Vulkan. The most complete and tested mode."),
		T(TKEY("mode_vulkan_tooltip"), "No DXVK: Community Shaders makes its own Vulkan device and draws the game through its render graph."),
		T(TKEY("mode_d3d12_tooltip"), "No DXVK: as Native Vulkan, on a Direct3D 12 device. Some features are not available yet."),
	};

	const auto boot = BootMode();
	const auto source = BootSource();
	ImGui::Text("%s %s", T(TKEY("running_on"), "Running on:"), labels[static_cast<size_t>(boot)]);

	const bool overridden = source == Source::Environment || source == Source::Ini || source == Source::TestRun;
	ImGui::Spacing();
	ImGui::TextUnformatted(T(TKEY("next_start"), "At the next start:"));
	for (int i = 0; i < static_cast<int>(Mode::Count); ++i) {
		const auto mode = static_cast<Mode>(i);
		Util::DisableGuard disable(!Available(mode));
		if (ImGui::RadioButton(labels[i], selected == mode))
			selected = mode;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(tooltips[i]);
			if (!Available(mode))
				ImGui::TextUnformatted(T(TKEY("not_built"), "This build does not include this mode."));
		}
	}

	ImGui::Spacing();
	if (overridden) {
		const char* why = source == Source::Ini ?
		                      T(TKEY("override_ini"), "GraphicsAPI.ini ([Override] Mode) sets the mode, so this choice applies only once that line is removed.") :
		                  source == Source::TestRun ?
		                      T(TKEY("override_test"), "This is a test run: its configuration sets the mode, and this choice applies to normal starts.") :
		                      T(TKEY("override_env"), "The environment (CS_ORG_NATIVE) sets the mode, so this choice applies only once that variable is unset.");
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(theme.StatusPalette.Warning, "%s", why);
		ImGui::PopTextWrapPos();
	} else if (selected != boot) {
		ImGui::TextColored(theme.StatusPalette.RestartNeeded, "%s", T(TKEY("restart"), "Restart the game to switch the graphics API."));
	}
}

void GraphicsAPI::LoadSettings(json& o_json)
{
	selected = Mode::DXVK;
	if (const auto found = o_json.find("Mode"); found != o_json.end() && found->is_string()) {
		const auto mode = FromKey(found->get<std::string>());
		if (mode != Mode::Count)
			selected = mode;
	}
}

void GraphicsAPI::SaveSettings(json& o_json)
{
	o_json["Mode"] = Key(selected);
}

void GraphicsAPI::RestoreDefaultSettings()
{
	selected = Mode::DXVK;
}
