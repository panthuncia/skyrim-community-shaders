#include "FrameGenController.h"

#include "../HDRDisplay.h"
#include "../Upscaling.h"
#include "FramePresenter.h"
#include "PresentationRevision.h"
#include "Streamline.h"

#include "../../Globals.h"
#include "../../RenderGraph/Engine/Native/Native.h"
#include "../../Utils/Game.h"

namespace FrameGen
{
	namespace
	{
		Method DesiredMethod()
		{
			auto& upscaling = globals::features::upscaling;
			// Not IsFrameGenerationActive: its surface is not ready while a replacement the controller asked for is in flight.
			if (!upscaling.IsFrameGenerationSelected())
				return Method::kNone;
			return upscaling.GetFrameGenMethod() == Upscaling::FrameGenMethod::kDLSSG
			           ? Method::kDLSSG
			           : Method::kFSR;
		}

		uint32_t FSRDebugSignature(const Upscaling::Settings& a_settings)
		{
			return (a_settings.fgDebugView ? 1u : 0u) |
			       (a_settings.fgDebugTearLines ? 2u : 0u) |
			       (a_settings.fgDebugPacingLines ? 4u : 0u) |
			       (a_settings.fgShowOnlyGenerated ? 8u : 0u);
		}

		struct Dims
		{
			uint32_t renderWidth;
			uint32_t renderHeight;
			uint32_t displayWidth;
			uint32_t displayHeight;
		};

		Dims CurrentDims(bool a_ignoreDynamicResolutionLock)
		{
			const auto display = float2{ (float)globals::game::graphicsState->screenWidth,
				(float)globals::game::graphicsState->screenHeight };
			const auto render = Util::ConvertToDynamic(display, a_ignoreDynamicResolutionLock);
			return { (uint32_t)render.x, (uint32_t)render.y, (uint32_t)display.x, (uint32_t)display.y };
		}

		bool IsHDRActive()
		{
			const auto& hdr = globals::features::hdrDisplay;
			return hdr.loaded && hdr.IsHDREnabledForFrame();
		}

	}

	PresentationConfig Controller::Presentation(Method a_method)
	{
		const auto& upscaling = globals::features::upscaling;
		// Native bakes the vsync into a presentation layer's swap chain, so DLSS-G's own vsync limit (SL-VSYNC-011) applies here.
		// Guest forces the sync interval per present instead (Hooks.cpp).
		const bool dlssgNoVsync = a_method == Method::kDLSSG && RenderGraph::Engine::Native::Active() && !Streamline::GetSingleton()->IsDLSSGVsyncSupported();
		return { .method = a_method, .hdr = IsHDRActive(), .vsync = upscaling.settings.vsync && !dlssgNoVsync, .tearing = upscaling.GetPresentModePreference() != 0 };
	}

	void Controller::RequestPresentation(Method a_method, const char* a_reason)
	{
		awaitedRequest = PresentationRevision::Request(Presentation(a_method), a_reason);
	}

	const char* Controller::Name(Method a_method)
	{
		switch (a_method) {
		case Method::kFSR:
			return "FSR-FG";
		case Method::kDLSSG:
			return "DLSS-G";
		default:
			return "off";
		}
	}

	void Controller::Reconcile()
	{
		// Wait for settings and hardware fallbacks to settle.
		if (!globals::features::upscaling.loaded ||
			!Streamline::GetSingleton()->IsFeatureSupportResolved() ||
			!globals::game::graphicsState)
			return;

		if (Upscaling::IsWindowUnusable())
			return;

		auto* streamline = Streamline::GetSingleton();

		if (faultRecoveryRequested) {
			if (phase == Phase::kTransitioning) {
				if (!streamline->IsDLSSGLoadSettled() || !streamline->IsFSRFGLoadSettled() ||
					streamline->IsDLSSGLoaded() || streamline->IsFSRFGLoaded())
					return;
				if (!ActivePresenter().Quiesce(Method::kFSR)) {
					logger::error("[FrameGen] fault cleanup deferred because command completion could not be proven");
					return;
				}
				StepPhaseCompletion();
			}
			if (phase == Phase::kIdle)
				faultRecoveryRequested = false;
			return;
		}

		const Method target = DesiredMethod();

		StepPhaseCompletion();
		if (!StepModeTeardown(target))
			return;
		StepLoadState(target);
		StepFSRDelivery(target);
		StepDLSSGVsync(target);
	}

	void Controller::StepDLSSGVsync(Method a_target)
	{
		// A presentation layer that keeps the vsync its swap chain was made with (native's) makes a change a replacement.
		if (a_target != Method::kDLSSG || phase != Phase::kIdle || owner != Method::kDLSSG || !Streamline::GetSingleton()->IsDLSSGLoaded())
			return;
		const auto layerVsync = ActivePresenter().LayerVsync();
		if (PresentationRevision::Pending() || !layerVsync)
			return;
		if (Presentation(Method::kDLSSG).vsync != *layerVsync)
			RequestPresentation(Method::kDLSSG, "DLSS-G vsync change");
	}

