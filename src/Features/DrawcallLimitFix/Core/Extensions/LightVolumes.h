#pragma once

// The light volumes contract's Core side (Api/LightVolumes.h): the contribution's entry, resolved once at InstallFeatures (none
// registered: no volumes, no landscape rule, no point light table), and the sink its sample fills: the colour commit's GpuShadowVolume
// list.

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
	 * @brief Where a light volumes sample puts its volumes: the colour commit's list (IndirectDraws::Impl::shadowVolumes, cleared before
	 * the sample), a GpuShadowVolume each, in the order put. The list is Core's storage: no allocation per commit once grown.
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
	/** @brief The contribution's entry (registered false: none, every field its default). */
	struct LightVolumesEntry
	{
		bool registered = false;
		std::string name;
		std::uint64_t landscapeFlags = 0;  // per join, data: the properties taking only affectsLand volumes
		decltype(Api::LightVolumesContribution::sample) sample = nullptr;
		decltype(Api::LightVolumesContribution::pointLightsActive) pointLightsActive = nullptr;
		decltype(Api::LightVolumesContribution::pointLights) pointLights = nullptr;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no name: none). Once. */
	void ResolveLightVolumes(const Api::LightVolumesContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (unregistered before): the contribution's entry. */
	const LightVolumesEntry& LightVolumes();
	/**
	 * @brief Render thread, once per colour commit (CommitShadowInputs): the frame's volumes into a_sink (the contribution's sample);
	 * nothing without one.
	 */
	void SampleLightVolumes(Api::LightVolumeSink& a_sink);
	/** @brief Any thread, after InstallFeatures: whether the Lighting draws read the frame's point light table (the contribution's). */
	bool PointLightsActive();
	/** @brief Render thread, once per commit while PointLightsActive: the frame's point lights into a_out (at most a_capacity); how many. */
	std::uint32_t SamplePointLights(Api::PointLight* a_out, std::uint32_t a_capacity);
}
