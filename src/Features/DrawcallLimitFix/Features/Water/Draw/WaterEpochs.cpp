// The water's two DCLF epochs (docs/development/plans/dclf-water-indirect.md, section 3.5): water.mask at the engine's mask batch
// and water.surface at its water batch. Each shares main.colour's shape and draws one deferred range of what this frame's colour
// commit culled (Core/Render/Passes/DeferredRanges.cpp).
// - The triggers are WaterBatches.cpp's batch thunks. Each submits its epoch before the engine runs the batch, so the batch hook
//   draws the water DCLF does not claim right after it, at the engine's point.
// - The recipe ends CS's open stage just before each epoch and opens the next (RecipeTable.h: water, post-water, sun water).
// - Geometry group 10's batch, just before the water batch, triggers no epoch. Its water (the wading surface) is the batch hook's, before DCLF's surface.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "RenderGraph/FrameAnchors.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace DCLF::Features::Water::Indirect
{
	namespace
	{
		struct WaterEpochSite
		{
			std::string_view epochId;
			std::string_view triggerId;
			Api::TriggerId trigger;
			std::uint32_t epoch = Draws::kNoEpoch;  // resolved at the first batch, after InstallFeatures
			bool resolved = false;
		};
		// The mask's and the surface's, by View.
		std::array<WaterEpochSite, 2> waterEpochSites{ { { "water.mask", "water.mask-batch" }, { "water.surface", "water.surface-batch" } } };

		// BSShaderAccumulator's batch renderer, which FinishAccumulating asks for passes (SE and AE alike).
		constexpr std::size_t kBatchRendererOffset = 0x130;

		WaterEpochSite& WaterSiteOf(View a_view) { return waterEpochSites[a_view == View::Mask ? 0 : 1]; }

		std::uint32_t WaterEpochOfSite(WaterEpochSite& a_site)
		{
			if (!std::exchange(a_site.resolved, true))
				a_site.epoch = Draws::EpochById(a_site.epochId);
			return a_site.epoch;
		}

		void WaterEpochReportLines(std::string& a_text)
		{
			const auto unchosen = Draws::TakeDeferredRunsWithoutChoice();
			for (auto& site : waterEpochSites) {
				const std::uint32_t epoch = WaterEpochOfSite(site);
				if (epoch == Draws::kNoEpoch || Draws::EpochTraitsOf(epoch).dormant)
					continue;
				const auto f = IndirectDraws::Get().TakeSharedEpochStats(epoch);
				a_text += fmt::format("[DCLF] {} epoch: reached {} frames, {} submitted; not submitted: {} without this frame's colour epoch, {} without a recording{}, "
									  "{} with no water member, {} failed{}\n",
					site.epochId, f.reached, f.epochs, f.noSharedPayload, f.noRecording, f.noRecording ? " <- NO WATER RECORDING" : "", f.empty, f.failed,
					f.failed ? " <- EPOCH FAILED" : "");
			}
			if (unchosen)
				a_text += fmt::format("[DCLF] water epochs: {} runs found no target set chosen by their commit and drew no water <- DEFERRED TARGETS\n", unchosen);
		}
	}

	void RegisterEpochs(Api::InstallContext& a_context)
	{
		for (auto& site : waterEpochSites)
			site.trigger = a_context.FindTrigger(site.triggerId);
		a_context.RegisterReportLines({ .name = "water epochs", .point = Api::ReportPoint::AsyncWorkForward, .text = &WaterEpochReportLines });
	}

	bool MaskBatchWanted(const void* a_batchRenderer)
	{
		static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
		const auto* accumulator = static_cast<const std::byte*>(*mainAccumulator);
		if (!accumulator || !a_batchRenderer || *reinterpret_cast<void* const*>(accumulator + kBatchRendererOffset) != a_batchRenderer ||
			!FrameAnchors::IsRenderOwner())
			return false;
		if (RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
			return false;
		auto& site = WaterSiteOf(View::Mask);
		const std::uint32_t epoch = WaterEpochOfSite(site);
		return site.trigger && epoch != Draws::kNoEpoch && !Draws::EpochTraitsOf(epoch).dormant;
	}

	bool ReachBatch(View a_view, const void* a_accumulator, void** a_cpuRun, std::uint32_t* a_cpuVariant)
	{
		if (a_cpuRun)
			*a_cpuRun = nullptr;
		// The main camera's batch only. Cube map faces and other accumulators run the same batches.
		static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
		if (!a_accumulator || a_accumulator != *mainAccumulator || !FrameAnchors::IsRenderOwner())
			return false;
		if (RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
			return false;
		// A CPU-driven frame draws claimed water in the water family's anchor pass, so the stage needs no epoch of its own.
		if (a_cpuRun && a_cpuVariant) {
			std::uint32_t variant = 0;
			if (void* run = Draws::TakeCpuRange(a_view == View::Mask ? 0u : 1u, variant)) {
				*a_cpuRun = run;
				*a_cpuVariant = variant;
				return true;
			}
		}
		auto& site = WaterSiteOf(a_view);
		const std::uint32_t epoch = WaterEpochOfSite(site);
		if (!site.trigger || epoch == Draws::kNoEpoch)
			return false;
		DCLF_ZONE("CS.DCLF.Water.Epoch");
		Extensions::TriggerScope trigger(site.trigger);
		return Draws::SubmitDeferredEpoch(epoch, a_view == View::Mask ? 0u : 1u);
	}
}
