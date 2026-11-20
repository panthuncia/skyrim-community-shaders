#include "RenderGraph/Frame/Variables.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

#	include "RenderGraph/Frame/VariablesLane.h"

#	include <ORGModuleServices/Variables.h>

#	include <cstring>
#	include <format>
#	include <functional>
#	include <map>
#	include <memory>
#	include <mutex>
#	include <string>

namespace RenderGraph::Frame::Vars
{
	namespace
	{
		using org::services::Variable;

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

		struct FrameVariables
		{
			org::services::VariableSet set;
			Variable<Extent2D> displaySize = set.Declare("DisplaySize", Extent2D{});
			// Decision 6a: render size and ratios only reach constants and viewports.
			Variable<Extent2D> renderSize = set.Declare("RenderSize", Extent2D{}, { .sizes = false });
			Variable<Ratios> dynamicRatios = set.Declare("DynamicRatios", Ratios{}, { .sizes = false });
			Variable<Upscaler> upscaleMethod = set.Declare("UpscaleMethod", Upscaler{});
			Variable<bool> hdrEnabled = set.Declare("HdrEnabled", false);
			Variable<TargetIdentity> hdrOutputTarget = set.Declare("HdrOutputTarget", TargetIdentity{});
			Variable<ShadowMapShape> engineShadowMap = set.Declare("EngineShadowMap", ShadowMapShape{});
			Variable<ShadowMapShape> shadowCascades = set.Declare("ShadowCascades", ShadowMapShape{});
			Variable<CubemapShape> reflectionCubemap = set.Declare("ReflectionCubemap", CubemapShape{});
			Variable<HeightMapIdentity> heightMap = set.Declare("HeightMap", HeightMapIdentity{});
			Variable<SurfaceFormat> presenterFormat = set.Declare("PresenterFormat", SurfaceFormat{});
			Variable<UpscalerOptions> upscalerOptions = set.Declare("UpscalerOptions", UpscalerOptions{});
			Variable<ImageSpaceShape> engineImageSpace = set.Declare("EngineImageSpace", ImageSpaceShape{});
			Variable<FrameGen::PresentationConfig> presentationConfig = set.Declare("PresentationConfig", FrameGen::PresentationConfig{});
			Variable<bool> interior = set.Declare("Interior", false);
			// Sizes nothing: a reload changes pipelines only.
			Variable<std::uint64_t> shaderGeneration = set.Declare("ShaderGeneration", std::uint64_t{}, { .sizes = false });

			std::mutex eventsMutex;
			std::map<std::string, VariableEventCounts, std::less<>> events;

			// The serial variables declared on first use.
			std::mutex serialMutex;
			std::map<std::string, Variable<std::uint64_t>, std::less<>> features;
			std::map<std::uint32_t, SettingVariable> settings;

			// Render thread.
			std::shared_ptr<const org::services::VariableSnapshot> adopted;
		};

