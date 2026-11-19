// The shadow epoch's frame points, called by Core's FrameDriver: the shadow views' coverage before the engine's lights register, the
// shadow frame, the occlusion maps' readiness, and the shadow epoch under shadow-maps.after.

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <cstdint>

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		// Render thread, after the install: the epoch's trigger.
		Api::TriggerId shadowMapsAfter;

		void BeforeShadowCasters()
		{
			// The frame's views are DCLF's only while the selected revision recorded the shadow payload, decided before any of them registers.
			if (ActiveToggles().shadows)
				IndirectDraws::Get().DecideShadowCoverage();
		}

		void BeforeShadowMaps()
		{
			if (ActiveToggles().shadows)
				IndirectDraws::Get().BeginShadowFrame();
		}

		std::uint32_t AfterShadowMaps(std::uint32_t a_maps)
		{
			Extensions::TriggerScope trigger(shadowMapsAfter);
			return IndirectDraws::Get().ExecuteShadowFrame(a_maps);
		}

		bool OcclusionReady(std::uint32_t a_map) { return IndirectDraws::Get().OcclusionReady(a_map); }
	}

	void RegisterShadowEpochs(Api::InstallContext& a_context)
	{
		shadowMapsAfter = a_context.FindTrigger("shadow-maps.after");
		a_context.RegisterShadowEpochs(
			{ .beforeShadowCasters = &BeforeShadowCasters, .beforeShadowMaps = &BeforeShadowMaps, .afterShadowMaps = &AfterShadowMaps,
				.occlusionReady = &OcclusionReady });
	}
}
