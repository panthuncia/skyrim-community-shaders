#include "RenderGraph/Engine/TonemapAnchor.h"

#include "Features/Effects11/ORG/ORGFxRuntime.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "RenderGraph/FrameAnchors.h"

namespace RenderGraph::Engine
{
	bool RunTonemapAnchor(std::uint32_t a_input, std::uint32_t a_output)
	{
		// The ORG FX runtime (CS_ORG_FX=1) settles its passes for this frame before the anchor runs them.
		ORGFxRuntime::Get().Prepare(a_input, a_output);
		// Post Processing's chain may run in the anchor's epoch (ORGPostProcessing).
		if (globals::features::postProcessing.loaded)
			globals::features::postProcessing.BeforeTonemapAnchor(static_cast<RE::RENDER_TARGET>(a_input));
		return FrameAnchors::Run({ .anchor = FrameAnchors::Anchor::Tonemap, .input = static_cast<std::int32_t>(a_input), .output = static_cast<std::int32_t>(a_output) });
	}
}
