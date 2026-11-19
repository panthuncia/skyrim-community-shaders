#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF::Api
{
	class ShadowSourceCamera;
}

namespace DCLF::Features::Shadows::Local
{
	/**
	 * @brief The views of the local shadow lights DCLF selects: per descriptor, the camera and the planes the light's cull tests.
	 * Written on the render thread as the selection keeps each light and read later in the frame.
	 */
	struct LightFrame
	{
		struct Light
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<ShadowCameras::Camera> views;  // by descriptor
			std::array<float, 4> sphere{};  // the light's position and radius (radius 0: none)
		};
		std::vector<Light> lights;  // this frame's, in the selection's order
		std::uint32_t sceneFrame = ~0u;

		/** @brief Descriptor a_descriptor of a_light's views this frame, or null when not selected this frame. */
		const ShadowCameras::Camera* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (a_sceneFrame != sceneFrame)
				return nullptr;
			for (const auto& entry : lights)
				if (entry.light == a_light)
					return a_descriptor < entry.views.size() && entry.views[a_descriptor].valid ? &entry.views[a_descriptor] : nullptr;
			return nullptr;
		}

		/** @brief Every view of the lights kept in scene frame a_sceneFrame, with its light's sphere, for the actor rate. */
		template <class F>
		void ForEachView(std::uint32_t a_sceneFrame, F&& a_visit) const
		{
			if (sceneFrame != a_sceneFrame)
				return;
			for (const auto& entry : lights)
				for (const auto& view : entry.views)
					if (view.valid)
						a_visit(view, entry.sphere);
		}
	};

	/** @brief The board. Render thread. Its storage is reused in place. */
	LightFrame& LightsBoard();

	/**
	 * @brief Computes the local shadow lights' views into the board, at CalculateActiveShadowCasterLights' (0x1414cc570) return.
	 * By then each kept light's UpdateCamera (vfunc 0x10) has run. Nothing here reads a cull or the renderer's state at draw time.
	 */
	class LightViews
	{
	public:
		static LightViews& Get();
		/** @brief Computes a kept light's views for this frame. The first of a frame drops the last frame's. */
		void Update(const RE::BSShadowLight* a_light);

	private:
		LightViews() = default;
	};

	/** @brief The Spot and Point sources' camera, from the board. */
	void LightCamera(Api::ShadowSourceCamera& a_camera);
}
