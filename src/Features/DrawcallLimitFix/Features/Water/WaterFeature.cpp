// Water: the engine's BSWaterShader draws drawn by the render graph in graph mode.
// The engine side is WaterBatches.cpp and WaterNative.cpp, the graph side WaterDraws.cpp.

#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"
#include "Features/DrawcallLimitFix/Features/Water/Scene/WaterScene.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"
#include "RenderGraph/Engine/Native/Native.h"


#include <algorithm>
#include <array>
#include <string>

namespace DCLF::Features::Water
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// The scene side does not need graph mode. What it does not claim, the batch hooks below compute and draw.
			WaterScene::Install(a_context);
			// The water program family, its deferred ranges and their epochs.
			Indirect::Register(a_context);
			Indirect::RegisterEpochs(a_context);
			if (!Enabled())
				return;
			const bool hooked = InstallHooks();
			logger::info("[DCLF] water: on (graph mode): the engine's water batches are drawn at the EngineDraws anchor (mask and surface)");
			// The batch hooks trigger the water epochs, so only then may the port claim water.
			if (hooked) {
				Indirect::SetDrawer(true);
				logger::info("[DCLF] water: plain water is claimed and drawn by the water.mask and water.surface epochs");
			}
		}

		std::string Status()
		{
			return WaterDraws::Status();
		}
	}

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "water", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
