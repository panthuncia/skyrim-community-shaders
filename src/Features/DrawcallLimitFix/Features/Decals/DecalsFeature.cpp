// Decals feature registration: switches, toggles, object classes, main ranges, decal capture and order.

#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Decals
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			const auto decalsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_DECALS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: decals stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			const auto layersSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LAYERS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: multi-index shapes' main passes stay native (live toggle's seed)", .after = "CS_DCLF_SKIN_PARTITIONS" });
			const auto decals = a_context.RegisterToggle({ .name = "decals", .label = "Decals", .seed = decalsSwitch,
				.entersClassification = true, .after = "skinned" });
			// The layer is the additional property's passes, hint 12, drawn as decal group 3.
			const auto layers = a_context.RegisterToggle({ .name = "multi-index layers", .label = "Multi-index shapes: cave walls with an ice or snow layer",
				.seed = layersSwitch, .entersClassification = true, .after = "skinned", .needs = { "decals" } });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Decal, .toggle = decals });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Layer, .toggle = layers });
			// Engine geometry groups 3, 2 and 4. Blended decals come after Terrain Blending's groups, which use orders 2 and 3.
			a_context.RegisterMainRange({ .slot = kOpaqueRange, .name = "decals-opaque", .order = 0, .depthPrepass = true, .objectClass = Api::ObjectClass::Decal });
			a_context.RegisterMainRange({ .slot = kLayerRange, .name = "decals-layers", .order = 1, .objectClass = Api::ObjectClass::Layer });
			a_context.RegisterMainRange({ .slot = kBlendedRange, .name = "decals-blended", .order = 4, .objectClass = Api::ObjectClass::Decal });
			RegisterDecalCapture(a_context);
			const auto order = RegisterOrder(a_context);
			a_context.RegisterReportLines({ .name = "decals", .point = Api::ReportPoint::SceneCoordinatorLastPass, .component = order, .componentText = &CandidateLines });
			a_context.RegisterReportLines({ .name = "decal order parity", .point = Api::ReportPoint::SceneCoordinatorMaterials, .component = order,
				.componentText = &OrderParityLines });
			if (Diagnostics::Observe().scene)
				SubscribeOrderParity();
		}
	}

	Api::Feature Feature()
	{
		return { "decals", &Install, &InstallDecalHooks };
	}
}
