#pragma once

#include "FeatureCategories.h"
#include "FeatureConstraints.h"
#include "FeatureVersions.h"
#include "I18n/I18n.h"
#include "Utils/RestartSettings.h"
#include "Vram/DxvkMemory.h"

#include <span>
#include <string_view>
#ifdef TRACY_ENABLE
#	include <Tracy/Tracy.hpp>
#	include <Tracy/TracyD3D11.hpp>
#endif

struct SharedLighting;

struct Feature
{
	/** @brief A setting the Go To palette lists for a feature, for controls the settings catalog cannot see. */
	struct SettingSearchEntry
	{
		std::string id;                       ///< Stable per feature; keys the recent picks
		std::string label;                    ///< Shown as the result title
		std::string description;              ///< Extra searchable text
		std::function<void()> focusCallback;  ///< Runs when the palette queues the feature page, before it draws; set state for the next draw rather than assume the page is open
	};
	/** @brief Override to list settings the Go To palette should find; none by default. */
	virtual std::vector<SettingSearchEntry> GetSettingsSearchEntries() { return {}; }

	// Restart-required settings introspection. Default: none.
	// Features with restart-gated fields override these to expose them to UI
	// helpers and MCP/RemoteControl without per-feature glue.
	virtual std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const { return {}; }
	/**
 * Retrieves the boot configuration value for a given JSON key.
 * @param jsonKey The JSON key identifying which boot value to retrieve.
 * @returns A pointer to the boot configuration value, or nullptr if not defined.
 */
	virtual const void* GetBootValue(std::string_view /*jsonKey*/) const { return nullptr; }
	/**
 * Retrieves the raw settings data blob.
 * @return Pointer to the settings blob data, or nullptr if unavailable.
 */
	virtual const void* GetSettingsBlob() const { return nullptr; }
	virtual size_t GetSettingsBlobSize() const { return 0; }

	// Nexus Mods base URL for Skyrim Special Edition
	static constexpr std::string_view NEXUS_BASE_URL = "https://www.nexusmods.com/skyrimspecialedition/mods/";
	bool loaded = false;
	// Whether the feature .ini is present on disk. Unlike `loaded` this stays true for
	// features disabled at boot, so they remain reachable in the UI.
	bool installed = false;
	std::string version;
	std::string failedLoadedMessage;

	virtual std::string GetName() = 0;
	virtual std::string GetShortName() = 0;
	virtual std::string GetDisplayName() { return GetName(); }
	/** @brief "CS/<short name>": the VRAM owner of what DXVK creates during the feature's callbacks (ForEachLoadedFeature). */
	const char* GetVramOwner();
	virtual std::string GetFeatureModLink() { return ""; }
	virtual std::string_view GetShaderDefineName() { return ""; }

	/** @brief Gets additional shader define key-value pairs for this feature. */
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetShaderDefineOptions() { return {}; }
	// Names and values must outlive queued compilations; explicit caller defines take precedence.
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetCommonShaderDefines() { return {}; }

protected:
	/** @brief Builds a full Nexus Mods URL from a numeric mod ID. */
	static std::string MakeNexusModURL(std::string_view modId) noexcept
	{
		std::string url;
		url.reserve(NEXUS_BASE_URL.size() + modId.size());
		url.append(NEXUS_BASE_URL);
		url.append(modId);
		return url;
	}

public:
	virtual bool HasShaderDefine(RE::BSShader::Type) { return false; }

	/**
	 * @brief Whether the feature is a CORE feature.
	 *
	 * Core features appear under "Core Features" in the UI. If a "CORE" file
	 * is present in the feature folder root, the feature is bundled into the
	 * main CS zip and automatically considered core.
	 */
	virtual bool IsCore() const
	{
		return FeatureVersions::FEATURE_CORE_NAMES.contains(const_cast<Feature*>(this)->GetShortName());
	}

