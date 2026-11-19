// Landscape feature registration.

#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "ShaderCache.h"

#include <cstdint>

namespace DCLF::Features::Landscape
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;

		void Install(Api::InstallContext& a_context)
		{
			InstallSeams(a_context);
			const auto terrainSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_MTLAND", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: terrain stays native (live toggle's seed)", .before = "CS_DCLF_FORWARD" });
			const auto terrain = a_context.RegisterToggle({ .name = "terrain", .label = "Terrain", .seed = terrainSwitch,
				.entersClassification = true, .after = "projected" });
			// Their objects take the land-blend extras, Core's per-object VS rows.
			for (const auto technique : { Technique::MTLand, Technique::MTLandLODBlend })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .toggle = terrain, .extras = Api::ObjectExtras::LandBlend });
			const auto trace = a_context.RegisterSwitch({ .name = "CS_DCLF_LAND_TRACE", .kind = Api::SwitchKind::Diagnostic, .reduced = Api::SwitchReduced::Never,
				.description = "1: every loaded land quad's engine state (node, geometry, property, material, texture paths) when first seen and when it changes",
				.after = "CS_DCLF_TREE_LOD_AUDIT" });
			if (SwitchEnabled(trace))
				SubscribeLandTrace();
		}
	}

	Api::Feature Feature()
	{
		return { "landscape", &Install };
	}
}
