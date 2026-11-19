#pragma once

// Core's side of Api/SunVolume.h: the contribution's entry and the sinks its callbacks fill.
// Each value is written from Core's ShadowCameras math exactly as the latch and the shadow inputs read it.

#include "Features/DrawcallLimitFix/Api/SunVolume.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Draws
{
	struct GpuCascade;
	struct SunEntryProcess;
}

namespace DCLF::ShadowCameras
{
	struct Camera;
	struct Planes;
}

namespace DCLF::Api
{
	/**
	 * @brief Where cascades go: the colour commit's list, one GpuCascade per put, in order. Planes 0-5 are the cull, 6-11 the caster volume.
	 * Core reuses the storage, so nothing allocates per commit once grown.
	 */
	class SunCascadeSink
	{
	public:
		explicit SunCascadeSink(std::vector<Draws::GpuCascade>& a_out) :
			out(a_out) {}

		void Put(const ShadowCameras::Camera& a_cascade);

	private:
		std::vector<Draws::GpuCascade>& out;
	};

	/** @brief Where the full-frustum planes go, one SunEntryProcess each, in order. */
	class SunEntryPlanesSink
	{
	public:
		explicit SunEntryPlanesSink(std::vector<Draws::SunEntryProcess>& a_out) :
			out(a_out) {}

		void Put(const ShadowCameras::Planes& a_planes);

	private:
		std::vector<Draws::SunEntryProcess>& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry. Every field keeps its default when none is registered. */
	struct SunVolumeEntry
	{
		bool registered = false;
		std::string name;
		decltype(Api::SunVolumeContribution::cascades) cascades = nullptr;
		decltype(Api::SunVolumeContribution::entryPlanes) entryPlanes = nullptr;
	};

	/** @brief Main thread, once, at the end of InstallFeatures. */
	void ResolveSunVolume(const Api::SunVolumeContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const SunVolumeEntry& SunVolume();
	/** @brief Render thread, once per colour commit. This frame's cascades, or nothing without a contribution. */
	void SampleSunCascades(Api::SunCascadeSink& a_sink);
	/** @brief Render thread. The full-frustum planes of scene frame a_frame or the one before. Returns whether there were any. */
	bool SampleSunEntryPlanes(Api::SunEntryPlanesSink& a_sink, std::uint32_t a_frame);
}