	/**
	 * @brief Gets the category for UI grouping (e.g., "Terrain", "Lighting", "Characters").
	 *
	 * Core features are distributed to their respective categories.
	 */
	virtual std::string_view GetCategory() const { return FeatureCategories::kOther; }

	/**
	 * Release maturity stage, declared via the feature .ini (Alpha/Beta flags) and
	 * baked into FeatureVersions.h at build time. Alpha takes precedence over Beta.
	 */
	enum class ReleaseStage
	{
		Release,
		Beta,
		Alpha
	};

	virtual ReleaseStage GetReleaseStage() const
	{
		const auto name = const_cast<Feature*>(this)->GetShortName();
		if (FeatureVersions::FEATURE_ALPHA_NAMES.contains(name))
			return ReleaseStage::Alpha;
		if (FeatureVersions::FEATURE_BETA_NAMES.contains(name))
			return ReleaseStage::Beta;
		return ReleaseStage::Release;
	}

	bool IsAlpha() const { return GetReleaseStage() == ReleaseStage::Alpha; }
	bool IsBeta() const { return GetReleaseStage() == ReleaseStage::Beta; }

	/**
	 * Whether the feature has yet to ship, declared via `Unreleased = True` in the
	 * feature .ini and baked into FeatureVersions.h at build time. Orthogonal to
	 * ReleaseStage: it gates UI visibility only, and carries no marker of its own.
	 */
	bool IsUnreleased() const { return FeatureVersions::FEATURE_UNRELEASED_NAMES.contains(const_cast<Feature*>(this)->GetShortName()); }

	/**
	 * Whether an unreleased feature must stay out of the UI entirely. Unreleased
	 * features only surface once their .ini is installed, so a shipped build never
	 * advertises work in progress; once installed they render like any other feature.
	 */
	bool IsHiddenUnreleased() const { return !installed && IsUnreleased(); }

	/**
	 * Whether the feature is disabled at boot by default (before any user override).
	 * Only pre-release CORE features start disabled, since they ship with the base mod
	 * without the user asking for them. A pre-release addon is installed deliberately,
	 * so it starts enabled. Users can flip either via the "Disable at Boot" menu.
	 */
	virtual bool IsDisabledByDefault() const { return IsCore() && GetReleaseStage() != ReleaseStage::Release; }

	/** @brief Whether the feature can never be disabled at boot, e.g. infrastructure other features rely on. */
	virtual bool IsAlwaysEnabled() const { return false; }

	/** @brief Whether the feature persists through the shared settings JSON rather than its own store. */
	virtual bool UsesMainSettings() const { return true; }

	/**
	 * Whether the feature will show up in the GUI menu
	 */
	virtual bool IsInMenu() const { return true; }

	/**
	 * @brief Whether the feature gets its own entry in the menu's feature list.
	 *
	 * False for a feature drawn inside a built-in page instead (the Performance Overlay lives on the
	 * Profiling page). Such a feature stays in the menu everywhere else: scene settings, presets, search tools.
	 */
	virtual bool IsInFeatureList() const { return IsInMenu(); }

	/**
	 * Whether to print the INI version missing message when this feature is unloaded
	 */
	virtual bool DrawFailLoadMessage() const { return true; }

	/**
	 * @brief Gets the feature summary and key features for hover tooltip and unloaded UI.
	 * @return Pair of {description, key feature bullet points}.
	 */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() { return {}; }

	/** @brief Allocates GPU resources (textures, buffers) needed by this feature. */
	virtual void SetupResources() {}
	// All loaded features have finished allocating resources.
	virtual void PostSetupResources() {}
	virtual void ModifySharedLighting(SharedLighting&) {}

	/** @brief Releases and recreates transient state (e.g. on resolution change). */
	virtual void Reset() {}

	virtual void DrawSettings() {}

	/** @brief Opt-in: whether the feature page shows a Debug section (see Util::DebugSection). */
	virtual bool HasDebugSection() const { return false; }

