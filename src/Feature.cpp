#include "Feature.h"

#include "CSEditor/SceneManager/SceneManager.h"
#include "FeatureIssues.h"
#include "FeatureVersions.h"
#include "Features/CSEditor.h"
#include "Features/CloudShadows.h"
#include "Features/DeveloperTools.h"
#include "Features/GraphicsAPI.h"
#include "Features/DynamicCubemaps.h"
#include "Features/Effects11.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/GrassCollision.h"
#include "Features/GrassLighting.h"
#include "Features/GrassOptimizations.h"
#include "Features/HDRDisplay.h"
#include "Features/HairSpecular.h"
#include "Features/HorizonFix.h"
#include "Features/IBL.h"
#include "Features/InteriorSun.h"
#include "Features/LODBlending.h"
#include "Features/DrawcallLimitFix.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PerformanceOverlay.h"
#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/ProceduralAurora.h"
#include "Features/ProceduralSun.h"
#include "Features/RainRefraction.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Skin.h"
#include "Features/SkySync.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainHelper.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/TreeWind.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/ViewAtmosphere.h"
#include "Features/VolumetricLighting.h"
#include "Features/VolumetricShadows.h"
#include "Features/TextureStreaming.h"
#include "Features/VRAMBudget.h"
#include "Features/WaterEffects.h"
#include "Features/WetnessEffects.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Utils/Format.h"
#include "Vram/Attribution.h"

#include "RenderGraph/Frame/Variables.h"
#include "State.h"
#include "TruePBR.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

void Feature::Load(json& o_json)
{
	// Convert string to wstring
	auto ini_filename = std::format("{}.ini", GetShortName());
	std::wstring ini_filename_w;
	std::ranges::copy(ini_filename, std::back_inserter(ini_filename_w));
	auto ini_path = L"Data\\Shaders\\Features\\" + ini_filename_w;

	CSimpleIniA ini;
	ini.SetUnicode();
	SI_Error rc = ini.LoadFile(ini_path.c_str());

	if (rc < 0) {
		if (!FeatureIssues::IsObsoleteFeature(GetShortName()))
			logger::info("{} failed to load, feature disabled", ini_filename);
		loaded = false;
		return;
	}

	bool hasError = false;
	std::string errorVersion;
	FeatureIssues::FeatureIssueInfo::IssueType errorType = FeatureIssues::FeatureIssueInfo::IssueType::UNKNOWN;

	if (FeatureIssues::IsObsoleteFeature(GetShortName())) {
		hasError = true;
		errorVersion = "N/A";
		errorType = FeatureIssues::FeatureIssueInfo::IssueType::OBSOLETE;
		failedLoadedMessage = std::format("{} is an obsolete feature that has been removed", GetShortName());
	} else if (auto value = ini.GetValue("Info", "Version")) {
		try {
			REL::Version featureVersion(std::regex_replace(value, std::regex("-"), "."));

			// Check if feature exists in minimal versions
			REL::Version minimalFeatureVersion;
			if (!Feature::IsFeatureKnown(GetShortName(), &minimalFeatureVersion)) {
				hasError = true;
				errorVersion = value;
				errorType = FeatureIssues::FeatureIssueInfo::IssueType::UNKNOWN;
				failedLoadedMessage = std::format("{} {} is an unknown feature not supported by this CS version. This may be a feature from a development branch.", GetShortName(), value);
			} else {
				// Version compatibility check
				bool oldFeature = featureVersion.compare(minimalFeatureVersion) == std::strong_ordering::less;
				bool majorVersionMismatch = featureVersion.major() < minimalFeatureVersion.major();

				if (!oldFeature && !majorVersionMismatch) {
					loaded = true;
					logger::info("{} {} successfully loaded", ini_filename, value);
				} else {
					hasError = true;
					errorVersion = value;
					errorType = FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH;

					std::string minimalVersionString = Util::GetFormattedVersion(minimalFeatureVersion);

					if (IsCore()) {
						failedLoadedMessage = std::format("This feature is already included as part of the core Community Shaders installation. Uninstall this feature with your mod manager.");
					} else if (majorVersionMismatch) {
						failedLoadedMessage = std::format("{} {} is too old, major version incompatibility detected. Required: {}", GetShortName(), value, minimalVersionString);
					} else {
						failedLoadedMessage = std::format("{} {} is an old feature version, required: {}", GetShortName(), value, minimalVersionString);
					}
				}
			}

			version = value;
		} catch (const std::exception& e) {
			hasError = true;
			errorVersion = value;
			errorType = FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH;
			failedLoadedMessage = std::format("{} {} has invalid version format: {}", GetShortName(), value, e.what());
		}
	} else {
		hasError = true;
		errorVersion = "unknown";
		errorType = FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH;

		// Get the minimum required version to include in the error message
		std::string requiredVersion = Feature::GetFeatureRequiredVersion(GetShortName());

		failedLoadedMessage = std::format("The {} file is missing. This feature is not installed! Version required: {}", ini_filename, requiredVersion);
	}

	if (hasError) {
		loaded = false;
		logger::warn("{}", failedLoadedMessage);

		// Guard against empty shortName to prevent bogus filesystem access
		std::string shortName = GetShortName();
		if (!shortName.empty()) {
			FeatureIssues::FeatureFileInfo fileInfo = FeatureIssues::GetFeatureFileInfo(shortName);

			// For version mismatch, also pass the minimum required version
			std::string minimumVersion;
			if (errorType == FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH) {
				minimumVersion = Feature::GetFeatureRequiredVersion(shortName);
			}

			FeatureIssues::AddFeatureIssue(shortName, errorVersion, failedLoadedMessage, errorType, fileInfo, minimumVersion);

		} else {
			logger::error("Feature has empty short name, cannot add to feature issues list");
		}
	} else {
		if (!UsesMainSettings())
			return;

		// No errors, load settings now
		if (o_json[GetName()].is_structured()) {
			logger::info("Loading {} settings", GetName());
			try {
				ApplySettings(o_json[GetName()], "Feature::Load");
			} catch (...) {
				// Plain defaults would be saved back as the user's edits, so the layered base is tried first.
				logger::warn("Invalid settings for {}, using its defaults and Baseline packs.", GetName());
				ApplyDefaults("Feature::Load");
				if (const auto* base = globals::state->GetFeatureBase(GetName())) {
					try {
						auto baseSettings = *base;
						ApplySettings(baseSettings, "Feature::Load");
					} catch (...) {
						logger::warn("Invalid Baseline settings for {}, using default.", GetName());
						ApplyDefaults("Feature::Load");
					}
				}
			}
		} else {
			logger::info("Loading default settings for {}", GetName());
			ApplyDefaults("Feature::Load");
		}
	}
}

