// Decal occlusion: the engine's decal occlusion test drawn by the render graph (CS_ORG_OCCLUSION_QUERIES=1, default off). The feature
// reserves the DecalQueries anchor and installs the engine hooks; the rest is DecalOcclusionCapture.cpp and DecalOcclusionDraws.cpp.

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
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
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
