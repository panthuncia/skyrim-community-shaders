// Terrain Shadows: its shadow height map (t60), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/TerrainShadows/TerrainShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/TerrainShadows.h"
#include "Features/TerrainShadows/ORGTerrainShadows.h"

namespace DCLF::Integrations::TerrainShadows
{
	namespace
	{
		// The view EarlyPrepass binds at t60 while terrain shadows are on. It is already this frame's at the faces' commit, which follows the shadow maps.
		// Off, or before the map exists, EarlyPrepass binds nothing, so the captured view.
		void* ShadowHeightView()
		{
			const auto& terrainShadows = globals::features::terrainShadows;
			return terrainShadows.loaded && terrainShadows.settings.EnableTerrainShadow ? terrainShadows.GetShadowHeightSRV() : nullptr;
		}

		// On the ORG path the view is the graph texture's export, so commits read it ordered after the update at the EarlyPrepass anchor.
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "terrain shadows height", .textureRegister = kShadowHeightRegister, .view = &ShadowHeightView,
				.capturedWhenNull = true, .graphResources = { ORGTerrainShadows::kShadowHeightId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "terrain shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::terrainShadows); } };
	}
}
