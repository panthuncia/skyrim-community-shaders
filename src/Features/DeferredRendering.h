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
	enum class DebugView : std::uint32_t
	{
		Final,
		Compatibility,
		Albedo,
		Specular,
		Reflectance,
		Normal,
		Masks,
		MaterialIdentity,
		LinearDepth,
		DirectLighting,
		AmbientLighting,
		SubsurfaceLighting
	};
	struct Settings
	{
		bool visualizeDeferredCoverage = false;
		bool classifyVisibleMaterials = false;
		DebugView debugView = DebugView::Final;
	};
	struct MaterialClassification
	{
		std::array<std::uint32_t, 256> visible{};
		std::array<std::uint32_t, 256> deferred{};
		std::uint32_t depthCoveredPixels{};
		std::uint32_t unclassifiedDepthPixels{};
		std::uint32_t enabledEvaluatorMask{};
		std::uint64_t serial{};
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
		kUsesCharacterLight = 1u << 4,
		kUsesSoftLighting = 1u << 5,
		kUsesRimLighting = 1u << 6,
		kUsesBackLighting = 1u << 7
	};

	struct FrameSnapshot
	{
		std::uint32_t abiVersion = CS::Deferred::kAbiVersion;
		std::vector<LightData> lights;
		std::vector<LightingContext> contexts;
		std::vector<CS::Deferred::PBRMaterialRecord> pbrMaterials;
		Matrix cameraView{};
		Matrix cameraViewInverse{};
		Matrix projectionInverse{};
		Matrix viewProjectionInverse{};
		float4 cameraData{};
		float4 dynamicResolutionParams2{};
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
	// Deferred rendering is foundational infrastructure on this branch. Alpha
	// status is informational and must never silently place it in Disable at Boot.
	bool IsDisabledByDefault() const override { return false; }
	bool IsInMenu() const override { return true; }
	bool CanConfigureWhileUnloaded() const override { return true; }
	void DrawSettings() override;
	void LoadSettings(json& json) override;
	void SaveSettings(json& json) override;
	void RestoreDefaultSettings() override;
	bool ToggleAtBootSetting() override;
	bool AppliesBootToggleImmediately() const override { return true; }
	bool IsRuntimeEnabled() const noexcept { return loaded && runtimeEnabled.load(std::memory_order_acquire); }
	std::uint32_t GetEnabledEvaluatorMask() const noexcept
	{
		return enabledEvaluatorMask.load(std::memory_order_acquire);
	}
	void SetEnabledEvaluatorMask(std::uint32_t mask) noexcept
	{
		enabledEvaluatorMask.store(mask, std::memory_order_release);
	}
	bool IsCoverageVisualizationEnabled() const noexcept
	{
		return settings.visualizeDeferredCoverage || std::getenv("CS_DX12_FORCE_COVERAGE") != nullptr;
	}
	bool IsMaterialClassificationEnabled() const noexcept
	{
		return settings.classifyVisibleMaterials || std::getenv("CS_DX12_CLASSIFY_MATERIALS") != nullptr;
	}
	DebugView GetDebugView() const noexcept { return settings.debugView; }
	bool IsDebugViewEnabled() const noexcept { return settings.debugView != DebugView::Final; }
	void UpdateMaterialClassification(const std::uint32_t* visible, const std::uint32_t* deferred,
		std::uint32_t evaluatorMask, std::uint32_t depthCovered, std::uint32_t unclassifiedDepth);
	MaterialClassification GetMaterialClassification() const;
	void RecordShaderSelection(RE::BSShader::Type type, std::uint32_t vertexDescriptor,
		std::uint32_t pixelDescriptor, bool insideDeferred, bool inWorld,
		bool activeReflections) noexcept;
	void RecordAppliedRenderTargets(bool isCompute) noexcept;
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Deferred rendering infrastructure and clustered-light assignment.", {} };
	}

	void BeginFrame(std::span<const LightData> lights, const std::uint32_t (&clusterDimensions)[3], float cameraNearPlane, float cameraFarPlane);
	ContextIndex AssignContext(const RE::BSRenderPass* renderPass, const LightingContext& context);
	CS::Deferred::PBRMaterialIndex AssignPBRMaterial(const RE::BSRenderPass* renderPass,
		const CS::Deferred::PBRMaterialRecord& material);
	void FinalizeFrame();
	std::shared_ptr<const FrameSnapshot> GetFrameSnapshot() const;
	void RetainSubmittedFrame(std::shared_ptr<const FrameSnapshot> frame, std::uint64_t completionValue);
	void RetireFrames(std::uint64_t completedValue);
	ContextIndex GetContext(const RE::BSRenderPass* renderPass) const;
	CS::Deferred::PBRMaterialIndex GetPBRMaterial(const RE::BSRenderPass* renderPass) const;

private:
	ContextIndex InternContext(const LightingContext& context);
	CS::Deferred::PBRMaterialIndex InternPBRMaterial(const CS::Deferred::PBRMaterialRecord& material);

	mutable std::shared_mutex snapshotMutex;
	std::vector<LightData> deferredLights;
	std::vector<LightingContext> deferredContexts;
	std::vector<CS::Deferred::PBRMaterialRecord> deferredPBRMaterials;
	eastl::hash_map<const RE::BSRenderPass*, ContextIndex> drawContexts;
	eastl::hash_map<const RE::BSRenderPass*, CS::Deferred::PBRMaterialIndex> drawPBRMaterials;
	std::uint32_t clusterSize[3]{};
	float nearPlane = 1.0f;
	float farPlane = 16384.0f;
	Matrix cameraView{};
	Matrix cameraViewInverse{};
	Matrix projectionInverse{};
	Matrix viewProjectionInverse{};
	float4 cameraData{};
	float4 dynamicResolutionParams2{};
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
	std::atomic_uint32_t enabledEvaluatorMask{ 0 };
	mutable std::mutex classificationMutex;
	MaterialClassification materialClassification;
	static constexpr std::size_t kSelectionClassCount = 64 + RE::BSShader::Type::Total;
	std::array<std::atomic_uint32_t, kSelectionClassCount> shaderSelections{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> shaderSelectionsInsideDeferred{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> shaderSelectionsWithDeferredPermutation{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> shaderSelectionsWithGBufferPermutation{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> shaderSelectionsInWorld{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> shaderSelectionsInWorldOutsideDeferred{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> shaderSelectionsInReflections{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> targetApplications{};
	std::array<std::atomic_uint32_t, kSelectionClassCount> targetApplicationsWithIdentity{};
};
#pragma warning(pop)
