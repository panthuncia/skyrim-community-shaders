#include "RenderGraph/Engine/TonemapAnchor.h"

#include "Features/Effects11/ORG/ORGFxRuntime.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Frame/EngineTargetUse.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include <rhi_helpers.h>
#endif

namespace RenderGraph::Engine
{
	namespace
	{
		/** @brief Guest mode. */
		TonemapTargets EngineTargets([[maybe_unused]] std::uint32_t a_input, [[maybe_unused]] std::uint32_t a_output)
		{
			TonemapTargets targets;
#if defined(CS_HAS_RENDER_GRAPH)
			const auto total = static_cast<std::uint32_t>(RE::RENDER_TARGETS::kTOTAL);
			if (a_input >= total || a_output >= total)
				return targets;
			const auto& engine = globals::game::renderer->GetRuntimeData().renderTargets;
			// Native mode: an engine target with no memory is not named, so the tonemap outside CS's chain draws nothing there.
			const auto unbacked = [](std::uint32_t a_target) {
				if (!Frame::EngineTargetUse::ColourUnbacked(a_target))
					return false;
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::info("[ORG] Tonemap anchor: the engine's tonemap names {}, which has no memory in native mode; the anchor names no target for it (logged once)",
						magic_enum::enum_name(static_cast<RE::RENDER_TARGET>(a_target)));
				return true;
			};
			if (engine[a_input].texture && engine[a_input].SRV && !unbacked(a_input)) {
				targets.input = FrameAnchors::EngineTarget(a_input);
				D3D11_TEXTURE2D_DESC desc{};
				engine[a_input].texture->GetDesc(&desc);
				targets.inputFormat = rhi::helpers::ToRHI(desc.Format);
			}
			if (engine[a_output].texture && engine[a_output].RTV && !unbacked(a_output)) {
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
		TonemapTargets targets;
		if (!ImageSpace::ChainTonemapTargets(targets))
			targets = EngineTargets(a_input, a_output);
		// The FX runtime settles its passes before the anchor runs them.
		ORGFxRuntime::Get().Prepare(targets);
		if (globals::features::postProcessing.loaded)
			globals::features::postProcessing.BeforeTonemapAnchor(targets);
		return FrameAnchors::Run({ .anchor = FrameAnchors::Anchor::Tonemap, .input = static_cast<std::int32_t>(a_input), .output = static_cast<std::int32_t>(a_output) });
	}
}
