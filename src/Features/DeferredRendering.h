#pragma once

#include "Feature.h"
#include "Features/DeferredRendering/DeferredTypes.h"
#include "Features/GrassLighting.h"
#include "Features/IBL.h"
#include "Features/Skylighting.h"

#pragma warning(push)
#pragma warning(disable : 4324) // Frame snapshot intentionally embeds 16-byte shader ABI records.
class DeferredRendering final : public Feature
{
public:
	struct Settings
	{
		bool visualizeDeferredCoverage = false;
	};

	Settings settings;
	using LightData = CS::Deferred::Light;
	using LightFlags = CS::Deferred::LightFlags;
	using LightingContext = CS::Deferred::LightingContext;
	using ContextIndex = CS::Deferred::ContextIndex;
	static constexpr std::uint32_t MAX_LIGHTS = CS::Deferred::kMaxLights;
	static constexpr ContextIndex INVALID_CONTEXT = CS::Deferred::kInvalidContext;
	enum ContextFeatureFlags : std::uint32_t
	{
		kWorld = 1u << 0,
		kLightingUniformsValid = 1u << 1,
		kReceivesDeferredShadow = 1u << 2,
		kReceivesDirectionalShadow = 1u << 3,
		kUsesCharacterLight = 1u << 4
	};

	struct FrameSnapshot
	{
		std::uint32_t abiVersion = CS::Deferred::kAbiVersion;
		std::vector<LightData> lights;
		std::vector<LightingContext> contexts;
		Matrix cameraView{};
		Matrix cameraViewInverse{};
		Matrix projectionInverse{};
		CS::Deferred::LightingTransform lightingTransform{};
		GrassLighting::Settings grassLighting{};
		IBL::PerFrame ibl{};
		Skylighting::SkylightingCB skylighting{};
		bool skylightingEnabled{};
		std::uint32_t renderWidth{};
		std::uint32_t renderHeight{};
		std::uint32_t clusterSize[3]{};
		float nearPlane = 1.0f;
		float farPlane = 16384.0f;
	};

	std::string GetName() override { return "Deferred Rendering"; }
	std::string GetShortName() override { return "DeferredRendering"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool IsInMenu() const override { return true; }
	bool CanConfigureWhileUnloaded() const override { return true; }
	void DrawSettings() override;
	void LoadSettings(json& json) override;
	void SaveSettings(json& json) override;
	void RestoreDefaultSettings() override;
	bool ToggleAtBootSetting() override;
	bool AppliesBootToggleImmediately() const override { return true; }
	bool IsRuntimeEnabled() const noexcept { return loaded && runtimeEnabled.load(std::memory_order_acquire); }
	bool IsCoverageVisualizationEnabled() const noexcept
	{
		return settings.visualizeDeferredCoverage || std::getenv("CS_DX12_FORCE_COVERAGE") != nullptr;
	}
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Deferred rendering infrastructure and clustered-light assignment.", {} };
	}

	void BeginFrame(std::span<const LightData> lights, const std::uint32_t (&clusterDimensions)[3], float cameraNearPlane, float cameraFarPlane);
	ContextIndex AssignContext(const RE::BSRenderPass* renderPass, const LightingContext& context);
	void FinalizeFrame();
	std::shared_ptr<const FrameSnapshot> GetFrameSnapshot() const;
	void RetainSubmittedFrame(std::shared_ptr<const FrameSnapshot> frame, std::uint64_t completionValue);
	void RetireFrames(std::uint64_t completedValue);
	ContextIndex GetContext(const RE::BSRenderPass* renderPass) const;

private:
	ContextIndex InternContext(const LightingContext& context);

	mutable std::shared_mutex snapshotMutex;
	std::vector<LightData> deferredLights;
	std::vector<LightingContext> deferredContexts;
	eastl::hash_map<const RE::BSRenderPass*, ContextIndex> drawContexts;
	std::uint32_t clusterSize[3]{};
	float nearPlane = 1.0f;
	float farPlane = 16384.0f;
	Matrix cameraView{};
	Matrix cameraViewInverse{};
	Matrix projectionInverse{};
	CS::Deferred::LightingTransform lightingTransform{};
	GrassLighting::Settings grassLighting{};
	IBL::PerFrame ibl{};
	Skylighting::SkylightingCB skylighting{};
	bool skylightingEnabled{};
	std::uint32_t renderWidth{};
	std::uint32_t renderHeight{};
	std::shared_ptr<const FrameSnapshot> finalizedFrame;
	std::deque<std::pair<std::uint64_t, std::shared_ptr<const FrameSnapshot>>> submittedFrames;
	std::atomic_bool runtimeEnabled{ false };
};
#pragma warning(pop)