void Feature::Save(json& o_json)
{
	if (UsesMainSettings())
		SaveSettings(o_json[GetName()]);
}

/**
 * @brief Provides access to the registry of all known features.
 * @return A constant reference to the vector of all known feature instances.
 */
const std::vector<Feature*>& Feature::GetFeatureList()
{
	static std::vector<Feature*> features = {
		&globals::features::truePBR,
		&globals::features::volumetricShadows,
		&globals::features::grassLighting,
		&globals::features::grassCollision,
		&globals::features::grassOptimizations,
		&globals::features::screenSpaceShadows,
		&globals::features::extendedMaterials,
		&globals::features::wetnessEffects,
		&globals::features::lightLimitFix,
		&globals::features::drawcallLimitFix,
		&globals::features::dynamicCubemaps,
		&globals::features::cloudShadows,
		&globals::features::waterEffects,
		&globals::features::performanceOverlay,
		&globals::features::subsurfaceScattering,
		&globals::features::terrainShadows,
		&globals::features::screenSpaceGI,
		&globals::features::skylighting,
		&globals::features::skySync,
		&globals::features::terrainBlending,
		&globals::features::terrainHelper,
		&globals::features::volumetricLighting,
		&globals::features::viewAtmosphere,
		&globals::features::lodBlending,
		&globals::features::hairSpecular,
		&globals::features::interiorSun,
		&globals::features::proceduralSun,
		&globals::features::terrainVariation,
		&globals::features::ibl,
		&globals::features::extendedTranslucency,
		&globals::features::upscaling,
		&globals::features::developerTools,
		&globals::features::graphicsAPI,
		&globals::features::csEditor,
		&globals::features::sceneManager,
		&globals::features::screenshotFeature,
		&globals::features::linearLighting,
		&globals::features::effects11,
		&globals::features::rainRefraction,
		&globals::features::unifiedWater,
		&globals::features::horizonFix,
		&globals::features::treeWind,
		&globals::features::vramBudget,
		&globals::features::textureStreaming,
		&globals::features::exponentialHeightFog,
		&globals::features::hdrDisplay,
		&globals::features::physicalSky,
		&globals::features::skin,
		&globals::features::postProcessing,
		&globals::features::proceduralAurora
	};

	return features;
}

Feature* Feature::FindFeatureByShortName(const std::string& shortName)
{
	// GetShortName() returns by value, so index once rather than allocating per feature per call.
	static const auto featuresByShortName = [] {
		std::unordered_map<std::string, Feature*> index;
		for (auto* feature : GetFeatureList())
			index.emplace(feature->GetShortName(), feature);
		return index;
	}();

	const auto it = featuresByShortName.find(shortName);
	return it != featuresByShortName.end() && it->second->loaded ? it->second : nullptr;
}