	/** @brief Draws the Debug section body; only called when HasDebugSection() is true. Use Util::DebugSection. */
	virtual void DrawDebugSection() {}

	/** @brief Draws the UI shown when this feature failed to load. */
	virtual void DrawUnloadedUI();

	// The default frame callbacks below note that they ran, so ForEachLoadedFeature skips a feature's empty callback from then on.
	// An override must not call them.

	/** @brief Per-frame work executed before the reflections pass. */
	virtual void ReflectionsPrepass() { t_defaultCallbackRan = true; }

	/** @brief Per-frame work executed before the main rendering pass. */
	virtual void Prepass() { t_defaultCallbackRan = true; }

	/** @brief Per-frame work after every feature's Prepass, so it sees the depth they wrote. */
	virtual void PostPrepass() { t_defaultCallbackRan = true; }

	/** @brief Per-frame work executed before Prepass, earliest per-frame hook. */
	virtual void EarlyPrepass() { t_defaultCallbackRan = true; }

	/**
	 * @brief Called during disk-cache shader loading to generate additional shader permutations.
	 *
	 * Invoked once per BSShader load when the shader cache is in disk-cache mode.
	 * Features can override this to inject custom permutation descriptors into the
	 * shader cache so that feature-specific technique variants are compiled and stored.
	 * This is a cold path (disk I/O, not per-frame); performance is not critical here.
	 *
	 * @param shader The BSShader being loaded.
	 */
	virtual void GenerateShaderPermutations(RE::BSShader*) {}

	/** @brief Called during SKSE Load -- earliest hook point, only for critical hooks like D3D. */
	virtual void Load() {}

	/** @brief Called after all game data files have been loaded. */
	virtual void DataLoaded() {}

	/** @brief Invalidates session state before loading a save or on starting a new game. */
	virtual void GameReset() {}

	/** @brief Called after loading an existing save. */
	virtual void GameLoaded() {}

	/** @brief Called after all SKSE plugins have finished PostLoad. */
	virtual void PostPostLoad() {}

	/**
	 * @brief Loads the feature from its INI file and JSON settings.
	 *
	 * Validates the INI version against FeatureVersions, sets the loaded flag,
	 * and delegates to LoadSettings on success.
	 * @param o_json Root JSON object containing per-feature settings sections.
	 */
	void Load(json& o_json);

	void Save(json& o_json);
	virtual void SaveSettings(json&) {}
	virtual void LoadSettings(json&) {}
	virtual void RestoreDefaultSettings() {}

	/** @brief Rewrites defaults that depend on the current settings, so a middle-click reset restores the right value.
	 *  @param a_defaults The feature's default settings, in the shape SaveSettings writes. */
	virtual void ApplyContextualDefaults(json& a_defaults) const { (void)a_defaults; }

	/**
	 * @brief Toggles the "disabled at boot" state for this feature.
	 * @return The new disabled state (true = disabled at boot).
	 */
	virtual bool ToggleAtBootSetting();

	/**
	 * @brief Whether the menu's on/off toggle also takes effect in the running session, not only at the next
	 * boot. A feature that returns true reads IsFeatureDisabled itself each frame and stands down or resumes.
	 */
	virtual bool AppliesBootToggleLive() const { return false; }

	/** @brief Why the menu's on/off toggle is locked this session. Both are null when it is not locked. */
	struct BootToggleLock
	{
		const char* note = nullptr;  // short, shown in place of the toggle's caption
		const char* why = nullptr;   // the toggle's tooltip
	};

	/**
	 * @brief Whether the menu's on/off toggle is locked this session, and why. A locked toggle is drawn disabled, and the
	 * feature's ToggleAtBootSetting should refuse the change too.
	 */
	virtual BootToggleLock GetBootToggleLock() const { return {}; }

