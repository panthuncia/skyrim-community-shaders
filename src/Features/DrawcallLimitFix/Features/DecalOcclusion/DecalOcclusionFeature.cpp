// Decal occlusion feature registration. On in graph mode.

#include "Features/DrawcallLimitFix/Features/DecalOcclusion/DecalOcclusion.h"

#include "RenderGraph/Engine/Engine.h"
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
			InstallHooks();
			logger::info("[DCLF] decal occlusion: on (graph mode): the decal occlusion tests are drawn at the DecalQueries anchor");
		}

		std::string Status()
		{
			return DecalDraws::Status();
		}
	}

	bool Enabled()
	{
		return ::RenderGraph::Engine::GraphMode();
	}

	Api::Feature Feature()
	{
		return { .name = "decal occlusion", .install = &Install, .status = &Status };
	}
}
