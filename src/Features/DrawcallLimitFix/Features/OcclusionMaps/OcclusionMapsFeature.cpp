// OcclusionMaps: Skylighting's sky occlusion map and the precipitation mask, drawn by Core's shadow engine.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/Skylighting.h"
#include "Features/Skylighting/ORGSkylighting.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"
#include "Snapshot/Publication.h"

namespace DCLF::Features::OcclusionMaps
{
	namespace
	{
		// Skylighting swaps its own occlusion texture in for the precipitation target. In graph mode it is a graph twin's export.
		void ImportSkyTarget(Api::OcclusionTargetImport& a_import)
		{
			if (auto* sky = globals::features::skylighting.texOcclusion) {
				a_import.supplied = true;
				a_import.texture = sky->resource.get();
				a_import.view = sky->dsv.get();
				a_import.graph = ORGSkylighting::Get().OcclusionMap();
			}
		}
		/**
		 * @brief Skylighting's probe grid floor, latched before this frame's grid update. It takes the lower of the last update's bottom
		 * and this frame's, so a camera that moved down draws no fewer occluders. -FLT_MAX after a reset.
		 */
		float SkyFloorHeight()
		{
			const auto& skylighting = globals::features::skylighting;
			float bottom = skylighting.probeGridBottomZ;
			if (bottom > -FLT_MAX) {
				// The world camera's height: native's from the adopted snapshot, guest's read now.
				const auto& snapshot = Snapshot::Current();
				if (const auto* view = snapshot && Snapshot::FrameCamera::Owned() ? &snapshot->cameras.Current(Snapshot::ViewId::Main) : nullptr) {
					if (view->valid)
						bottom = std::min(bottom, skylighting.ProbeGridBottomZ(view->position.z));
				} else if (const auto* camera = CS_ENGINE_READ(Camera, RecordTime, RE::Main::WorldRootCamera()))
					bottom = std::min(bottom, skylighting.ProbeGridBottomZ(camera->world.translate.z));
			}
			return bottom - Skylighting::OCCLUSION_BELOW_GRID_MARGIN;
		}
		constexpr bool kPrecipitationAvailable = true;

		/**
		 * @brief The maps' contributions. The sky map exists while Skylighting is loaded, the mask always.
		 * Both draw from the precipitation accumulator's camera. The mask uses Skylighting's occluder rule when it is loaded.
		 */
		void RegisterMaps(Api::InstallContext& a_context)
		{
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Sky,
				.available = &globals::features::skylighting.loaded,
				.bits = { kSetOccluderSky, kViewSkyOccluder },
				.technique = &SkylightingTechnique,
				.minRadius = true,
				.floorHeight = &SkyFloorHeight,
				.target = Api::OcclusionTarget::ImportedTexture,
				.labels = { "sky occlusion", "Skylighting", "DCLF Skylighting occlusion map" },
				.importTarget = &ImportSkyTarget,
				.camera = &PrecipitationCamera });
			const bool skylighting = globals::features::skylighting.loaded;
			a_context.RegisterOcclusionMap({ .map = Api::OcclusionMap::Precipitation,
				.available = &kPrecipitationAvailable,
				.bits = { kSetOccluderPrecipitation, kViewPrecipOccluder },
				.technique = skylighting ? &PrecipitationTechnique : &EnginePrecipitationTechnique,
				.minRadius = skylighting,
				.target = Api::OcclusionTarget::EngineMask,
				.labels = { "precipitation", "precipitation", "DCLF precipitation occlusion mask" },
				.camera = &PrecipitationCamera });
		}

		void Install(Api::InstallContext& a_context)
		{
			// DCLF draws both maps from its own tables on the GPU, and the engine no longer culls or registers the scene for them.
			RegisterMaps(a_context);
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "occlusion maps", &Install };
	}
}