	void Controller::StepPhaseCompletion()
	{
		if (phase != Phase::kTransitioning)
			return;

		auto* sl = Streamline::GetSingleton();
		auto& presenter = ActivePresenter();
		// The adopted revision's swap chain has the config asked for, and the features' load changes ran with it.
		if (presenter.AdoptedSerial() < awaitedSerial || !PresentationRevision::Has(awaitedRequest) || !sl->IsDLSSGLoadSettled() ||
			!sl->IsFSRFGLoadSettled())
			return;

		if (sl->IsDLSSGLoaded()) {
			const auto dims = CurrentDims(false);
			if (!sl->SetDLSSGMode(false, dims.displayWidth, dims.displayHeight)) {
				if (++phaseCompletionRetries <= 120)
					return;
				logger::error("[FrameGen] SetDLSSGMode(false) failed {} times; forcing phase completion",
					phaseCompletionRetries);
			}
			owner = Method::kDLSSG;
		} else if (sl->IsFSRFGLoaded()) {
			owner = Method::kFSR;
		} else {
			owner = Method::kNone;
		}

		phaseCompletionRetries = 0;
		phase = Phase::kIdle;
		logger::info("[FrameGen] FG method switch settled - present owner: {}", Name(owner));

		presenter.Settled(owner);
	}

	bool Controller::StepModeTeardown(Method a_target)
	{
		auto* sl = Streamline::GetSingleton();
		if (!sl->IsDLSSGLoaded() && (dlssgModeOn || owner == Method::kDLSSG)) {
			dlssgModeOn = false;
			if (owner == Method::kDLSSG)
				owner = Method::kNone;
			logger::warn("[FrameGen] DLSS-G was unloaded before mode teardown - reconciled local state");
		}
		if (!sl->IsFSRFGLoaded() && (fsrDelivered == FSRDeliveryState::kDelivered || owner == Method::kFSR)) {
			fsrDelivered = FSRDeliveryState::kTornDown;
			fsrVsyncRebakePending = false;
			if (owner == Method::kFSR)
				owner = Method::kNone;
			logger::warn("[FrameGen] FSR-FG was unloaded before mode teardown - reconciled local state");
		}

		// Streamline requires DLSS-G to be disabled and drained before teardown.
		if (dlssgModeOn && a_target != Method::kDLSSG) {
			const auto dims = CurrentDims(false);
			if (!sl->SetDLSSGMode(false, dims.displayWidth, dims.displayHeight))
				return false;
			// A lost device has nothing left in flight to drain, and waiting on it can never
			// succeed, so deferring here spins forever -- 17.6k of these in one session even after
			// the wait itself was latched. Treat the mode as torn down and let the controller move
			if (!ActivePresenter().Quiesce(Method::kDLSSG)) {
				static bool deferralReported = false;
				if (!std::exchange(deferralReported, true))
					logger::error("[FrameGen] DLSS-G teardown deferred because device idle could not be proven");
				return false;
			}

			dlssgModeOn = false;
			logger::info("[FrameGen] DLSS-G interpolation off + device drained (leaving DLSS-G)");
		}

		if (fsrDelivered == FSRDeliveryState::kDelivered && a_target != Method::kFSR) {
			if (!ActivePresenter().Quiesce(Method::kFSR)) {
				// Once only: this re-enters every reconcile.
				static bool fsrDeferralReported = false;
				if (!std::exchange(fsrDeferralReported, true))
					logger::error("[FrameGen] FSR-FG teardown deferred because command completion could not be proven");
				return false;
			}
			const auto& s = globals::features::upscaling.settings;
			if (!sl->SetFSRFrameGen(false, fsrHDRDelivered,
					s.fgDebugView, s.fgDebugTearLines, s.fgDebugPacingLines, s.fgShowOnlyGenerated))
				return false;
			fsrDelivered = FSRDeliveryState::kTornDown;
			fsrVsyncRebakePending = false;
			if (owner == Method::kFSR)
				owner = Method::kNone;
			logger::info("[FrameGen] FSR-FG unwrapped (leaving FSR-FG)");
		}

		return true;
	}

