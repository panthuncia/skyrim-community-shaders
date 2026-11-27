// Main::RenderWorld in native, driven by CS. See RenderWorld.h. The engine's order (AE 1.6.1170 0x1414cdaa0, SE 1.5.97
// 0x1412e3ec0): the accumulator started (its pre-resolve runs inside), the colour targets unbound, the ImageSpace calls, the
// post-resolve, then the sun's query draw. CS makes each CS point inside the resolve functions (SE 99938 / 99939, AE 106583 / 106584)
// in their order, with their clears and their copy, and calls no engine function.

#include "RenderGraph/Engine/Native/RenderWorld.h"

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"
#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"
#include "Features/ViewAtmosphere.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Native/EngineAccess.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "Snapshot/Publication.h"
#include "Utils/Logging.h"

#include "Globals.h"
#include "State.h"

#include <cstdint>

namespace RenderGraph::Engine::Native
{
	namespace
	{
		using Access::Global;

		/** @brief The resolve functions' clears and copies the skeleton did not take. */
		Access::UntakenLog& ResolveUntaken()
		{
			static Access::UntakenLog log("[ORG native] RenderWorld: the skeleton did not take a resolve function's clear or copy; it is not made");
			return log;
		}

		/** @brief The pre-resolve (SE 99938, AE 106583) for the main accumulator with render flags a_flags. */
		void PreResolve(RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
		{
			{
				// The first depth shift window: the opaque batches start.
				const WorldPass::ShiftWindow window;
				globals::deferred->WorldStart();
			}
			// The sky techniques' batch and the cloud group, outside a window.
			DCLF::Features::Sky::MainBatch(a_accumulator, a_flags);
			// The second window, to the end: the opaque batches end, the water mask, the reflections.
			const WorldPass::ShiftWindow window;
			globals::deferred->WorldOpaqueEnd(nullptr);
			// The water mask block (SE +0x3C5, AE +0x3B4) runs when the batch renderer holds mask passes, which in native means when DCLF's
			// mask epoch wants it: the stencil clear, the mask batch, and TAA's form 1 (the engine's ImageSpaceManager +0x1f0 +0x2c, CS's
			// WorldPass::TaaForm), which the post chain reads and the water blend's test needs.
			if (DCLF::Features::Water::Indirect::MaskBatchWanted(a_accumulator->GetRuntimeData()->batchRenderer)) {
				ResolveUntaken()(Skeleton::ClearDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, D3D11_CLEAR_STENCIL), "the water mask's stencil clear");
				DCLF::Features::Water::NativeMaskBatch(a_accumulator);
				WorldPass::SetTaaForm(1);
			}
			// With flag 0x40, the reflections pass.
			if (a_flags & 0x40)
				ImageSpace::NativeReflections();
		}

		/** @brief The post-resolve (SE 99939, AE 106584) for the main accumulator with render flags a_flags: one depth shift window. */
		void PostResolve(RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
		{
			const WorldPass::ShiftWindow window;
			// 2 when the camera is under water (TESWaterSystem's state): the blended draw then comes before the water.
			const bool underwater = Global<std::int32_t>(513267, 391008) == 2;
			if (underwater)
				DCLF::Features::Forward::NativeBlendedDraw(false);
			// SetRenderTarget(0, kMAIN, SRTM_FORCE_COPY_RESTORE) with kMAIN bound copies it into its copy.
			ResolveUntaken()(Skeleton::CopyMain(), "the kMAIN copy");
			const bool blend = WorldPass::WaterBlendRuns(a_flags);
			WorldPass::SetRawWater(blend);
			DCLF::Features::Water::NativeSurfaceBatch(a_accumulator);
			if (blend)
				ImageSpace::NativeWaterBlend();
			// Deferred's hook on the SetCameraData after the water (PostWater_SetCamera): View Atmosphere's composite.
			if (globals::deferred->postWaterHookInstalled)
				globals::features::viewAtmosphere.CompositeAfterWater();
			// The engine's state flush clears kMAIN_ONLY_ALPHA and the TAA mask here when ImageSpaceManager +0x168's effect
			// (ApplyReflections) is active. CS keeps that effect off on the game side (Snapshot/SideEffects), so native never clears.
			if (!underwater)
				DCLF::Features::Forward::NativeBlendedDraw(true);
		}
	}

	void RenderWorld(bool a_firstPerson)
	{
		ZoneScopedN("CS.Native.RenderWorld");
		globals::state->BeginPerfEvent("World");
		// CS's points inside read the render state from CS's frame (WorldPass), not the shadow state.
		const WorldPass::RenderWorldScope worldPass;
		auto* const accumulator = Global<RE::BSShaderAccumulator*>(528064, 415009);
		// Not written: the render mode (411475, 7 for the world), read only by the engine's RenderSSAO and effect shader setup, which
		// native does not run; and the reflections' camera (415299), which Main::RenderScene already set to this camera.
		// Sun::UpdateOcclusionQueries is not called: the game side takes the sun query's answer in its capture (Sky's SunQuery.cpp).
		// StartAccumulating's render flags (p0 << 4 | 0x41), RenderWorld's ImageSpace calls, then the post-resolve's flags.
		PreResolve(accumulator, (a_firstPerson ? 0x10u : 0u) | 0x41u);
		ImageSpace::RenderWorldImageSpace();
		PostResolve(accumulator, (Global<bool>(513335, 391110) ? 1u : 0u) | 0x40u);
		// The shadow state the resolve functions leave (RT1 and RT2 unbound, slots 3 and 4 in restore mode, the full depth range, the alpha
		// reference 128/255) is not written: no engine code that reads it runs after in native (the native post chain sets every target).
		// ClearItemsList and RenderFirstPersonView_End are not called: in native nothing registers into the main accumulator, so its
		// lists are empty at both ends of the world render (observed over every world render of the test configs).
		// Sun::SubmitOcclusionQuery(1, ...): the sun's occlusion draw into CS's query slot, tagged with the snapshot it is drawn for.
		if (const auto& snapshot = Snapshot::Current()) {
			if (const auto* block = snapshot->Block<DCLF::Features::Sky::SunQueryBlock>())
				DCLF::Features::Sky::SubmitSunQuery(accumulator, *block, snapshot->id);
		}
		// Not written: Main::RenderWorld's clears of 414748 and 414749, which nothing reads (Main::Draw's setup sets them, no xref reads them).
		globals::state->EndPerfEvent();
	}
}
