#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <array>
#include <cstdint>
#include <vector>

namespace DCLF::Api
{
	class ShadowSourceCamera;
	class SunCascadeSink;
	class SunEntryPlanesSink;
}

namespace DCLF::Features::Shadows::Sun
{
	/**
	 * @brief The sun's views from the cameras BSShadowDirectionalLight::UpdateCamera (0x141512230) just set up: each cascade's camera
	 * and the planes each full-frustum process tests. Written on the render thread before the engine's culls.
	 * The Sun source's registration threads read the cascades only after its bitsReady release.
	 */
	struct SunFrame
	{
		static constexpr std::uint32_t kMaxCascades = 4;

		std::uint32_t sceneFrame = ~0u;
		std::uint32_t count = 0;
		std::array<ShadowCameras::Camera, kMaxCascades> cascades{};
		std::vector<ShadowCameras::Planes> fullFrusta;  // what each process of the full-frustum cull tests (Process2), by process
		bool valid = false;

		/** @brief Cascade a_descriptor of this frame's views, or null. */
		const ShadowCameras::Camera* CascadeOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (!valid || sceneFrame != a_sceneFrame || a_descriptor >= count || !cascades[a_descriptor].valid)
				return nullptr;
			return &cascades[a_descriptor];
		}
	};

	/** @brief The board. Render thread. Its storage is reused in place. */
	SunFrame& SunBoard();

	/**
	 * @brief Hooks BSShadowDirectionalLight::UpdateCamera (0x141512230) and records the sun's views at its return.
	 * Everything derived uses the engine's own arithmetic (ShadowCameras). Render thread.
	 */
	class SunViews
	{
	public:
		static SunViews& Get();
		void Install();

	private:
		SunViews() = default;
		struct Hooks;
		friend struct Hooks;
		void Update(const void* a_light);

		bool installed = false;
	};

	/** @brief The Sun source's camera, from the board. */
	void SunCamera(Api::ShadowSourceCamera& a_camera);
	/** @brief This frame's sun cascades, in descriptor order. */
	void SampleSunCascades(Api::SunCascadeSink& a_sink);
	/** @brief The full-frustum planes of a_frame or the one before. */
	bool SampleSunEntryPlanes(Api::SunEntryPlanesSink& a_sink, std::uint32_t a_frame);
}
