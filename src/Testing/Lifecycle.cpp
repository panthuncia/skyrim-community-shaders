// The load milestones and phases, and the phase timeouts.
#include "Testing/Internal.h"
#include "Testing/SwitchRegistry.h"

#include "Globals.h"
#include "State.h"

namespace Testing
{
	namespace
	{
		// Consecutive ready frames before the world counts as ready.
		constexpr std::uint32_t kReadyFrames = 30;

		// Lifecycle state, guarded by the run's lock.
		bool inLoad = false;             // a load started and the world is not ready yet
		bool loadingScreenSeen = false;  // this load showed the loading menu
		bool worldReady = false;
		std::uint32_t readyFrames = 0;
		double loadTimerStart = -1.0;  // when the wait for world-ready started; below zero: none
		bool worldFrame = false;       // the last Present was a world frame (render thread only)

		// FPS over the last second.
		double fpsWindowStart = 0.0;
		std::uint32_t fpsWindowFrames = 0;

		/** @brief Whether the configuration waits for a load: then the load timeout applies. */
		bool LoadExpected(const Config& a_config)
		{
			const auto isLoadMilestone = [](std::string_view a_name) { return a_name == "load-start" || a_name == "loading-screen" || a_name == "world-ready"; };
			if (isLoadMilestone(a_config.expectReach))
				return true;
			return std::ranges::any_of(a_config.steps, [&](const Step& a_step) { return isLoadMilestone(a_step.at); });
		}

		/** @brief The phase from the milestones and the load state. Caller holds the lock. */
		void UpdatePhase(RunState& a_run)
		{
			if (!a_run.outcome.empty()) {
				a_run.phase = "exiting";
				return;
			}
			if (!FindMilestone("data-loaded"))
				a_run.phase = "boot";
			else if (!FindMilestone("shaders-ready"))
				a_run.phase = "compiling";
			else if (inLoad)
				a_run.phase = "loading";
			else if (worldReady)
				a_run.phase = "world";
			else
				a_run.phase = "menu";
		}

		/** @brief Starts a load: load-start recorded, the world no longer ready. Caller holds the lock. */
		void BeginLoad(std::uint32_t a_frame)
		{
			if (inLoad)
				return;
			inLoad = true;
			loadingScreenSeen = false;
			worldReady = false;
			readyFrames = 0;
			loadTimerStart = Now();
			RecordMilestone("load-start", a_frame);
		}

		std::string LastMilestone(const RunState& a_run) { return a_run.occurrences.empty() ? std::string("none") : a_run.occurrences.back().name; }
	}

	void OnSkseMessage(std::uint32_t a_type)
	{
		auto& run = Run();
		switch (a_type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			// Every feature's PostPostLoad ran: DCLF's feature switches are registered if it installed.
			SwitchRegistry::DumpIfAsked();
			if (Configured())
				RecheckSwitches();
			return;
		case SKSE::MessagingInterface::kDataLoaded:
			{
				SwitchRegistry::CheckPendingReads();
				std::vector<std::string> missing;
				{
					std::scoped_lock lock(run.mutex);
					if (!run.configured)
						return;
					RecordMilestone("data-loaded", run.frame);
					UpdatePhase(run);
					for (const auto& step : run.config.steps)
						if (!step.command.empty() && !HasCommand(step.command))
							missing.push_back(step.command);
				}
				for (const auto& name : missing)
					logger::warn("[TEST] a step runs '{}', which no one registered; that step will fail", name);
				return;
			}
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			{
				std::scoped_lock lock(run.mutex);
				if (!run.configured)
					return;
				BeginLoad(run.frame);
				UpdatePhase(run);
				return;
			}
		default:
			return;
		}
	}

	bool WorldFrame() { return worldFrame; }

	void UpdateLifecycle(std::uint32_t a_frame)
	{
		auto* state = globals::state;
		const bool mainMenu = state && state->isMainMenuOpen;
		const bool loading = state && state->isLoadingMenuOpen;
		const bool held = Holds::Any();
		worldFrame = !loading && !held;
		bool playerReady = false;
		if (!loading && !mainMenu) {
			// The player's cell attached and their 3D loaded: the world around them is drawn.
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			playerReady = cell && cell->IsAttached() && player->Get3D() != nullptr;
		}

		auto& run = Run();
		std::scoped_lock lock(run.mutex);
		run.frame = a_frame;
		const double now = Now();
		++fpsWindowFrames;
		if (now - fpsWindowStart >= 1.0) {
			run.fps = fpsWindowFrames / (now - fpsWindowStart);
			fpsWindowStart = now;
			fpsWindowFrames = 0;
		}
		if (!run.configured)
			return;

		if (mainMenu && !FindMilestone("main-menu")) {
			RecordMilestone("main-menu", a_frame);
			if (!inLoad)
				loadTimerStart = now;
		}
		if (loading) {
			BeginLoad(a_frame);
			if (!loadingScreenSeen) {
				loadingScreenSeen = true;
				RecordMilestone("loading-screen", a_frame);
			}
		}
		const bool ready = !loading && !mainMenu && !held && playerReady;
		if (ready) {
			if (!worldReady && ++readyFrames >= kReadyFrames) {
				worldReady = true;
				inLoad = false;
				loadTimerStart = -1.0;
				RecordMilestone("world-ready", a_frame);
			}
		} else {
			readyFrames = 0;
			// Back to a menu or a loading screen from the world: the next ready world is recorded again.
			if (worldReady && (loading || mainMenu))
				worldReady = false;
		}
		UpdatePhase(run);
	}

	void CheckPhaseTimeouts()
	{
		auto& run = Run();
		std::string reason;
		{
			std::scoped_lock lock(run.mutex);
			if (!run.configured || !run.outcome.empty())
				return;
			const double now = Now();
			if (const auto* dataLoaded = FindMilestone("data-loaded"); dataLoaded && !FindMilestone("shaders-ready")) {
				const auto compile = GetCompileProgress();
				if (!compile.compiling)
					RecordMilestone("shaders-ready", run.frame);
				else if (now - dataLoaded->t > run.config.compileTimeout)
					reason = fmt::format("the shaders did not finish compiling in {:.0f} s ({} of {}); last milestone {}", run.config.compileTimeout, compile.done,
						compile.total, LastMilestone(run));
			}
			// The load timer runs once the shaders are ready: a cold compile in the main menu is the compile phase's.
			if (const auto* shadersReady = FindMilestone("shaders-ready"); reason.empty() && shadersReady && loadTimerStart >= 0.0 && LoadExpected(run.config)) {
				const double since = (std::max)(loadTimerStart, shadersReady->t);
				if (now - since > run.config.loadTimeout)
					reason = fmt::format("the world was not ready {:.0f} s after {}; last milestone {}", run.config.loadTimeout, inLoad ? "the load started" : "the main menu",
						LastMilestone(run));
			}
			UpdatePhase(run);
		}
		if (!reason.empty())
			Finish("never-loaded", std::move(reason), 0u);
	}
}
