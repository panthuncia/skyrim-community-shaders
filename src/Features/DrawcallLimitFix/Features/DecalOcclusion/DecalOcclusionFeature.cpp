// Decal occlusion feature registration (CS_ORG_OCCLUSION_QUERIES=1, default off).

#include "Features/DrawcallLimitFix/Features/DecalOcclusion/DecalOcclusion.h"

#include "RenderGraph/Engine/Queries/OcclusionQueries.h"

#include <string>

namespace DCLF::Features::DecalOcclusion
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			// Must happen before the render graph initializes, which fixes the anchors' epochs.
			DecalDraws::Reserve();
			InstallHooks();
			logger::info("[DCLF] decal occlusion: on (CS_ORG_OCCLUSION_QUERIES=1): the decal occlusion tests are drawn at the DecalQueries anchor");
		}

		std::string Status()
		{
			return DecalDraws::Status();
		}
	}

	bool Enabled()
	{
		return ::RenderGraph::Engine::OcclusionQueries::Requested();
	}

	Api::Feature Feature()
	{
		return { .name = "decal occlusion", .install = &Install, .status = &Status };
	}
}