std::vector<std::string> Feature::GetLoadedFeatureNames()
{
	std::vector<std::string> names;
	for (auto* feature : GetFeatureList()) {
		if (feature->loaded && feature->IsInMenu())
			names.push_back(feature->GetShortName());
	}
	std::sort(names.begin(), names.end());
	return names;
}

bool Feature::ToggleAtBootSetting()
{
	if (IsAlwaysEnabled())
		return false;
	auto state = globals::state;
	const std::string featureName = GetShortName();
	auto disabled = state->IsFeatureDisabled(featureName);
	state->SetFeatureDisabled(featureName, !disabled);
	state->CheckCoreSettingsChanged();

	return state->IsFeatureDisabled(featureName);  // Return the new state
}

const char* Feature::GetVramOwner()
{
	// Cached outside the class: a member would change every feature's layout (and padding). Read on every feature callback.
	static std::shared_mutex mutex;
	static std::unordered_map<const Feature*, const char*> owners;
	{
		std::shared_lock lock(mutex);
		if (const auto it = owners.find(this); it != owners.end())
			return it->second;
	}
	const char* owner = Vram::FeatureOwner(GetShortName());
	std::unique_lock lock(mutex);
	return owners.emplace(this, owner).first->second;
}

void Feature::DrawUnloadedUI()
{
	// Prioritize detailed failure message if available
	if (!failedLoadedMessage.empty()) {
		// Use error color for all failure messages
		auto& themeSettings = Menu::GetSingleton()->GetTheme();
		ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", failedLoadedMessage.c_str());
		return;
	}

	// Fallback: Always show missing file message when no specific failure message exists
	auto& themeSettings = Menu::GetSingleton()->GetTheme();
	auto ini_filename = std::format("{}.ini", GetShortName());
	// Get the minimum required version to include in the error message
	std::string requiredVersion = Feature::GetFeatureRequiredVersion(GetShortName());

	auto missingFileMessage = std::format("The {} file is missing. This feature is not installed! Version required: {}", ini_filename, requiredVersion);
	ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", missingFileMessage.c_str());

	// Also show feature summary if available
	auto [description, keyFeatures] = GetFeatureSummary();
	if (!description.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", description.c_str());
	}

	if (!keyFeatures.empty()) {
		if (description.empty()) {
			ImGui::Spacing();
		}
		ImGui::TextWrapped("%s", T("feature.key_features", "Key features:"));
		for (const auto& feature : keyFeatures) {
			ImGui::BulletText("%s", feature.c_str());
		}
	}
}

std::string Feature::GetFeatureRequiredVersion(const std::string& shortName)
{
	if (shortName.empty()) {
		return "unknown";
	}
	auto iter = FeatureVersions::FEATURE_MINIMAL_VERSIONS.find(shortName);
	if (iter != FeatureVersions::FEATURE_MINIMAL_VERSIONS.end()) {
		return Util::GetFormattedVersion(iter->second);
	}

	return "unknown";
}

bool Feature::IsFeatureKnown(const std::string& shortName, REL::Version* outVersion)
{
	if (shortName.empty()) {
		return false;
	}

	auto iter = FeatureVersions::FEATURE_MINIMAL_VERSIONS.find(shortName);
	if (iter != FeatureVersions::FEATURE_MINIMAL_VERSIONS.end()) {
		if (outVersion) {
			*outVersion = iter->second;
		}
		return true;
	}

	return false;
}

namespace
{
	/** @brief What the settings events know of one feature. */
	struct FeatureSettingsRecord
	{
		Feature::SizeProjection raised;  // the projection raised last, or the baseline
		bool baseline = false;           // raised holds a projection
		std::uint32_t pending = 0;       // writes held back by open batches
		bool differed = false;           // the last check saw a difference
		std::vector<std::string_view> reported;  // fields the check reported
	};

	struct FeatureSettingsRecords
	{
		std::mutex mutex;
		std::unordered_map<const Feature*, FeatureSettingsRecord> records;
	};

	FeatureSettingsRecords& FeatureSettingsEventRecords()
	{
		static FeatureSettingsRecords records;
		return records;
	}

	thread_local std::uint32_t featureSettingsBatchDepth = 0;
	thread_local std::vector<Feature*> featureSettingsBatchWrites;

	std::string ProjectionValueText(const Feature::SizeProjection::Field* a_field)
	{
		if (!a_field)
			return "none";
		if (!a_field->text.empty())
			return a_field->text;
		if (a_field->real != 0.0)
			return std::format("{}", a_field->real);
		return std::format("{}", a_field->integer);
	}

	const Feature::SizeProjection::Field* FindProjectionField(const Feature::SizeProjection& a_projection, std::string_view a_name)
	{
		const auto it = std::ranges::find(a_projection.fields, a_name, &Feature::SizeProjection::Field::name);
		return it == a_projection.fields.end() ? nullptr : &*it;
	}

