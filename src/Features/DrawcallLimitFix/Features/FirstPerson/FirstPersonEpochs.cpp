// The first-person view's two epochs and its report lines. The depth epoch has no epoch of its own. Its passes run
// inside the main depth commit. The colour epoch runs at Main::RenderFirstPersonView's accumulator render. Render thread.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "RenderGraph/Engine/Native/RenderScene.h"

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// The engine's first-person NiCamera, set up by FUN_1414cb020. Its translation is in the root's space, not world space.
		constexpr Engine::Offset kFirstPersonCamera{ { 415007 }, { 528062 } };
		// f1stPersonFarDepthRange and its ControlDriven variant, picked by RenderDepth's second argument.
		constexpr Engine::Offset kFarDepthRange{ { 388994 }, { 512174 } }, kFarDepthRangeControlDriven{ { 388997 }, { 512176 } };

		using ViewSubmit = IndirectDraws::ViewSubmit;

		// Builds VS_PerFrame (b12) for the first-person camera, as the native driver's game half took it after the first-person shift.
		// The eye comes from DrawWorld +0x00, which is in world space.
		bool CaptureCamera(FirstPersonFrame& a_f)
		{
			auto& f = a_f;
			ShadowCameras::Camera view;
			std::array<float, 3> eyeWorld{};
			// The engine's own skeleton (guest, and native's frozen-menu capture) leaves its camera in place.
			if (const auto& prepared = RenderGraph::Engine::Native::Scene::Current(); prepared.valid) {
				const auto& taken = prepared.firstPersonCamera;
				if (!taken.valid)
					return false;
				ShadowCameras::CameraMatrices(taken.world, taken.frustum, taken.port, view);
				eyeWorld = taken.eye;
			} else {
				auto* camera = *REL::Relocation<RE::NiCamera**>{ REL::Offset(kFirstPersonCamera) };
				if (!camera)
					return false;
				ShadowCameras::CameraMatrices(*camera, view);
				std::memcpy(eyeWorld.data(), REL::Relocation<const float*>{ REL::Offset(Engine::Address::kDrawWorldEye) }.get(), sizeof(eyeWorld));
			}
			const RE::NiPoint3 eye{ eyeWorld[0], eyeWorld[1], eyeWorld[2] };
			// The rows were sampled before the first-person shift, so the eye must be the world eye too.
			view.eye = { eye.x, eye.y, eye.z };
			// Jittered, like the engine's first-person draws. On the first frame the previous camera is the current one.
			const Draws::PreviousCamera current{ view.viewProj, eye };
			f.perFrame.fill(std::byte{ 0 });
			Draws::PerFrameBlock(view, Draws::FrameBlockNow(), true, f.havePrevious ? &f.previous : &current, f.perFrame.data());
			f.viewProj = view.viewProj;
			f.eye = eye;
			f.previous = current;
			f.havePrevious = true;
			return true;
		}
	}

	void TakeDepthRange(bool a_controlDriven)
	{
		// The depth range is a revision input. Setting it here keeps the change outside any epoch's execution.
		auto& draws = IndirectDraws::Get();
		if (draws.ViewState(kView))
			draws.SetViewDepthRange(kView, *REL::Relocation<float*>{ REL::Offset(a_controlDriven ? kFarDepthRangeControlDriven : kFarDepthRange) });
	}

	void ExecuteDepth(const Api::ViewPointContext&)
	{
		DCLF_ZONE("CS.DCLF.ExecuteFirstPersonDepth");
		auto& draws = IndirectDraws::Get();
		if (!draws.ViewState(kView))
			return;
		auto& f = FrameState();
		const std::uint32_t frameNumber = Scene::FrameView::Get().SceneStep();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		if (!Extensions::ViewRegistered(kView) || !f.latch || f.latchFrame != frameNumber)
			return skip(0);
		const auto set = Scene::FrameView::Get().FrameClaims();
		if (!set || !(set->drawn & kSetFirstPerson))
			return skip(0);
		if (!draws.ViewCovered(kView))
			return skip(1);
		// Main::Draw sets the camera up before RenderDepth, so it is already this frame's here.
		if (!CaptureCamera(f))
			return skip(2);
		// The passes are committed into the main depth commit. If that did not commit, nothing is drawn.
		IndirectDraws::ViewListed listed;
		switch (draws.TakeViewInputs(kView, IndirectDraws::ViewInputsFrom::DepthThisFrame, listed)) {
		case IndirectDraws::ViewInputsTaken::Stale:
			return skip(3);
		case IndirectDraws::ViewInputsTaken::NoTarget:
			return skip(4);
		case IndirectDraws::ViewInputsTaken::Taken:
			break;
		}
		const std::array<Draws::ViewFace, 1> faces{ Draws::ViewFace{ true, std::span<const std::byte>(f.perFrame.data(), f.perFrame.size()), f.viewProj, f.eye } };
		switch (draws.SubmitViewEpoch(kView, 0, listed, faces)) {
		case ViewSubmit::NoRevision:
			return skip(1);
		case ViewSubmit::NoOwners:
			return skip(4);
		case ViewSubmit::Failed:
			logger::error("[DCLF] the first-person depth epoch's commit failed");
			return skip(5);
		case ViewSubmit::Submitted:
			break;
		}
		++f.depthEpochs;
		f.depthFrame = frameNumber;
	}

	void ExecuteColour()
	{
		DCLF_ZONE("CS.DCLF.ExecuteFirstPersonColour");
		auto& draws = IndirectDraws::Get();
		if (!draws.ViewState(kView))
			return;
		auto& f = FrameState();
		const std::uint32_t frameNumber = Scene::FrameView::Get().SceneStep();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		if (!Extensions::ViewRegistered(kView) || !f.latch || f.latchFrame != frameNumber || !draws.ViewCovered(kView))
			return;
		// The colour epoch draws what this frame's depth epoch culled.
		if (f.depthFrame != frameNumber)
			return skip(6);
		IndirectDraws::ViewListed listed;
		if (draws.TakeViewInputs(kView, IndirectDraws::ViewInputsFrom::ColourThisFrame, listed) != IndirectDraws::ViewInputsTaken::Taken)
			return skip(3);
		switch (draws.SubmitViewEpoch(kView, 1, listed, {})) {
		case ViewSubmit::NoRevision:
			return skip(1);
		case ViewSubmit::NoOwners:
			return skip(4);
		case ViewSubmit::Failed:
			logger::error("[DCLF] the first-person colour epoch failed");
			return skip(5);
		case ViewSubmit::Submitted:
			break;
		}
		++f.colourEpochs;
	}

	std::string Report()
	{
		auto& draws = IndirectDraws::Get();
		const auto* state = draws.ViewState(kView);
		if (!state)
			return {};
		auto& f = FrameState();
		const auto counts = PassCapture::Get().TakeViewCounts(kView);
		const bool on = Extensions::ViewRegistered(kView);
		if (!f.frames && !on && !counts.registered)
			return {};
		const auto& s = f.skipped;
		std::string text = fmt::format("[DCLF] first person (T7e): {} frames, {} the engine drew first person (latch); epochs: {} depth (in the Z-prepass epoch), {} colour; not drawn: "
									   "{} not the view's frame, {} not covered, {} without the camera, {} stale inputs, {} without resources, {} failed{}, {} colour without the "
									   "frame's depth epoch{}; targets colour {} motion {} depth {}, {} first-person pipeline slots, {} pipelines ready, depth range {:.4f}\n",
			f.frames, f.latched, f.depthEpochs, f.colourEpochs, s[0], s[1], s[2], s[3], s[4], s[5], s[5] ? " <- EPOCH FAILED" : "", s[6],
			s[6] ? " <- NO DEPTH EPOCH" : "", static_cast<int>(state->targets.colour), static_cast<int>(state->targets.colour2), static_cast<int>(state->targets.depth), f.slots,
			f.pipelinesReady, draws.ViewDepthRange(kView));
		const auto particles = FrameValues::TakeFirstPersonPaletteStats();
		text += fmt::format("[DCLF] first person's forward part (T7e): {:.1f} forward members a frame (blended Lighting, effects, particles; the main set's pipelines "
							"through their state classes' colour signatures, sorted back to front); particle members: {} expansions of {} particles faced to the first-person eye, "
							"their snapshots' bound at most {:.0f} units from it{}{}\n",
			f.frames ? static_cast<double>(f.forwardMembers) / static_cast<double>(f.frames) : 0.0, particles.writes, particles.units, particles.farthest,
			particles.farthest > 2000.0f ? " <- FIRST-PERSON PARTICLE SPACE" : "",
			state->forwardCompatible.load(std::memory_order_acquire) ? "" : "; the main depth format is not kPOST_ZPREPASS_COPY's: forward members stay the engine's");
		// memberKept counts members the engine drew itself. It should be 0 on every covered frame.
		if (counts.memberKept)
			text += fmt::format("[DCLF] first person's registrations (T7e, the first-person accumulator and RenderDepth's block): {} passes registered into the "
								"first-person accumulator, {} member passes withheld, {} member passes the engine drew{}\n",
				counts.registered, counts.withheld, counts.memberKept,
				counts.memberKept && f.depthEpochs ? " <- FIRST PERSON" : (counts.withheld || counts.registered ? " <- OK" : ""));
		const std::uint32_t depthFrame = f.depthFrame;
		f.frames = f.latched = f.depthEpochs = f.colourEpochs = f.forwardMembers = 0;
		f.skipped = {};
		f.depthFrame = depthFrame;
		return text;
	}
}
