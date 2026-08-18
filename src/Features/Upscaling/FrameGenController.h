#pragma once

#include <atomic>
#include <cstdint>

// Serializes DLSS-G and FSR-FG ownership changes on the render thread. Feature
// load changes are applied together while DXVK's swapchain is torn down.
namespace FrameGen
{
	enum class Method : uint8_t
	{
		kNone,
		kFSR,
		kDLSSG,
	};

	class Controller
	{
	public:
		struct RuntimeState
		{
			std::atomic<bool> dlssgDesiredLoaded{ false };
			std::atomic<bool> dlssgCurrentlyLoaded{ false };
			std::atomic<bool> fsrfgDesiredLoaded{ false };
			std::atomic<bool> fsrfgCurrentlyLoaded{ false };
			std::atomic<bool> fsrfgOwnsPresent{ false };
		};

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
		RuntimeState& GetRuntimeState() { return runtimeState; }

	private:
		Controller() = default;

		enum class Phase : uint8_t
		{
			kIdle,
			kTransitioning
		};

		enum class FSRDelivery : uint8_t
		{
			kPending,
			kDelivered,
		};

		void StepPhaseCompletion();
		bool StepModeTeardown(Method a_target);
		void StepLoadState(Method a_target);
		void StepFSRDelivery(Method a_target);

		static const char* Name(Method a_method);

		Phase phase = Phase::kIdle;
		Method owner = Method::kNone;

		bool dlssgModeOn = false;

		FSRDelivery fsrDelivery = FSRDelivery::kPending;
		uint32_t fsrDebugSigDelivered = 0;
		bool fsrHDRDelivered = false;
		// FFX bakes VSync into its wrapped swapchain.
		bool fsrWrapVsync = false;
		bool fsrVsyncRebakePending = false;
		bool faultRecoveryRequested = false;
		RuntimeState runtimeState;
	};
}