	/** @brief The names of the fields that differ between two projections, a field missing from one included. */
	std::vector<std::string_view> ChangedProjectionFields(const Feature::SizeProjection& a_old, const Feature::SizeProjection& a_new)
	{
		std::vector<std::string_view> changed;
		for (const auto& field : a_new.fields)
			if (const auto* old = FindProjectionField(a_old, field.name); !old || !(*old == field))
				changed.push_back(field.name);
		for (const auto& field : a_old.fields)
			if (!FindProjectionField(a_new, field.name))
				changed.push_back(field.name);
		return changed;
	}

	/** @brief Raises the feature's event when its projection changed since the last raise. */
	void RaiseIfProjectionChanged(Feature& a_feature, std::string_view a_writer)
	{
		auto projection = a_feature.ProjectSizeSettings();
		std::string change;
		{
			auto& records = FeatureSettingsEventRecords();
			const std::lock_guard lock(records.mutex);
			auto& record = records.records[&a_feature];
			if (!std::exchange(record.baseline, true)) {
				record.raised = std::move(projection);
				return;
			}
			if (record.raised == projection)
				return;
			for (const auto name : ChangedProjectionFields(record.raised, projection))
				change += std::format("{}{} {} -> {}", change.empty() ? "" : ", ", name, ProjectionValueText(FindProjectionField(record.raised, name)),
					ProjectionValueText(FindProjectionField(projection, name)));
			record.raised = std::move(projection);
			record.differed = false;
		}
		RenderGraph::Frame::Vars::RaiseFeatureSettings(a_feature.GetShortName(), std::format("{}, {}", a_writer, change));
	}
}

void Feature::SettingsWritten(std::string_view a_writer)
{
	if (featureSettingsBatchDepth > 0) {
		if (std::ranges::contains(featureSettingsBatchWrites, this))
			return;
		featureSettingsBatchWrites.push_back(this);
		auto& records = FeatureSettingsEventRecords();
		const std::lock_guard lock(records.mutex);
		++records.records[this].pending;
		return;
	}
	RaiseIfProjectionChanged(*this, a_writer);
}

void Feature::ApplySettings(json& a_json, std::string_view a_writer)
{
	// A load that throws may have written part of the settings.
	try {
		LoadSettings(a_json);
	} catch (...) {
		SettingsWritten(a_writer);
		throw;
	}
	SettingsWritten(a_writer);
}

void Feature::ApplyDefaults(std::string_view a_writer)
{
	try {
		RestoreDefaultSettings();
	} catch (...) {
		SettingsWritten(a_writer);
		throw;
	}
	SettingsWritten(a_writer);
}

Feature::SettingsWriteBatch::SettingsWriteBatch(std::string_view a_writer) :
	writer(a_writer)
{
	++featureSettingsBatchDepth;
}

Feature::SettingsWriteBatch::~SettingsWriteBatch()
{
	if (--featureSettingsBatchDepth > 0)
		return;
	for (auto* feature : std::exchange(featureSettingsBatchWrites, {})) {
		{
			auto& records = FeatureSettingsEventRecords();
			const std::lock_guard lock(records.mutex);
			--records.records[feature].pending;
		}
		try {
			RaiseIfProjectionChanged(*feature, writer);
		} catch (const std::exception& e) {
			logger::error("[ORG] settings event for {} failed: {}", feature->GetShortName(), e.what());
		}
	}
}

std::vector<std::string> Feature::FindMissedSettingsWriters()
{
	const auto& features = GetFeatureList();
	std::vector<SizeProjection> projections;
	projections.reserve(features.size());
	for (const auto* feature : features)
		projections.push_back(feature->ProjectSizeSettings());

	std::vector<std::pair<Feature*, std::vector<std::string_view>>> found;
	{
		auto& records = FeatureSettingsEventRecords();
		const std::lock_guard lock(records.mutex);
		for (std::size_t i = 0; i < features.size(); ++i) {
			auto& record = records.records[features[i]];
			if (!std::exchange(record.baseline, true)) {
				record.raised = std::move(projections[i]);
				continue;
			}
			if (record.pending > 0 || record.raised == projections[i]) {
				record.differed = false;
				continue;
			}
			if (!std::exchange(record.differed, true))
				continue;
			std::vector<std::string_view> fields;
			for (const auto name : ChangedProjectionFields(record.raised, projections[i]))
				if (!std::ranges::contains(record.reported, name)) {
					record.reported.push_back(name);
					fields.push_back(name);
				}
			if (!fields.empty())
				found.emplace_back(features[i], std::move(fields));
		}
	}

	std::vector<std::string> missed;
	for (auto& [feature, fields] : found)
		for (const auto name : fields)
			missed.push_back(std::format("{}.{}", feature->GetShortName(), name));
	return missed;
}