		FrameVariables& FrameVariableSet()
		{
			// Never destroyed: writers may run during process teardown.
			static auto* variables = new FrameVariables;
			return *variables;
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

		template <class T>
		bool SetFrameVariable(Variable<T> a_variable, const T& a_value, std::string_view a_name, std::string_view a_writer)
		{
			auto& set = FrameVariableSet().set;
			const T old = set.Get(a_variable);
			if (old == a_value || !set.Set(a_variable, a_value))
				return false;
			NoteVariableEvent(a_name, [&] { return std::format("{} -> {}", old, a_value); }, a_writer);
			return true;
		}

		template <class T>
		T AdoptedFrameValue(Variable<T> a_variable)
		{
			// Every frame-wide variable is declared with T{}.
			const auto& adopted = FrameVariableSet().adopted;
			return adopted ? adopted->Get(a_variable) : T{};
		}

		/** @brief Bumps a serial variable. Under serialMutex. Returns the new serial. */
		std::uint64_t BumpSerial(Variable<std::uint64_t> a_variable)
		{
			auto& set = FrameVariableSet().set;
			const std::uint64_t next = set.Get(a_variable) + 1;
			set.Set(a_variable, next);
			return next;
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

	bool SetDisplaySize(Extent2D a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().displaySize, a_value, "DisplaySize", a_writer); }
	bool SetRenderSize(Extent2D a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().renderSize, a_value, "RenderSize", a_writer); }
	bool SetDynamicRatios(Ratios a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().dynamicRatios, a_value, "DynamicRatios", a_writer); }
	bool SetUpscaleMethod(Upscaler a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().upscaleMethod, a_value, "UpscaleMethod", a_writer); }
	bool SetHdrEnabled(bool a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().hdrEnabled, a_value, "HdrEnabled", a_writer); }
	bool SetHdrOutputTarget(TargetIdentity a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().hdrOutputTarget, a_value, "HdrOutputTarget", a_writer); }
	bool SetEngineShadowMap(ShadowMapShape a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().engineShadowMap, a_value, "EngineShadowMap", a_writer); }
	bool SetShadowCascades(ShadowMapShape a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().shadowCascades, a_value, "ShadowCascades", a_writer); }
	bool SetReflectionCubemap(CubemapShape a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().reflectionCubemap, a_value, "ReflectionCubemap", a_writer); }
	bool SetHeightMap(HeightMapIdentity a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().heightMap, a_value, "HeightMap", a_writer); }
	bool SetPresenterFormat(SurfaceFormat a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().presenterFormat, a_value, "PresenterFormat", a_writer); }
	bool SetUpscalerOptions(UpscalerOptions a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().upscalerOptions, a_value, "UpscalerOptions", a_writer); }
	bool SetInterior(bool a_value, std::string_view a_writer) { return SetFrameVariable(FrameVariableSet().interior, a_value, "Interior", a_writer); }
	bool SetShaderGeneration(std::uint64_t a_value, std::string_view a_writer)
	{
		return SetFrameVariable(FrameVariableSet().shaderGeneration, a_value, "ShaderGeneration", a_writer);
	}
	bool SetEngineImageSpace(const ImageSpaceShape& a_value, std::string_view a_writer)
	{
		return SetFrameVariable(FrameVariableSet().engineImageSpace, a_value, "EngineImageSpace", a_writer);
	}
	bool SetPresentationConfig(const FrameGen::PresentationConfig& a_value, std::string_view a_writer)
	{
		return SetFrameVariable(FrameVariableSet().presentationConfig, a_value, "PresentationConfig", a_writer);
	}
	FrameGen::PresentationConfig WrittenPresentationConfig() { return FrameVariableSet().set.Get(FrameVariableSet().presentationConfig); }

	Extent2D AdoptedDisplaySize() { return AdoptedFrameValue(FrameVariableSet().displaySize); }
	Extent2D AdoptedRenderSize() { return AdoptedFrameValue(FrameVariableSet().renderSize); }
	Ratios AdoptedDynamicRatios() { return AdoptedFrameValue(FrameVariableSet().dynamicRatios); }
	Upscaler AdoptedUpscaleMethod() { return AdoptedFrameValue(FrameVariableSet().upscaleMethod); }
	bool AdoptedHdrEnabled() { return AdoptedFrameValue(FrameVariableSet().hdrEnabled); }
	TargetIdentity AdoptedHdrOutputTarget() { return AdoptedFrameValue(FrameVariableSet().hdrOutputTarget); }
	ShadowMapShape AdoptedEngineShadowMap() { return AdoptedFrameValue(FrameVariableSet().engineShadowMap); }
	CubemapShape AdoptedReflectionCubemap() { return AdoptedFrameValue(FrameVariableSet().reflectionCubemap); }
	HeightMapIdentity AdoptedHeightMap() { return AdoptedFrameValue(FrameVariableSet().heightMap); }
	SurfaceFormat AdoptedPresenterFormat() { return AdoptedFrameValue(FrameVariableSet().presenterFormat); }
	UpscalerOptions AdoptedUpscalerOptions() { return AdoptedFrameValue(FrameVariableSet().upscalerOptions); }
	std::uint64_t AdoptedShaderGeneration() { return AdoptedFrameValue(FrameVariableSet().shaderGeneration); }
	FrameGen::PresentationConfig AdoptedPresentationConfig() { return AdoptedFrameValue(FrameVariableSet().presentationConfig); }

	void RaiseFeatureSettings(std::string_view a_feature, std::string_view a_writer)
	{
		auto& variables = FrameVariableSet();
		const std::string name = fmt::format("FeatureSettings.{}", a_feature);
		std::uint64_t serial = 0;
		{
			const std::lock_guard lock(variables.serialMutex);
			auto it = variables.features.find(a_feature);
			if (it == variables.features.end())
				it = variables.features.emplace(std::string(a_feature), variables.set.Declare(name, std::uint64_t{ 0 })).first;
			serial = BumpSerial(it->second);
		}
		NoteVariableEvent(name, [&] { return std::format("{} -> {}", serial - 1, serial); }, a_writer);
	}

	std::uint64_t FeatureSettingsEvents(std::string_view a_feature)
	{
		auto& variables = FrameVariableSet();
		const std::lock_guard lock(variables.serialMutex);
		const auto it = variables.features.find(a_feature);
		return it == variables.features.end() ? 0 : variables.set.Get(it->second);
	}

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
			}
			return it->second;
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
		std::shared_ptr<const org::services::VariableSnapshot> Take()
		{
			return FrameVariableSet().set.Take();
		}

		bool ConstantsOnly(std::uint32_t a_index)
		{
			// The variables declared with { .sizes = false } above. A derived variable of one is refused by the set unless it is
			// constants only too, and none is declared.
			const auto& variables = FrameVariableSet();
			return a_index == variables.renderSize.index || a_index == variables.dynamicRatios.index || a_index == variables.shaderGeneration.index;
		}

		const std::shared_ptr<const org::services::VariableSnapshot>& Adopted()
		{
			return FrameVariableSet().adopted;
		}

		void Adopt(std::shared_ptr<const org::services::VariableSnapshot> a_snapshot)
		{
			FrameVariableSet().adopted = std::move(a_snapshot);
		}

		const org::services::VariableSet& Set()
		{
			return FrameVariableSet().set;
		}

		std::uint32_t DeriveUint(std::string a_name, std::uint32_t a_dependency, std::function<std::uint32_t(const org::services::VariableSnapshot&)> a_fn)
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

		FrameGen::PresentationConfig PresentationConfigIn(const org::services::VariableSnapshot& a_values)
		{
			return a_values.Get(FrameVariableSet().presentationConfig);
		}

		std::string ChangedNames(const org::services::VariableSnapshot& a_newer, const org::services::VariableSnapshot* a_older)
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

