// Skins: the skinned shapes DCLF draws.

#include "Features/DrawcallLimitFix/Features/Skins/Skins.h"

namespace DCLF::Features::Skins
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Registered where Core's switches were, to keep the order.
			const auto skinnedSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SKINNED", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: skinned objects stay native (live toggle's seed)", .after = "CS_DCLF_CULL" });
			const auto partitionsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_SKIN_PARTITIONS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: skins of several partitions stay native (live toggle's seed)", .after = "CS_DCLF_SKINNED" });
			// Registered where Core's toggles were. Features/FirstPerson's toggle needs the skinned one.
			const auto skinned = a_context.RegisterToggle({ .name = "skinned", .label = "Skinned", .section = "Object classes", .seed = skinnedSwitch,
				.entersClassification = true, .before = "projected" });
			const auto partitions = a_context.RegisterToggle({ .name = "skin partitions", .label = "Skins of several partitions: LOD trees, actor bodies",
				.seed = partitionsSwitch, .entersClassification = true, .after = "tree LOD", .needs = { "skinned" } });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Skinned, .toggle = skinned });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::SkinPartitions, .toggle = partitions });
			RegisterReport(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "skins", &Install };
	}
}
