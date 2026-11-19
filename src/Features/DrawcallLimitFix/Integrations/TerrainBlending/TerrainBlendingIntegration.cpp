// Terrain Blending: the classify input, its switch and toggle, the terrain technique gates, its held groups, its mask (t55),
// the scene depth (t17) and its near depth twin. Callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/TerrainBlending/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "ShaderCache.h"

#include "Features/TerrainBlending.h"
#include "Features/TerrainBlending/ORGTerrainBlending.h"

#include <cstdint>

namespace DCLF::Integrations::TerrainBlending
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;

		// Any thread. Whether Terrain Blending draws its terrain after the opaque pass in a frame DCLF draws.
		// Classification takes it once per pass and a change drops the verdicts.
		bool DefersTerrain()
		{
			const auto& terrainBlending = globals::features::terrainBlending;
			return terrainBlending.loaded && terrainBlending.settings.Enabled;
		}

		// The mask (t55) is bound by its replay after this epoch, so it is named here instead of captured.
		// By the colour epoch it holds the Z-prepass's main depth. Null while it does not hold the terrain, so the captured view.
		// On the ORG path the mask is the graph mask's export, so commits read it ordered after the blend.
		void* MaskView()
		{
			return DefersTerrain() ? globals::features::terrainBlending.GetMaskSRV() : nullptr;
		}

		// Its terrain depth shares the main depth's description. The Z-prepass draws near terrain blocks into it as its depth hook does.
		void* TerrainDepthTarget()
		{
			const auto& terrainBlending = globals::features::terrainBlending;
			return terrainBlending.loaded && terrainBlending.terrainDepth.texture ? terrainBlending.terrainDepth.texture : nullptr;
		}

		// Its terrain depth as the graph owns it on the ORG path. Null means a D3D11 texture to import.
		std::shared_ptr<org::ExternalTextureResource> TerrainDepthGraph()
		{
			return globals::features::terrainBlending.loaded ? ::ORGTerrainBlending::Get().TerrainDepthGraph() : nullptr;
		}

		// Its near test with its depth-pass eye, for the terrain group 1 twin buckets.
		bool NearTest(float (&a_eyeDistance)[4])
		{
			if (!DefersTerrain())
				return false;
			const auto& terrainEye = globals::features::terrainBlending.eyePosition;
			a_eyeDistance[0] = terrainEye.x;
			a_eyeDistance[1] = terrainEye.y;
			a_eyeDistance[2] = terrainEye.z;
			a_eyeDistance[3] = ::TerrainBlending::kTerrainDepthDistance;
			return true;
		}

		void Install(Api::InstallContext& a_context)
		{
			// Switching it moves the terrain and its no-blend meshes between their held groups and the native loop.
			const auto defers = a_context.RegisterClassifyInput({ .name = "terrain blending defers", .sample = &DefersTerrain });
			// Listed right after Features/Landscape's CS_DCLF_MTLAND, and its toggle right after the terrain's.
			const auto terrainSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_TB_TERRAIN", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: terrain under Terrain Blending, and the meshes that feature redraws after it, stay native (T7c1; live toggle's seed)",
				.after = "CS_DCLF_MTLAND" });
			const auto terrain = a_context.RegisterToggle({ .name = "terrain under TB", .label = "Terrain under Terrain Blending",
				.tooltip = "With Terrain Blending on, DCLF draws the terrain as that feature does: its near blocks' depth into its terrain depth and, offset, into the main depth, and its colour blended after the opaque and decal draws, then the meshes it redraws after the terrain.",
				.seed = terrainSwitch, .entersClassification = true, .after = "terrain", .needs = { "terrain" } });
			// While Terrain Blending holds the terrain, MTLand and MTLandLODBlend also need this toggle, because it redraws every terrain pass blended
			// with its own depth state, which an opaque draw would break. DCLF then draws the terrain the same way in its terrain group.
			for (const auto technique : { Technique::MTLand, Technique::MTLandLODBlend })
				a_context.RegisterTechniqueGate({ .technique = static_cast<std::uint32_t>(technique), .whileOn = defers, .needs = terrain });
			// The terrain blocks use blend mode 1 with LESS_EQUAL and depth writes, and the no-blend meshes after them EQUAL, replayed before the blended decals.
			// Without the toggle the no-blend meshes stay native, because DCLF would draw them before the terrain blends over them.
			a_context.RegisterHeldGroup({ .heldWhile = defers, .slot = kTerrainRange, .name = "terrain", .order = 2,
				.propertyFlags = static_cast<std::uint64_t>(Flag::kMultiTextureLandscape) });
			a_context.RegisterHeldGroup({ .heldWhile = defers, .slot = kNoBlendRange, .name = "terrain-no-blend", .order = 3,
				.propertyFlags = static_cast<std::uint64_t>(Flag::kNoTransparencyMultiSample), .toggle = terrain });
			// Its mask, t55, each colour commit.
			a_context.RegisterFrameTexture({ .name = "terrain blending mask", .textureRegister = kMaskRegister, .view = &MaskView,
				.capturedWhenNull = true, .graphResources = { ::ORGTerrainBlending::kMaskId } });
			// t17 has no view of its own. The colour commit reads the captured view and the faces null.
			// On the ORG path that view is the 16-bit blended depth's export from the depth pass until the deferred pass's final depth.
			a_context.RegisterFrameTexture({ .name = "terrain blending scene depth", .textureRegister = kSceneDepthRegister,
				.graphResources = { ::ORGTerrainBlending::kBlendedDepth16Id } });
			// Its near blocks are drawn twice in the depth segment: into its terrain depth and, offset, into the main depth.
			a_context.RegisterNearTwin({ .name = "Terrain Blending's terrain depth", .debugName = "DCLF terrain blending depth",
				.resource = "cs.dclf.terrain-blending-depth", .target = &TerrainDepthTarget, .nearTest = &NearTest, .graph = &TerrainDepthGraph });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "terrain blending", .install = &Install, .status = [] { return FeatureStatus(globals::features::terrainBlending); } };
	}
}
