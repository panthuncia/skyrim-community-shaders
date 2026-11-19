#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
}

namespace DCLF::Api
{
	class ShadowSourceCamera;
}

namespace DCLF::Features::Shadows::Local
{
	/**
	 * @brief The focus shadows' views, per focus descriptor below the target count: the camera and target i's node.
	 * The focus accumulation (FUN_1414f0b90) culls nothing, so there is no caster volume. Render thread.
	 */
	struct FocusFrame
	{
		static constexpr std::uint32_t kMaxFocus = 4;  // focusShadowmapDescriptors

		struct Host
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<ShadowCameras::Camera> views;  // by focus descriptor
		};
		std::vector<Host> hosts;  // this frame's
		std::array<const RE::NiAVObject*, kMaxFocus> targets{};
		std::uint32_t targetCount = 0;
		std::uint32_t sceneFrame = ~0u;

		/** @brief a_light's focus view a_descriptor this frame, or null. */
		const ShadowCameras::Camera* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (a_sceneFrame != sceneFrame)
				return nullptr;
			for (const auto& host : hosts)
				if (host.light == a_light)
					return a_descriptor < host.views.size() && host.views[a_descriptor].valid ? &host.views[a_descriptor] : nullptr;
			return nullptr;
		}
		/** @brief Focus target a_descriptor's node this frame, or null. */
		const RE::NiAVObject* TargetOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			return a_sceneFrame == sceneFrame && a_descriptor < targetCount ? targets[a_descriptor] : nullptr;
		}
	};

	/** @brief The board. Render thread. Its storage is reused in place. */
	FocusFrame& FocusBoard();

	/**
	 * @brief Hooks BSShadowDirectionalLight::sub (0x1414f0480) and records the focus views from the cameras it just aimed.
	 * The targets are at 0x14332a488 ({distance, node}) with the count at 0x14332a498. Render thread.
	 */
	class FocusViews
	{
	public:
		static FocusViews& Get();
		void Install();

	private:
		FocusViews() = default;
		struct Hooks;
		friend struct Hooks;
		void Update(const RE::BSShadowLight* a_light);

		bool installed = false;
	};

	/** @brief The engine's focus target a_index's node, or null past the count. */
	const RE::NiAVObject* EngineFocusTarget(std::uint32_t a_index);
	std::uint32_t EngineFocusTargetCount();

	/** @brief The Focus source's camera, from the board. */
	void FocusCamera(Api::ShadowSourceCamera& a_camera);
}
