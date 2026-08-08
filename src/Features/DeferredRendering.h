#pragma once

#include "Feature.h"
#include "Features/LightLimitFix.h"

class DeferredRendering final : public Feature
{
public:
	using LightData = LightLimitFix::LightData;
	static constexpr std::uint32_t MAX_LIGHTS = LightLimitFix::MAX_LIGHTS;
	using ContextIndex = std::uint16_t;
	static constexpr ContextIndex INVALID_CONTEXT = 0xFFFF;

	struct alignas(16) LightingContext
	{
		std::int32_t roomIndex = -1;
		std::uint32_t shadowLightMembershipMask = 0;
		std::uint32_t featureFlags = 0;
		std::uint32_t reserved = 0;

		auto operator<=>(const LightingContext&) const = default;
	};
	STATIC_ASSERT_ALIGNAS_16(LightingContext);

	struct FrameSnapshot
	{
		std::vector<LightData> lights;
		std::vector<LightingContext> contexts;
		std::uint32_t clusterSize[3]{};
		float nearPlane = 1.0f;
		float farPlane = 16384.0f;
	};

	std::string GetName() override { return "Deferred Rendering"; }
	std::string GetShortName() override { return "DeferredRendering"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool IsInMenu() const override { return false; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Deferred rendering infrastructure and clustered-light assignment.", {} };
	}

	void BeginFrame(std::span<const LightData> lights, const std::uint32_t (&clusterDimensions)[3], float cameraNearPlane, float cameraFarPlane);
	ContextIndex AssignContext(const RE::BSRenderPass* renderPass, const LightingContext& context);
	FrameSnapshot GetFrameSnapshot() const;
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
};
