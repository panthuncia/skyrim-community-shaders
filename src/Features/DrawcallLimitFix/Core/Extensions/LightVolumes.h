#pragma once

// Core's side of Api/LightVolumes.h: the contribution's entry and the sink its sample fills.
// Without a contribution there are no volumes, no landscape rule and no point light table.

#include "Features/DrawcallLimitFix/Api/LightVolumes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Draws
{
	struct GpuShadowVolume;
}

namespace DCLF::Api
{
	/**
	 * @brief Where a sample puts its volumes: the colour commit's list, one GpuShadowVolume per put, in order.
	 * Core reuses the storage, so nothing allocates per commit once grown.
	 */
	class LightVolumeSink
	{
	public:
		explicit LightVolumeSink(std::vector<Draws::GpuShadowVolume>& a_out) :
			out(a_out) {}

		void Put(const LightVolume& a_volume);

	private:
		std::vector<Draws::GpuShadowVolume>& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry. Every field keeps its default when none is registered. */
	struct LightVolumesEntry
	{
		bool registered = false;
		std::string name;
		std::uint64_t landscapeFlags = 0;  // properties that take only affectsLand volumes
		decltype(Api::LightVolumesContribution::sample) sample = nullptr;
		decltype(Api::LightVolumesContribution::pointLightsActive) pointLightsActive = nullptr;
		decltype(Api::LightVolumesContribution::pointLights) pointLights = nullptr;
	};

	/** @brief Main thread, once, at the end of InstallFeatures. */
	void ResolveLightVolumes(const Api::LightVolumesContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const LightVolumesEntry& LightVolumes();
	/** @brief Render thread, once per colour commit. The frame's volumes, or nothing without a contribution. */
	void SampleLightVolumes(Api::LightVolumeSink& a_sink);
	/** @brief Any thread, after InstallFeatures. Whether Lighting draws read the point light table. */
	bool PointLightsActive();
	/** @brief Render thread, per commit while PointLightsActive. Writes at most a_capacity point lights and returns how many. */
	std::uint32_t SamplePointLights(Api::PointLight* a_out, std::uint32_t a_capacity);
}
