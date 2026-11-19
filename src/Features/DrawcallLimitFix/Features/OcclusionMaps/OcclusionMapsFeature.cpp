// OcclusionMaps: Skylighting's sky occlusion map and the precipitation mask, drawn by Core's shadow engine.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/Skylighting.h"
#include "Features/Skylighting/ORGSkylighting.h"

namespace DCLF::Features::OcclusionMaps
{
	namespace
	{
		constexpr auto kSky = static_cast<std::uint32_t>(Api::OcclusionMap::Sky);
		constexpr auto kPrecipitation = static_cast<std::uint32_t>(Api::OcclusionMap::Precipitation);

		// Skylighting swaps its own occlusion texture in for the precipitation target. With CS_ORG_SKYLIGHTING it is a graph twin's export.
		void ImportSkyTarget(Api::OcclusionTargetImport& a_import)
		{
			if (auto* sky = globals::features::skylighting.texOcclusion) {
				a_import.supplied = true;
				a_import.texture = sky->resource.get();
				a_import.view = sky->dsv.get();
				a_import.graph = ORGSkylighting::Get().OcclusionMap();
			}
		}
		bool SkyReferenceDue() { return ReferenceDue(kSky); }
		/**
		 * @brief Skylighting's probe grid floor, latched before this frame's grid update. It takes the lower of the last update's bottom
		 * and this frame's, so a camera that moved down draws no fewer occluders. -FLT_MAX after a reset.
		 */
		float SkyFloorHeight()
		{
			const auto& skylighting = globals::features::skylighting;
			float bottom = skylighting.probeGridBottomZ;
			if (const auto* camera = RE::Main::WorldRootCamera(); camera && bottom > -FLT_MAX)
				bottom = std::min(bottom, skylighting.ProbeGridBottomZ(camera->world.translate.z));
			return bottom - Skylighting::OCCLUSION_BELOW_GRID_MARGIN;
		}
		bool PrecipitationReferenceDue() { return ReferenceDue(kPrecipitation); }
		constexpr bool kPrecipitationAvailable = true;

		/**
		 * @brief The maps' contributions. The sky map exists while Skylighting is loaded, the mask always.
		 * Both draw from the precipitation accumulator's camera. The mask uses Skylighting's occluder rule when it is loaded.
		 */
		void RegisterMaps(Api::InstallContext& a_context, Api::ToggleId a_sky, Api::ToggleId a_precipitation)
		{
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Sky,
				.toggle = a_sky,
				.available = &globals::features::skylighting.loaded,
				.bits = { kSetOccluderSky, kViewSkyOccluder },
				.technique = &SkylightingTechnique,
				.minRadius = true,
				.floorHeight = &SkyFloorHeight,
				.target = Api::OcclusionTarget::ImportedTexture,
				.labels = { "sky occlusion", "Skylighting", "DCLF Skylighting occlusion map" },
				.importTarget = &ImportSkyTarget,
				.referenceDue = &SkyReferenceDue,
				.camera = &PrecipitationCamera,
				.ownsAccumulator = &SkyOwnsAccumulator });
			const bool skylighting = globals::features::skylighting.loaded;
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Precipitation,
				.toggle = a_precipitation,
				.available = &kPrecipitationAvailable,
				.bits = { kSetOccluderPrecipitation, kViewPrecipOccluder },
				.technique = skylighting ? &PrecipitationTechnique : &EnginePrecipitationTechnique,
				.minRadius = skylighting,
				.target = Api::OcclusionTarget::EngineMask,
				.labels = { "precipitation", "precipitation", "DCLF precipitation occlusion mask" },
				.referenceDue = &PrecipitationReferenceDue,
				.camera = &PrecipitationCamera,
				.ownsAccumulator = &PrecipitationOwnsAccumulator });
		}

		void Install(Api::InstallContext& a_context)
		{
			// Registered where Core's switches were, to keep the switch order.
			const auto skySwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SKYLIGHT", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: Skylighting's occlusion map stays native (live toggle's seed)", .before = "CS_DCLF_OPAQUE_EFFECTS" });
			const auto precipitationSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_PRECIPITATION", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: the precipitation occlusion mask stays native (live toggle's seed)", .before = "CS_DCLF_OPAQUE_EFFECTS" });
			// The first feature toggles registered, so their bits in the packed word stay where Core's were. They need the shadows.
			const auto sky = a_context.RegisterToggle({ .name = "sky occlusion", .label = "Draw Skylighting's occlusion map",
				.tooltip = "With Skylighting loaded, DCLF draws its sky occlusion height map from its own tables on the GPU, and the engine no longer culls or registers the scene for it.",
				.seed = skySwitch, .entersClassification = true, .after = "shadows", .needs = { "shadows" } });
			const auto precipitation = a_context.RegisterToggle({ .name = "precipitation occlusion", .label = "Draw the precipitation occlusion mask",
				.tooltip = "DCLF draws the precipitation occlusion mask (where rain and snow stop) from its own tables on the GPU, and the engine no longer culls or registers the scene for it.",
				.seed = precipitationSwitch, .entersClassification = true, .after = "shadows", .needs = { "shadows" } });
			RegisterMaps(a_context, sky, precipitation);
			RegisterReport(a_context);
			RegisterParity(a_context);
			if (Diagnostics::Observe().claimsOrScene)
				SubscribeOcclusionViewCheck();
		}
	}

	Api::Feature Feature()
	{
		return { "occlusion maps", &Install };
	}
}
