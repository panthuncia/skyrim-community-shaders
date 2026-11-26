// Main::RenderWorld in native, driven by CS. See RenderWorld.h. The engine's order (AE 1.6.1170 0x1414cdaa0, SE 1.5.97
// 0x1412e3ec0): the world node hidden, the accumulator started (its pre-resolve runs inside), the colour targets unbound, the
// ImageSpace calls, the post-resolve, then the sun's query draw.

#include "RenderGraph/Engine/Native/RenderWorld.h"

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

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

		/** @brief The engine's functions RenderWorld calls that CS still calls. */
		struct Engine
		{
			REL::Relocation<void (*)(RE::NiCamera*, void*, std::uint32_t)> startAccumulating{ REL::RelocationID(99790, 106437) };
			REL::Relocation<void (*)(RE::NiCamera*, void*, std::uint32_t)> postResolve{ REL::RelocationID(99791, 106438) };  // FinishAccumulatingPostResolveDepth
			REL::Relocation<void (*)(void*, std::uint32_t, std::int32_t, std::uint32_t, bool)> setTarget{ REL::RelocationID(75646, 77453) };
		};

		const Engine& Functions()
		{
			static const Engine engine;
			return engine;
		}

		// BSShaderAccumulator +0x5C: set while the main accumulator renders the world; its batches switch the depth mode by it.
		constexpr std::size_t kAccumulatorRendering = 0x5C;

		/** @brief As the engine's depth mode setter: the dirty flag follows whether the previous mode differs. */
		void SetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode)
		{
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			if (shadow.depthStencilDepthMode == a_mode)
				return;
			shadow.depthStencilDepthMode = a_mode;
			if (shadow.depthStencilDepthModePrevious == a_mode)
				shadow.stateUpdateFlags.reset(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
			else
				shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
		}
	}

	void RenderWorld(bool a_firstPerson)
	{
		ZoneScopedN("CS.Native.RenderWorld");
		globals::state->BeginPerfEvent("World");
		// CS's points inside read the render state from CS's frame (WorldPass), not the shadow state.
		const WorldPass::RenderWorldScope worldPass;
		const auto& engine = Functions();
		auto* const accumulator = Global<std::byte*>(528064, 415009);
		auto* const camera = CS_ENGINE_READ(Camera, RecordTime, RE::Main::WorldRootCamera());
		auto& targets = Global<std::byte>(524970, 411451);  // the render target manager
		// The render mode the effect shaders and RenderSSAO read (7: the world), and the camera the reflections' helper keeps.
		Global<std::uint32_t>(524994, 411475) = 7;
		Global<RE::NiCamera*>(528353, 415299) = camera;
		// Sun::UpdateOcclusionQueries is not called: the game side takes the sun query's answer in its capture (Sky's SunQuery.cpp).
		// The node the engine hides while it renders the world (restored at the end).
		auto* const hidden = Global<RE::NiAVObject*>(528086, 415031);
		const bool wasHidden = hidden->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		hidden->GetFlags().set(RE::NiAVObject::Flag::kHidden);
		accumulator[kAccumulatorRendering] = std::byte{ 1 };
		SetDepthMode(RE::BSGraphics::DepthStencilDepthMode::kTest);
		// SetCameraData for the world camera and the accumulator's pre-resolve with its CS points (Deferred, DCLF, the sky, the water).
		engine.startAccumulating(camera, accumulator, (a_firstPerson ? 0x10u : 0u) | 0x41u);
		// No colour targets bound (and the snow pass's two more), as the engine leaves them for the ImageSpace calls.
		const bool improvedSnow = Global<bool>(513332, 391105);
		for (std::uint32_t slot = 0; slot < (improvedSnow ? 6u : 4u); ++slot)
			engine.setTarget(&targets, slot, -1, 3, true);
		ImageSpace::RenderWorldImageSpace();
		engine.postResolve(camera, accumulator, (Global<bool>(513335, 391110) ? 1u : 0u) | 0x40u);
		accumulator[kAccumulatorRendering] = std::byte{ 0 };
		// ClearItemsList and RenderFirstPersonView_End are not called: in native nothing registers into the main accumulator, so its
		// lists are empty at both ends of the world render (observed over every world render of the test configs).
		// Sun::SubmitOcclusionQuery(1, ...): the sun's occlusion draw into CS's query slot, tagged with the snapshot it is drawn for.
		if (const auto& snapshot = Snapshot::Current()) {
			if (const auto* block = snapshot->Block<DCLF::Features::Sky::SunQueryBlock>())
				DCLF::Features::Sky::SubmitSunQuery(reinterpret_cast<RE::BSShaderAccumulator*>(accumulator), DCLF::Features::Sky::SunQueryGeometry(*block), camera, snapshot->id);
		}
		Global<bool>(527795, 414748) = false;
		Global<bool>(527796, 414749) = false;
		if (!wasHidden)
			hidden->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
		globals::state->EndPerfEvent();
	}
}
