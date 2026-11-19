#pragma once

#include "Features/Upscaling/PresentationConfig.h"

#include <cstdint>
#include <string>

// Serializes DLSS-G and FSR-FG ownership changes on the render thread. Feature
// load changes are applied together while the presenter's swap chain is torn down.
namespace FrameGen
{
	enum class FSRDeliveryState : int
	{
		kNever = -1,
		kTornDown = 0,
		kDelivered = 1
	};

	class Controller
	{
	public:
		static Controller* GetSingleton()
		{
			static Controller singleton;
			return &singleton;
		}

		/** @brief Reconciles the active frame-generation method. */
		void Reconcile();

		/** @brief Enables DLSS-G after its load transition has settled. */
		void EngageDLSSG();
		/** @brief Records a present-time fault teardown already requested by the host. */
		void NotifyFaultTeardownRequested();
		/** @brief Whether FSR frame generation can consume resources for this render frame. */
		[[nodiscard]] bool IsFSRPresenterReady() const;

		/** @brief The presentation this frame wants with a_method: its colour space, vsync and present mode. */
		static PresentationConfig Presentation(Method a_method);

	private:
		Controller() = default;

		enum class Phase : uint8_t
		{
			kIdle,
			kTransitioning
		};

		void StepPhaseCompletion();
		bool StepModeTeardown(Method a_target);
		void StepLoadState(Method a_target);
		void StepFSRDelivery(Method a_target);
		/** @brief Native: a replacement when DLSS-G's swap chain was made with another vsync than the frame wants. */
		void StepDLSSGVsync(Method a_target);
		/** @brief Writes a replacement with a_method to the presentation variable, and waits for its adoption before the switch completes. */
		void RequestPresentation(Method a_method, const char* a_reason);

		static const char* Name(Method a_method);

		Phase phase = Phase::kIdle;
		Method owner = Method::kNone;

		bool dlssgModeOn = false;

		// kNever until sl.fsr_g accepts its first state update.
		FSRDeliveryState fsrDelivered = FSRDeliveryState::kNever;
		uint32_t fsrDebugSigDelivered = 0;
		bool fsrHDRDelivered = false;
		// FFX bakes VSync into its wrapped swapchain.
		bool fsrWrapVsync = false;
		bool fsrVsyncRebakePending = false;
		bool faultRecoveryRequested = false;
		uint32_t phaseCompletionRetries = 0;
		// The presentation request a switch waits for (PresentationRevision::Request).
		uint32_t awaitedRequest = 0;
		// A fault teardown's presenter request, made outside the presentation variable.
		uint64_t awaitedSerial = 0;
	};
}
