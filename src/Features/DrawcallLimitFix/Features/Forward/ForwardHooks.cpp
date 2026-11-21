// The forward epoch's engine hooks: the main pass's blended draw (FUN_1414b39d0) and Main::Draw's refraction point.
// The blended draw is also where CS's deferred+water stage commits in native mode, every world frame, even when the
// forward epoch does not run.

#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Globals.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <cstdint>
#include <utility>

namespace DCLF::Features::Forward
{
	namespace
	{
		namespace Address
		{
			inline constexpr Engine::Offset kBlendedDraw{ { 106585 }, { 99940 } };  // FUN_1414b39d0
			inline constexpr Engine::Offset kBlendedUnderWater{ { 106584, 0x51 }, { 99939, 0x54 } };  // Call sites in FUN_1414b3390.
			inline constexpr Engine::Offset kBlendedAfterWater{ { 106584, 0x543 }, { 99939, 0x551 } };
			inline constexpr Engine::Offset kRefractionCheck{ { 36559, 0x870 }, { 35560, 0x860 }, { 36559, 0x88d } };
			inline constexpr Engine::Offset kGroupFiveCount{ { 106598 }, { 99953 } };  // FUN_1414b4840
			inline constexpr Engine::Offset kRefractionNormalsBegun{ { 415291 }, { 528345 } };  // Byte. Set once the normals are cleared this frame.
			inline constexpr Engine::Offset kRefractionNormalsDrawn{ { 415292 }, { 528346 } };  // Byte. ISRefraction's IsActive reads it.
			inline constexpr Engine::Offset kClearGroup{ { 106600 }, { 99955 } };  // FUN_1414b4ae0. Clears a group's pass list without drawing.
		}

		Api::TriggerId blendedDraw;

		// Set the flags FUN_1414b4860 sets, so the engine's later refraction draws add to DCLF's instead of clearing,
		// and ISRefraction runs. The engine clears both flags when it renders.
		void MarkRefractionNormals()
		{
			static REL::Relocation<std::uint8_t*> normalsBegun{ REL::Offset(Address::kRefractionNormalsBegun) };
			static REL::Relocation<std::uint8_t*> normalsDrawn{ REL::Offset(Address::kRefractionNormalsDrawn) };
			*normalsBegun = 1;
			*normalsDrawn = 1;
		}

		// N == 0 is the call used when the camera is under water, before the water. N == 1 is the one after the water.
		template <int N>
		struct Accumulator_BlendedDraw
		{
			static void thunk(void* a_accumulator, std::uint32_t a_renderFlags)
			{
				// DCLF's blended objects draw before all of the engine's. The same function draws other accumulators, so check for the main one.
				static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
				const bool main = a_accumulator && a_accumulator == *mainAccumulator;
				if (main) {
					// Under water, the water comes after this point, so the sun stage's water steps draw it.
					if constexpr (N == 0) {
						static bool logged = false;
						if (!std::exchange(logged, true))
							logger::info("[DCLF] forward: the main pass's blended draw came before the water (the camera is under water)");
					}
					(void)FrameAnchors::TapPoint(::RenderGraph::Frame::SegmentId::Deferred);
				}
				if (main) {
					Extensions::TriggerScope trigger(blendedDraw);
					IndirectDraws::Get().NoteBlendedFrameParams(globals::game::frameBufferCached.GetFrameParams().z);
					if (SubmitForwardEpoch())
						MarkRefractionNormals();
				}
				func(a_accumulator, a_renderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// AE 0x140644d20, where Main::Draw reads group 5's count to decide whether to draw refraction normals.
		// Once DCLF has begun the normals, group 6 would draw here too, which the engine never does. So clear it first.
		struct Main_Draw_RefractionCheck
		{
			static std::uint32_t thunk(void* a_accumulator)
			{
				if (a_accumulator && RefractionDrawnThisFrame()) {
					using ClearGroup = void (*)(void*, std::int32_t);
					static REL::Relocation<ClearGroup> clearGroup{ REL::Offset(Address::kClearGroup) };
					clearGroup(a_accumulator, 6);
				}
				return func(a_accumulator);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void PreflightHooks()
	{
		HookSites::RequireCall("the main pass's blended draw (under water)", REL::Offset(Address::kBlendedUnderWater).address());
		HookSites::RequireCall("the main pass's blended draw (after water)", REL::Offset(Address::kBlendedAfterWater).address());
		HookSites::RequireCall("Main::Draw's refraction point", REL::Offset(Address::kRefractionCheck).address());
	}

	void InstallHooks(Api::InstallContext& a_context)
	{
		blendedDraw = a_context.FindTrigger("blended.draw");
		// Chained onto whatever the calls already hold.
		stl::write_thunk_call<Accumulator_BlendedDraw<0>>(REL::Offset(Address::kBlendedUnderWater).address());
		stl::write_thunk_call<Accumulator_BlendedDraw<1>>(REL::Offset(Address::kBlendedAfterWater).address());
		stl::write_thunk_call<Main_Draw_RefractionCheck>(REL::Offset(Address::kRefractionCheck).address());
	}
}
