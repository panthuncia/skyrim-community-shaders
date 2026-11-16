#include "RenderGraph/Engine/TonemapAnchor.h"

#include "Features/Effects11/ORG/ORGFxRuntime.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include <rhi_helpers.h>
#endif

namespace RenderGraph::Engine
{
	namespace
	{
		/** @brief The engine's targets of the tonemap call (guest): graph imports, with their view formats. */
		TonemapTargets EngineTargets([[maybe_unused]] std::uint32_t a_input, [[maybe_unused]] std::uint32_t a_output)
		{
			TonemapTargets targets;
#if defined(CS_HAS_RENDER_GRAPH)
			const auto total = static_cast<std::uint32_t>(RE::RENDER_TARGETS::kTOTAL);
			if (a_input >= total || a_output >= total)
				return targets;
			const auto& engine = globals::game::renderer->GetRuntimeData().renderTargets;
			if (engine[a_input].texture && engine[a_input].SRV) {
				targets.input = FrameAnchors::EngineTarget(a_input);
				D3D11_TEXTURE2D_DESC desc{};
				engine[a_input].texture->GetDesc(&desc);
				targets.inputFormat = rhi::helpers::ToRHI(desc.Format);
			}
			if (engine[a_output].texture && engine[a_output].RTV) {
				targets.output = FrameAnchors::EngineTarget(a_output);
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				engine[a_output].RTV->GetDesc(&view);
				targets.outputFormat = rhi::helpers::ToRHI(view.Format);
			}
#endif
			return targets;
		}
	}

	bool RunTonemapAnchor(std::uint32_t a_input, std::uint32_t a_output)
	{
		// CS's native chain names its own targets; otherwise the engine's call does.
		TonemapTargets targets;
		if (!ImageSpace::ChainTonemapTargets(targets))
			targets = EngineTargets(a_input, a_output);
		// The ORG FX runtime (CS_ORG_FX=1) settles its passes for this frame before the anchor runs them.
		ORGFxRuntime::Get().Prepare(targets);
		// Post Processing's chain may run in the anchor's epoch (ORGPostProcessing).
		if (globals::features::postProcessing.loaded)
			globals::features::postProcessing.BeforeTonemapAnchor(targets);
		return FrameAnchors::Run({ .anchor = FrameAnchors::Anchor::Tonemap, .input = static_cast<std::int32_t>(a_input), .output = static_cast<std::int32_t>(a_output) });
	}
}