	/**
	 * Weather analysis configuration for features that want to provide weather analysis.
	 * If sectionName is empty, the feature will not appear in weather analysis UI.
	 * Features should populate this struct to opt-in to weather analysis display.
	 */
	struct WeatherAnalysisConfig
	{
		std::string sectionName;             // Display name for the collapsible section (empty = no weather analysis)
		std::function<void()> drawFunction;  // Custom draw function for weather analysis content

		// Constructor for easy initialization
		WeatherAnalysisConfig() = default;
		WeatherAnalysisConfig(const std::string& name, std::function<void()> drawFunc) :
			sectionName(name), drawFunction(std::move(drawFunc)) {}
	};

	/**
	 * Get weather analysis configuration for this feature.
	 * Returns empty sectionName by default (no weather analysis).
	 * Features should override this to provide their weather analysis section name and draw function.
	 */
	virtual WeatherAnalysisConfig GetWeatherAnalysisConfig() const { return {}; }

	/**
	 * @brief Returns constraints this feature imposes on other features' settings
	 *
	 * Features override this to declare runtime incompatibilities with other features.
	 * The constraint system will automatically:
	 * - Force the target setting to the specified value
	 * - Disable the UI control for the constrained setting
	 * - Show a tooltip explaining which features caused the constraint
	 *
	 * @return Vector of constraints this feature currently imposes (empty if none)
	 */
	virtual std::vector<FeatureConstraints::Constraint> GetActiveConstraints() const { return {}; }

	/** @brief Invalidates any cached compiled shaders owned by this feature. */
	virtual void ClearShaderCache() {}

	static const std::vector<Feature*>& GetFeatureList();
	/** @brief Each listed feature's VRAM owner (GetVramOwner), in GetFeatureList's order. Made once. */
	static const std::vector<const char*>& GetVramOwners();

	/**
	 * @brief Finds a loaded feature by its short name.
	 *
	 * @param shortName The short name to search for.
	 * @return Pointer to the feature if found and loaded, nullptr otherwise.
	 */
	static Feature* FindFeatureByShortName(const std::string& shortName);

	/**
	 * @brief Gets sorted short names of all loaded features that appear in the menu.
	 *
	 * @return Sorted vector of short name strings.
	 */
	static std::vector<std::string> GetLoadedFeatureNames();

	// Feature utility functions
	/**
	 * @brief Gets the minimum required version for a feature as a formatted string.
	 * @param shortName The short name of the feature.
	 * @return Formatted version string, or "unknown" if the feature is not in FEATURE_MINIMAL_VERSIONS.
	 */
	static std::string GetFeatureRequiredVersion(const std::string& shortName);

	/**
	 * @brief Checks if a feature has a minimum required version defined.
	 * @param shortName The short name of the feature.
	 * @param outVersion If non-null, receives the minimum version when found.
	 * @return True if the feature exists in FEATURE_MINIMAL_VERSIONS.
	 */
	static bool IsFeatureKnown(const std::string& shortName, REL::Version* outVersion = nullptr);

	// Injected once from State after TracyD3D11Context is created so ForEachLoadedFeature
	// can emit GPU timer zones without pulling in State headers here.
#ifdef TRACY_ENABLE
	inline static TracyD3D11Ctx s_tracyCtx = nullptr;
	// Per-feature GPU zones are a D3D11 query pair each, so they are opt-in (CS_FEATURE_GPU_ZONES=1).
	inline static bool s_tracyGpuZones = false;

	/** @brief Sets the Tracy GPU context and whether ForEachLoadedFeature puts a GPU zone on each feature. */
	static void SetTracyCtx(TracyD3D11Ctx ctx, bool a_gpuZones) noexcept
	{
		s_tracyCtx = ctx;
		s_tracyGpuZones = a_gpuZones;
	}
#endif

