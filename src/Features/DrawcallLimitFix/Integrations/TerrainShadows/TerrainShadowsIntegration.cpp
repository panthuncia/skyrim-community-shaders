// Terrain Shadows: its shadow height map (t60), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/TerrainShadows/TerrainShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/TerrainShadows.h"

namespace DCLF::Integrations::TerrainShadows
{
	namespace
	{
		// The view EarlyPrepass binds at t60 while terrain shadows are on: its shadow height map, remade there on a new heightmap, so
		// already this frame's texture (and this frame's update) at the faces' commit, which follows the shadow maps. Off, or before the
		// map exists, EarlyPrepass binds nothing (the reflection prepass's binding, or an earlier frame's, stands): the captured view.
		// On the ORG path (CS_ORG_TERRAIN_SHADOWS) the view is the graph texture's D3D11 export, imported like any other view.
		void* ShadowHeightView()
		{
			const auto& terrainShadows = globals::features::terrainShadows;
			return terrainShadows.loaded && terrainShadows.settings.EnableTerrainShadow ? terrainShadows.GetShadowHeightSRV() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "terrain shadows height", .textureRegister = kShadowHeightRegister, .view = &ShadowHeightView,
				.capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "terrain shadows", .install = &Install, .status = [] { return FeatureStatus(globals::features::terrainShadows); } };
	}
}
