// Submits the forward epoch at the main pass's blended draw. It shares the colour epoch's shape.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Globals.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace DCLF::Features::Forward
{
	namespace
	{
		// Render thread.
		struct RefractionStats
		{
			std::uint64_t drawn = 0, notReady = 0;
			std::uint32_t frame = ~0u;
		};
		RefractionStats refraction;

		// The first call must come after InstallFeatures has resolved the schedule.
		std::uint32_t ForwardEpoch()
		{
			static const std::uint32_t epoch = Draws::EpochById("forward");
			return epoch;
		}

		// Returns whether this recording draws the refraction normals.
		bool PrepareRefraction(void*, const Draws::Resources* a_resources, const Draws::PassFrame* a_shape)
		{
			bool drawn = false;
			if (Extensions::ObjectClassOn(Api::ObjectClass::Refraction)) {
				const auto* resources = a_resources;
				auto& pipelines = DrawPipelines::Get();
				pipelines.RequestRefraction();
				if (resources && resources->refractionNormals && globals::game::utilityShader) {
					// The engine's write mask for write mode 2 with blending off (FUN_1414b4860). Refraction is outside the deferred pass.
					std::uint8_t writeMask = 0xF;
					if (auto* blend = EngineBlendStates()[0][0][2][0]) {
						D3D11_BLEND_DESC desc{};
						blend->GetDesc(&desc);
						writeMask = static_cast<std::uint8_t>(desc.RenderTarget[0].RenderTargetWriteMask & 0xF);
					}
					pipelines.SetRefractionInputs(*globals::game::utilityShader, resources->refractionFormat, writeMask);
				}
				if (a_shape && a_shape->refractionCapacity) {
					drawn = resources && resources->refractionNormals && resources->pool && a_shape->indirect.refraction.valid() &&
					        a_shape->indirect.refractionFormat == resources->refractionFormat;
					if (!drawn)
						++refraction.notReady;
				}
			}
			return drawn;
		}

		void ReportLines(std::string& a_text)
		{
			const auto f = IndirectDraws::Get().TakeSharedEpochStats(ForwardEpoch());
			a_text += fmt::format("[DCLF] forward epoch (T7d): reached {} frames, {} epochs; not submitted: {} without this frame's colour epoch, "
								  "{} without a forward recording{}, {} with nothing to draw (no forward or refraction slot), {} failed{}\n",
				f.reached, f.epochs, f.noSharedPayload, f.noRecording,
				f.noRecording ? " <- NO FORWARD RECORDING" : "", f.empty, f.failed,
				f.failed ? " <- EPOCH FAILED" : "");
			if (refraction.drawn || refraction.notReady || Extensions::ObjectClassOn(Api::ObjectClass::Refraction))
				a_text += fmt::format("[DCLF] refraction (T7d): {} forward epochs drew the refraction normals, {} with refraction slots could not "
									  "(no refraction pipeline yet, or no refraction normals){}\n",
					refraction.drawn, refraction.notReady,
					refraction.notReady && !refraction.drawn ? " <- NO REFRACTION DRAW" : "");
			refraction.drawn = refraction.notReady = 0;
		}
	}

	bool SubmitForwardEpoch()
	{
		DCLF_ZONE("CS.DCLF.ExecuteForward");
		const std::uint32_t epoch = ForwardEpoch();
		if (!Extensions::ObjectClassOn(Api::ObjectClass::Forward) || epoch == Draws::kNoEpoch)
			return false;
		const auto result = IndirectDraws::Get().SubmitSharedEpoch(epoch, { .prepare = &PrepareRefraction,
			.empty = [](const Draws::PassFrame& a_shape) { return !a_shape.forwardCapacity && !a_shape.refractionCapacity; } });
		if (!result.submitted || !result.optional)
			return false;
		refraction.frame = Scene::FrameView::Get().SceneStep();
		++refraction.drawn;
		return true;
	}

	bool RefractionDrawnThisFrame()
	{
		return refraction.frame == Scene::FrameView::Get().SceneStep();
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "forward", .point = Api::ReportPoint::AsyncWorkForward, .text = &ReportLines });
	}
}