	void Controller::StepLoadState(Method a_target)
	{
		if (phase != Phase::kIdle)
			return;

		auto* sl = Streamline::GetSingleton();
		const bool wantDLSSG = a_target == Method::kDLSSG;
		const bool wantFSRFG = a_target == Method::kFSR;

		if (sl->IsDLSSGLoaded() == wantDLSSG && sl->IsFSRFGLoaded() == wantFSRFG) {
			if (wantDLSSG && owner != Method::kDLSSG) {
				const auto dims = CurrentDims(false);
				if (!sl->SetDLSSGMode(false, dims.displayWidth, dims.displayHeight))
					return;
				owner = Method::kDLSSG;
				logger::info("[FrameGen] DLSS-G already loaded - registered + adopted as present owner");
			} else if (wantFSRFG && owner != Method::kFSR) {
				owner = Method::kFSR;
				logger::info("[FrameGen] FSR-FG already loaded - adopted as present owner");
			}
			return;
		}

		// One rule for both methods, as in the Streamline sample: the present mode follows the sync interval, never the
		// frame generation method. The replacement carries it, so no extra recreate is introduced here.
		sl->SetDLSSGDesiredLoaded(wantDLSSG);
		sl->SetFSRFGDesiredLoaded(wantFSRFG);
		RequestPresentation(a_target, "FG method switch");
		phase = Phase::kTransitioning;
		if (owner == Method::kDLSSG && !wantDLSSG)
			owner = Method::kNone;
		if (owner == Method::kFSR && !wantFSRFG)
			owner = Method::kNone;
		logger::info("[FrameGen] FG method switch requested: DLSS-G load={} FSR-FG load={} (swapchain recreate)",
			wantDLSSG, wantFSRFG);
	}

	void Controller::StepFSRDelivery(Method a_target)
	{
		auto& upscaling = globals::features::upscaling;
		auto* sl = Streamline::GetSingleton();
		const bool wantFSR = a_target == Method::kFSR;

		if (!wantFSR || phase != Phase::kIdle || !sl->IsFSRFGLoaded())
			return;

		// Present the new interval once before recreating the FFX-wrapped swapchain.
		if (fsrDelivered == FSRDeliveryState::kDelivered && upscaling.settings.vsync != fsrWrapVsync) {
			if (!fsrVsyncRebakePending) {
				fsrVsyncRebakePending = true;
			} else {
				fsrVsyncRebakePending = false;
				fsrWrapVsync = upscaling.settings.vsync;
				RequestPresentation(a_target, "FSR-FG vsync change");
			}
		} else {
			fsrVsyncRebakePending = false;
		}

		const uint32_t debugSig = FSRDebugSignature(upscaling.settings);
		const bool hdr = IsHDRActive();
		if (fsrDelivered == FSRDeliveryState::kDelivered && debugSig == fsrDebugSigDelivered && hdr == fsrHDRDelivered)
			return;

		const bool enableEdge = fsrDelivered != FSRDeliveryState::kDelivered;
		const bool hdrChanged = fsrDelivered == FSRDeliveryState::kDelivered && hdr != fsrHDRDelivered;

		const auto& s = upscaling.settings;
		if (sl->SetFSRFrameGen(true, hdr,
				s.fgDebugView, s.fgDebugTearLines, s.fgDebugPacingLines, s.fgShowOnlyGenerated)) {
			fsrDelivered = FSRDeliveryState::kDelivered;
			fsrDebugSigDelivered = debugSig;
			fsrHDRDelivered = hdr;
			owner = Method::kFSR;
			fsrWrapVsync = s.vsync;
			logger::info("[FrameGen] FSR-FG enable delivered - present owner: {}", Name(owner));

			// FFX installs its interpolation swapchain during vkCreateSwapchainKHR.
			if (enableEdge)
				RequestPresentation(a_target, "FSR-FG wrap");
			else if (hdrChanged)
				RequestPresentation(a_target, "FSR-FG HDR transfer change");
		}
	}

	bool Controller::IsFSRPresenterReady() const
	{
		if (phase != Phase::kIdle || DesiredMethod() != Method::kFSR || fsrDelivered != FSRDeliveryState::kDelivered ||
			!Streamline::GetSingleton()->IsFSRFGLoaded())
			return false;

		return fsrHDRDelivered == IsHDRActive() && ActivePresenter().Surface().ready;
	}

	void Controller::EngageDLSSG()
	{
		auto* sl = Streamline::GetSingleton();
		if (phase != Phase::kIdle || !sl->IsDLSSGLoaded())
			return;

		auto& upscaling = globals::features::upscaling;
		const auto& s = upscaling.settings;

		const auto dims = CurrentDims(true);

		const bool dynamic = s.dlssgDynamic;
		const bool useDynamic = dynamic && sl->IsDLSSGDynamicSupported();
		const bool useAuto = dynamic && !useDynamic;
		const uint32_t numFramesToGenerate = upscaling.GetFixedDLSSGMultiplier() - 1u;
		const float dynTargetFps = dynamic ? static_cast<float>(upscaling.GetTargetFrameRate()) : 0.0f;

		if (sl->SetDLSSGMode(true, dims.displayWidth, dims.displayHeight,
				numFramesToGenerate, useAuto, useDynamic, dynTargetFps))
			dlssgModeOn = true;
	}

	void Controller::NotifyFaultTeardownRequested()
	{
		awaitedSerial = ActivePresenter().RequestedSerial();
		dlssgModeOn = false;
		fsrDelivered = FSRDeliveryState::kTornDown;
		fsrVsyncRebakePending = false;
		faultRecoveryRequested = true;
		phase = Phase::kTransitioning;
	}

}
