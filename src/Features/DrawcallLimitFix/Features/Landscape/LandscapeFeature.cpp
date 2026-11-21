// Landscape feature registration.

#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"

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
			// Their objects take the land-blend extras, Core's per-object VS rows.
			for (const auto technique : { Technique::MTLand, Technique::MTLandLODBlend })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .extras = Api::ObjectExtras::LandBlend });
		}
	}

	Api::Feature Feature()
	{
		return { "landscape", &Install };
	}
}