#else

#	include <atomic>
#	include <mutex>
#	include <utility>

namespace RenderGraph::Frame::Vars
{
	namespace
	{
		// Without revisions the values read back are the newest written ones.
		std::atomic<bool> g_stubHdrEnabled{ false };
		std::mutex g_stubPresentationMutex;
		FrameGen::PresentationConfig g_stubPresentation{};
	}

	bool SetDisplaySize(Extent2D, std::string_view) { return false; }
	bool SetRenderSize(Extent2D, std::string_view) { return false; }
	bool SetDynamicRatios(Ratios, std::string_view) { return false; }
	bool SetUpscaleMethod(Upscaler, std::string_view) { return false; }
	bool SetHdrEnabled(bool a_value, std::string_view) { return g_stubHdrEnabled.exchange(a_value) != a_value; }
	bool SetHdrOutputTarget(TargetIdentity, std::string_view) { return false; }
	bool SetEngineShadowMap(ShadowMapShape, std::string_view) { return false; }
	bool SetShadowCascades(ShadowMapShape, std::string_view) { return false; }
	bool SetReflectionCubemap(CubemapShape, std::string_view) { return false; }
	bool SetHeightMap(HeightMapIdentity, std::string_view) { return false; }
	bool SetPresenterFormat(SurfaceFormat, std::string_view) { return false; }
	bool SetUpscalerOptions(UpscalerOptions, std::string_view) { return false; }
	bool SetInterior(bool, std::string_view) { return false; }
	bool SetShaderGeneration(std::uint64_t, std::string_view) { return false; }
	bool SetEngineImageSpace(const ImageSpaceShape&, std::string_view) { return false; }
	bool SetPresentationConfig(const FrameGen::PresentationConfig& a_value, std::string_view)
	{
		const std::lock_guard lock(g_stubPresentationMutex);
		return std::exchange(g_stubPresentation, a_value) != a_value;
	}
	FrameGen::PresentationConfig WrittenPresentationConfig()
	{
		const std::lock_guard lock(g_stubPresentationMutex);
		return g_stubPresentation;
	}

	Extent2D AdoptedDisplaySize() { return {}; }
	Extent2D AdoptedRenderSize() { return {}; }
	Ratios AdoptedDynamicRatios() { return {}; }
	Upscaler AdoptedUpscaleMethod() { return {}; }
	bool AdoptedHdrEnabled() { return g_stubHdrEnabled.load(); }
	TargetIdentity AdoptedHdrOutputTarget() { return {}; }
	ShadowMapShape AdoptedEngineShadowMap() { return {}; }
	CubemapShape AdoptedReflectionCubemap() { return {}; }
	HeightMapIdentity AdoptedHeightMap() { return {}; }
	SurfaceFormat AdoptedPresenterFormat() { return {}; }
	UpscalerOptions AdoptedUpscalerOptions() { return {}; }
	std::uint64_t AdoptedShaderGeneration() { return 0; }
	FrameGen::PresentationConfig AdoptedPresentationConfig() { return WrittenPresentationConfig(); }

	void RaiseFeatureSettings(std::string_view, std::string_view) {}
	std::uint64_t FeatureSettingsEvents(std::string_view) { return 0; }
	void RaiseSetting(std::uint32_t, std::string_view, const void*, std::size_t) {}
	void LogStats() {}
}

#endif
