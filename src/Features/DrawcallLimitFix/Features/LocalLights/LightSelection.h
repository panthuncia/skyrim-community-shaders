#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

namespace RE
{
	class BSShadowLight;
}

namespace DCLF
{
	/**
	 * @brief DCLF's port of CalculateActiveShadowCasterLights (0x1414cc570): which shadow lights draw this frame.
	 * The engine keeps at most four lights, the sun first, each visible and sharing a room with the camera. Render thread.
	 *
	 * DCLF does the room test on its own portal walk of this frame. The engine instead tests each light's rooms as its
	 * last walk left them, so the two can differ for a light that moved.
	 * The focus-shadow flag (+0x558) persists across frames, so DCLF keeps its own copy per light.
	 */
	class LightSelection
	{
	public:
		struct Selected
		{
			const RE::BSShadowLight* light = nullptr;
			std::uint32_t slot = 0;                  // shadowLightsAccum slot.
			std::uint32_t maskIndex = 0;             // +0x520
			Portals::ContextHold portalContext;      // An owned snapshot, never the walk's scratch.
			std::uint32_t portalContextIndex = ~0u;  // Entry in Frame::portalContexts, or ~0u without a strict context.
			std::uint32_t portalWalk = 0;            // PortalViews::LightWalk
		};
		/**
		 * @brief A light kept by any selection this frame. The engine can select twice in a frame, and lights kept by the
		 * first call still draw, so the frame's atlas holds the contexts of every call.
		 */
		struct FrameLight
		{
			const RE::BSShadowLight* light = nullptr;
			Portals::ContextHold portalContext;
			std::uint32_t portalContextIndex = ~0u;
			std::uint32_t portalWalk = 0;
		};
		struct Frame
		{
			std::uint32_t sceneFrame = ~0u;
			std::uint32_t selections = 0;                      // Selections this frame. locals holds the last one's.
			std::vector<FrameLight> frameLights;               // In first-kept order.
			const FrameLight* LightOf(const RE::BSShadowLight* a_light) const
			{
				for (const auto& entry : frameLights)
					if (entry.light == a_light)
						return &entry;
				return nullptr;
			}
			const RE::BSShadowLight* sun = nullptr;  // Null when the sun does not draw.
			std::uint32_t sunSlots = 0;
			std::vector<Selected> locals;                      // Kept lights after the sun, in the engine's order.
			std::vector<const RE::BSShadowLight*> slots;       // shadowLightsAccum. Null marks a slot a light took but did not fill.
			std::vector<const RE::BSShadowLight*> focusHosts;
			std::uint32_t focusCount = 0;                      // At most 4.
			std::vector<std::uint32_t> portalContexts;         // The camera's context, then frameLights' strict contexts.
			bool valid = false;
		};

		static LightSelection& Get();
		void Install();
		/**
		 * @brief Native mode, in place of CalculateActiveShadowCasterLights: the same selection, with the engine's camera updates, focus
		 * cameras, keeps and bookkeeping done by CS (Features/Shadows/Native) and lights not kept released.
		 */
		void SelectNative();

		/**
		 * @brief This frame's selection, or null before CalculateActiveShadowCasterLights has run this frame. Native's render side reads
		 * the copy its light set carries (the selection its game half made), whatever a_sceneFrame.
		 */
		const Frame* Current(std::uint32_t a_sceneFrame) const;
		/** @brief Native's game half, at the light set: a copy of the selection's frame for the render side, or null. */
		std::shared_ptr<const Frame> CopyFrame() const { return frame.valid ? std::make_shared<const Frame>(frame) : nullptr; }

	private:
		LightSelection() = default;
		struct Hooks;
		friend struct Hooks;
		bool SharesRoomWithCamera(const RE::BSShadowLight* a_light);
		void Select(bool a_native = false);
		static void TellKeptLights(const Frame& a_frame);

		Frame frame;
		// A light not walked this frame keeps its last entry.
		std::unordered_map<const RE::BSShadowLight*, PortalViews::LightRooms> lightRooms;
		std::unordered_map<const RE::BSShadowLight*, bool> roomVerdicts;
		// DCLF's copy of +0x558 per light, and last frame's focus host (0x1433dcfb8).
		std::unordered_map<const RE::BSShadowLight*, bool> focusFlags;
		const RE::BSShadowLight* lastFocusHost = nullptr;

		bool installed = false;
	};
}
