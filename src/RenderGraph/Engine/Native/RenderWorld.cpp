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
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"
#include "Utils/Logging.h"

#include "Globals.h"
#include "State.h"

#include <cstdint>

namespace RenderGraph::Engine::Native
{
	namespace
	{
		template <class T>
		T& Global(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return *reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address());
		}

		template <class T>
		T& Field(const void* a_base, std::size_t a_offset)
		{
			return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_offset);
		}

		/** @brief A clear or copy the skeleton did not take: nothing else makes it in native. Logged once per what. */
		void Untaken(bool a_taken, const char* a_what)
		{
			if (a_taken)
				return;
			static Util::ReasonLog log("[ORG native] RenderWorld: the skeleton did not take a resolve function's clear or copy; it is not made");
			log.Report(a_what);
		}

		/**
		* @brief The shadow state the resolve functions leave for the engine code after RenderWorld (the first-person view, RenderPostProcess),
		* written as their setters would (observed over every native world render of river and interior: nothing else changes). Render
		* targets 1 and 2 unbound, slots 3 and 4 in restore mode, the full depth range, the alpha reference 128/255, and the state they set
		* and set back marked dirty (targets, viewport, depth mode, depth bias, blend, alpha test).
		*/
		void LeaveResolveState()
		{
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			shadow.renderTargets[1] = RE::RENDER_TARGET::kNONE;
			shadow.renderTargets[2] = RE::RENDER_TARGET::kNONE;
			shadow.setRenderTargetMode[3] = RE::BSGraphics::SetRenderTargetMode::SRTM_RESTORE;
			shadow.setRenderTargetMode[4] = RE::BSGraphics::SetRenderTargetMode::SRTM_RESTORE;
			shadow.viewPort.MaxDepth = 1.0f;
			// The engine marks the reference's change with 0x200 (CommonLib's DIRTY_ALPHA_ENABLE_TEST name).
			constexpr float kAlphaRef = 128.0f / 255.0f;
			if (shadow.alphaTestRef != kAlphaRef) {
				shadow.alphaTestRef = kAlphaRef;
				shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_ENABLE_TEST);
			}
			using F = RE::BSGraphics::ShaderFlags;
			shadow.stateUpdateFlags.set(F::DIRTY_RENDERTARGET, F::DIRTY_VIEWPORT, F::DIRTY_DEPTH_MODE, F::DIRTY_RASTER_DEPTH_BIAS, F::DIRTY_ALPHA_BLEND,
				F::DIRTY_ALPHA_TEST_REF);
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
			// mask epoch wants it: the stencil clear, the mask batch, and TAA's form 1 (ImageSpaceManager +0x1f0, +0x2c), which the post
			// chain reads and the water blend's test needs.
			if (DCLF::Features::Water::Indirect::MaskBatchWanted(a_accumulator->GetRuntimeData()->batchRenderer)) {
				Untaken(Skeleton::ClearDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, D3D11_CLEAR_STENCIL), "the water mask's stencil clear");
				DCLF::Features::Water::NativeMaskBatch(a_accumulator);
				if (const auto taa = Field<std::uintptr_t>(RE::ImageSpaceManager::GetSingleton(), 0x1f0))
					Field<std::int32_t>(reinterpret_cast<const void*>(taa), 0x2c) = 1;
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
			Untaken(Skeleton::CopyMain(), "the kMAIN copy");
			const bool blend = WorldPass::WaterBlendRuns(a_flags);
			WorldPass::SetRawWater(blend);
			DCLF::Features::Water::NativeSurfaceBatch(a_accumulator);
			if (blend)
				ImageSpace::NativeWaterBlend();
			// Deferred's hook on the SetCameraData after the water (PostWater_SetCamera): View Atmosphere's composite.
			if (globals::deferred->postWaterHookInstalled)
				globals::features::viewAtmosphere.CompositeAfterWater();
			// With ImageSpaceManager +0x168's effect active and flag 0x40, the state flush clears kMAIN_ONLY_ALPHA and the TAA mask.
			const auto effect = Field<std::uintptr_t>(RE::ImageSpaceManager::GetSingleton(), 0x168);
			if ((a_flags & 0x40) && effect && Field<bool>(reinterpret_cast<const void*>(effect), 0x8)) {
				constexpr float kZero[4]{};
				Untaken(Skeleton::ClearColour(RE::RENDER_TARGET::kTEMPORAL_AA_MASK, kZero), "the TAA mask's clear");
				Untaken(Skeleton::ClearColour(RE::RENDER_TARGET::kMAIN_ONLY_ALPHA, kZero), "kMAIN_ONLY_ALPHA's clear");
			}
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
		auto* const camera = CS_ENGINE_READ(Camera, RecordTime, RE::Main::WorldRootCamera());
		// The render mode the engine's effect shaders read (7: the world), and the camera the reflections' helper keeps.
		Global<std::uint32_t>(524994, 411475) = 7;
		Global<RE::NiCamera*>(528353, 415299) = camera;
		// Sun::UpdateOcclusionQueries is not called: the game side takes the sun query's answer in its capture (Sky's SunQuery.cpp).
		// StartAccumulating's render flags (p0 << 4 | 0x41), RenderWorld's ImageSpace calls, then the post-resolve's flags.
		PreResolve(accumulator, (a_firstPerson ? 0x10u : 0u) | 0x41u);
		ImageSpace::RenderWorldImageSpace();
		PostResolve(accumulator, (Global<bool>(513335, 391110) ? 1u : 0u) | 0x40u);
		LeaveResolveState();
		// ClearItemsList and RenderFirstPersonView_End are not called: in native nothing registers into the main accumulator, so its
		// lists are empty at both ends of the world render (observed over every world render of the test configs).
		// Sun::SubmitOcclusionQuery(1, ...): the sun's occlusion draw into CS's query slot, tagged with the snapshot it is drawn for.
		if (const auto& snapshot = Snapshot::Current()) {
			if (const auto* block = snapshot->Block<DCLF::Features::Sky::SunQueryBlock>())
				DCLF::Features::Sky::SubmitSunQuery(accumulator, DCLF::Features::Sky::SunQueryGeometry(*block), camera, snapshot->id);
		}
		Global<bool>(527795, 414748) = false;
		Global<bool>(527796, 414749) = false;
		globals::state->EndPerfEvent();
	}
}