	/** @brief Writes "<a_shortName>::<a_method>" into a_buffer without allocating, truncating to fit; returns a_buffer. */
	static const char* FormatZoneName(std::span<char> a_buffer, std::string_view a_shortName, std::string_view a_method) noexcept
	{
		if (a_buffer.empty())
			return a_buffer.data();

		std::size_t length = 0;
		for (const auto part : { a_shortName, std::string_view("::"), a_method }) {
			const auto count = std::min(part.size(), a_buffer.size() - 1 - length);
			std::memcpy(a_buffer.data() + length, part.data(), count);
			length += count;
		}
		a_buffer[length] = '\0';
		return a_buffer.data();
	}

	// Debugger events (D3D11 BeginEvent/EndEvent) around the GPU-emitting callbacks below. Injected by
	// State, like the Tracy context. Begin returns whether it began an event, which it does only while
	// events are wanted (a capture tool is attached, or Frame Annotations is on). A begun event also
	// opens a Tracy zone of the same name while Tracy records.
	using GpuEventBegin = bool (*)(std::string_view);
	using GpuEventEnd = void (*)();
	inline static GpuEventBegin s_beginGpuEvent = nullptr;
	inline static GpuEventEnd s_endGpuEvent = nullptr;

	inline static thread_local bool t_defaultCallbackRan = false;

	/** @brief Sets (or with nulls, clears) the debugger event callbacks used by ForEachLoadedFeature. */
	static void SetGpuEventCallbacks(GpuEventBegin a_begin, GpuEventEnd a_end) noexcept
	{
		s_beginGpuEvent = a_begin;
		s_endGpuEvent = a_end;
	}

	/**
	 * @brief Invokes a callback on every loaded feature with optional Tracy profiling.
	 * @param methodName Label for the Tracy zone (e.g. "Reset", "Prepass").
	 * @param callback Callable receiving a Feature* for each loaded feature.
	 * @param emitGpuZone When true, also emits a debugger event (Nsight, RenderDoc, PIX) named
	 * "<feature>::<method>", and a Tracy GPU zone when CS_FEATURE_GPU_ZONES=1. Only for the frame callbacks, on the render thread:
	 * a feature whose callback is the empty default is skipped after its first call, with no event or zone.
	 */
	template <typename Func>
	static inline void ForEachLoadedFeature(std::string_view methodName, Func&& callback, bool emitGpuZone = false)
	{
		const auto& features = GetFeatureList();
		const auto& owners = GetVramOwners();
		// One per call site, since each passes its own lambda type.
		static std::vector<std::uint8_t> s_defaultOnly;
		if (emitGpuZone && s_defaultOnly.size() != features.size())
			s_defaultOnly.assign(features.size(), 0);
		for (std::size_t i = 0; i < features.size(); ++i) {
			auto* feature = features[i];
			if (!feature->loaded || (emitGpuZone && s_defaultOnly[i]))
				continue;
			// What DXVK creates during the callback is the feature's memory (VRAM attribution).
			Vram::OwnerScope vramScope(owners[i]);
#ifdef TRACY_ENABLE
			const bool tracy = TracyIsConnected;
#else
			constexpr bool tracy = false;
#endif
			const bool wantEvent = emitGpuZone && s_beginGpuEvent && s_endGpuEvent;
			char name[128];
			if (wantEvent || tracy)
				FormatZoneName(name, feature->GetShortName(), methodName);
			const bool gpuEvent = wantEvent && s_beginGpuEvent(name);
#ifdef TRACY_ENABLE
			// The debugger event already opened a zone of this name.
			ZoneTransientN(___tracy_feature_zone, name, tracy && !gpuEvent);
			TracyD3D11ZoneTransientS(s_tracyCtx, ___tracy_d3d11_feature_zone, name, 0, tracy && emitGpuZone && s_tracyGpuZones && s_tracyCtx != nullptr);
#endif
			t_defaultCallbackRan = false;
			callback(feature);
			if (emitGpuZone && t_defaultCallbackRan)
				s_defaultOnly[i] = 1;
			if (gpuEvent)
				s_endGpuEvent();
		}
	}
};
