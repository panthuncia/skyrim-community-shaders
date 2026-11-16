#include "RenderGraph/Frame/FrameDriver.h"

#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

namespace DCLF
{
	// Features/DrawcallLimitFix/Core/Scene/FrameView: whether Drawcall Limit Fix draws the world now. Render thread.
	bool DrawsWorld();
	// Features/DrawcallLimitFix/Core/Render/FrameDriver: Drawcall Limit Fix's build point, run at Renderer::Begin before the frame's
	// description, so a build attaches before the schedule is chosen. Its scene-frame start then skips it for this frame.
	void BuildPointAtFrameStart(std::uint64_t a_frame);
}

namespace RenderGraph::Frame::Driver
{
	namespace
	{
		constexpr std::uint32_t kKinds = static_cast<std::uint32_t>(Kind::Count);
		constexpr std::uint32_t kErrors = static_cast<std::uint32_t>(TapError::Count);

		constexpr std::uint32_t kScheduled = static_cast<std::uint32_t>(Scheduled::Count);

		/** @brief Per segment, since the last report: taps, recordings asked for, the schedule's switches and how frames ran. */
		struct SegmentCounts
		{
			std::uint64_t declared = 0;    // frames whose description contained the segment
			std::uint64_t taps = 0;        // end taps that fired in such a frame
			std::uint64_t recordings = 0;  // recordings asked for (a description with none yet), all at a Begin
			std::uint64_t late = 0;        // recordings asked for after a Begin (mid-frame re-records: the rule says none)
			std::uint64_t switches = 0;    // schedule switches (per frame slot)
			std::array<std::uint64_t, kScheduled> scheduled{};  // frames by how the segment ran (Scheduled)
			std::uint64_t absentTaps = 0;  // taps in frames without a schedule
			std::uint64_t lagged = 0;      // work left out because the schedule lagged
		};

		/** @brief The switches' lag, in frames since a slot began to wait for the description: 0, 1, 2, 3, 4-7, 8-15, 16 and more. */
		constexpr std::array<std::uint64_t, 7> kLagBuckets{ 0, 1, 2, 3, 4, 8, 16 };

		struct State
		{
			bool installed = false;
			// The frame being drawn and the one before it. OnBegin writes them inside Renderer::Lock, the render owner reads them.
			Description current{};
			Description previous{};
			bool began = false;
			std::array<bool, kSegments> tapped{};
			bool captured = false;  // the engine's frozen background is drawn (its gIsInMenuMode2): no more capture until no menu freezes it
			std::atomic<bool> loading{ false };  // between SetLoadingScreenActive(true) and the end of SetLoadingScreenActive(false)
			// The census since the last report, and the totals.
			std::mutex mutex;
			std::array<std::uint64_t, kKinds> frames{};
			std::array<SegmentCounts, kSegments> segments{};
			std::array<std::uint64_t, kErrors> errors{};
			std::vector<std::pair<const char*, std::uint64_t>> hosted;  // NoteHosted, by host name, since the last report
			std::atomic<std::uint64_t> currentFrame{ 0 };  // current.frame, for other threads
			std::atomic<std::uint64_t> loadStart{ 0 };     // the frame a loading screen started in, plus one (0: none yet)
			std::atomic<std::uint64_t> errorsTotal{ 0 };
			std::uint64_t kindChanges = 0;
			std::uint32_t errorLines = 0;
			// NoteRecording's log lines per segment in the current window: its start, the lines logged and those left out.
			struct RecordingLog
			{
				std::chrono::steady_clock::time_point start{};
				std::uint32_t lines = 0;
				std::uint64_t dropped = 0;
			};
			std::array<RecordingLog, kSegments> recordingLog{};
			std::array<RecordingLog, kSegments> switchLog{};
			std::array<std::uint64_t, kLagBuckets.size()> lags{};       // since the last report
			std::array<std::uint64_t, kLagBuckets.size()> lagsTotal{};  // the session's
			std::array<bool, kErrors> errorLogged{};  // the mismatch policy's one line per kind for the session
		};

		/** @brief Logs a line within a_log's window (16 a segment in 5 s), else counts it; the count is logged once the window passes. */
		void WindowedLine(State::RecordingLog& a_log, std::string_view a_what, std::uint64_t a_frame, const std::function<void()>& a_line)
		{
			constexpr std::uint32_t kLines = 16;
			constexpr auto kWindow = std::chrono::seconds(5);
			const auto now = std::chrono::steady_clock::now();
			if (now - a_log.start >= kWindow) {
				if (a_log.dropped)
					logger::info("[ORG] declared frame: {} more {} were not logged in the {} s before frame {}", a_log.dropped, a_what,
						std::chrono::duration_cast<std::chrono::seconds>(kWindow).count(), a_frame);
				a_log = { now, 0, 0 };
			}
			if (++a_log.lines <= kLines)
				a_line();
			else
				++a_log.dropped;
		}

		State& Get()
		{
			static State state;
			return state;
		}

		thread_local bool t_inMovieTick = false;

		/**
		 * @brief BSMoviePlayer::Tick (SE 87890, AE 90259), when the Bink hooks do not detour it (CS_ORG_BINK and CS_ORG_BINK_SHOT off):
		 * a frame it begins (no frame open: the startup logo, a blocking movie) is a Bink movie's.
		 */
		struct PlayerTick
		{
			static std::uint64_t thunk(std::uintptr_t a_player, std::uintptr_t a_arg2, std::uintptr_t a_arg3, std::uintptr_t a_arg4)
			{
				const MovieTickScope frameKind;
				return func(a_player, a_arg2, a_arg3, a_arg4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The engine's own test for a frozen background: MenuManager (SE 79938, AE 82075) scans the menu stack under its lock for
		 * a menu with kFreezeFrameBackground (IMenu flag 0x20). Main's frame (SE 35556 / AE 36555) calls it right after Begin to choose
		 * between drawing the world and compositing the frozen frame.
		 */
		bool FreezesBackground()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui)
				return false;
			using Fn = bool (*)(RE::UI*);
			static REL::Relocation<Fn> freezes{ REL::RelocationID(79938, 82075) };
			return freezes(ui);
		}

		/**
		 * @brief The kind of the frame Begin starts on this thread. The callers of Begin (Ghidra, SE): Main's frame (35556), the loading
		 * loop (ServingThread::RunLoadingScreen, 38085) and BSMoviePlayer::Tick when no frame is open. Main's frame draws the world
		 * when no menu freezes the background and the player's Get3D2 (vtable +0x380) is set. With a freezing menu it draws the world
		 * once (SE 35558 / AE 36558: also needs the player's cell) and composites that frame after.
		 */
		Kind DecideKind(State& a_state, bool& a_capture)
		{
			a_capture = false;
			if (t_inMovieTick)
				return Kind::Bink;
			if (!FrameAnchors::IsMainThread()) {
				static std::atomic_flag logged;
				if (!a_state.loading.load(std::memory_order_acquire) && !logged.test_and_set())
					logger::warn("[ORG] frame: a frame began on a thread that is neither the main thread nor a movie tick, outside a loading screen; "
								 "it counts as a loading screen's (logged once)");
				return Kind::Loading;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool has3D = player && player->Get3D2();
			if (!FreezesBackground()) {
				a_state.captured = false;
				return has3D ? Kind::World : Kind::MainMenu;
			}
			if (!a_state.captured && player && player->GetParentCell() && has3D) {
				a_capture = true;
				a_state.captured = true;
			}
			return Kind::FrozenWorld;
		}
	}

	void Install()
	{
		auto& state = Get();
		state.installed = true;
		// The Bink hooks mark their ticks themselves; without them the driver detours the tick for the kind alone.
		if (!REL::Module::IsVR() && !Engine::Bink::TickHooked())
			stl::detour_thunk<PlayerTick>(REL::RelocationID(87890, 90259));
		logger::info("[ORG] frame driver: each Renderer::Begin builds the frame's description (kind, slot, segments)");
	}

	bool Installed()
	{
		return Get().installed;
	}

	MovieTickScope::MovieTickScope() :
		outer(std::exchange(t_inMovieTick, true))
	{}

	MovieTickScope::~MovieTickScope()
	{
		t_inMovieTick = outer;
	}

	void OnLoadingScreen(bool a_active)
	{
		auto& state = Get();
		state.loading.store(a_active, std::memory_order_release);
		// A load that starts inside a world frame ends that frame's world work where it is (LoadStartedThisFrame).
		if (a_active)
			state.loadStart.store(state.currentFrame.load(std::memory_order_acquire) + 1, std::memory_order_release);
	}

	bool LoadStartedThisFrame()
	{
		auto& state = Get();
		return state.loadStart.load(std::memory_order_acquire) == state.currentFrame.load(std::memory_order_acquire) + 1;
	}

	void OnBegin()
	{
		auto& state = Get();
		if (!state.installed)
			return;
		// The frame before it: a declared segment whose tap never fired is a tap error.
		if (state.began)
			for (std::uint32_t s = 0; s < kSegments; ++s) {
				const auto& fields = state.current.segments[s];
				if (fields.declared && fields.graph && !state.tapped[s])
					NoteTapError(TapError::Missing, static_cast<SegmentId>(s), "the segment's end tap");
			}
		Description next{};
		next.frame = state.current.frame + 1;
		next.slot = static_cast<std::uint32_t>(next.frame % RenderGraphRuntime::kGameFramesInFlight);
		next.parity = static_cast<std::uint32_t>(next.frame & 1);
		next.kind = DecideKind(state, next.capture);
		// Native mode with segments: every build attaches at a frame boundary before the frame's schedule is chosen. Drawcall Limit
		// Fix's build point (where its graph builds attach and are handed out) runs here for a frame that draws the world, not at
		// Main::Draw's start after Begin. Guest keeps it at Main::Draw.
		if (FrameAnchors::SegmentsOn() && FrameAnchors::IsRenderOwner() && (next.kind == Kind::World || (next.kind == Kind::FrozenWorld && next.capture)))
			DCLF::BuildPointAtFrameStart(next.frame);
		next.dclfDraws = (next.kind == Kind::World || next.kind == Kind::FrozenWorld) && DCLF::DrawsWorld();
		// Hosted clears and copies the last frame left waiting are tap errors. Before the new frame is current: they are the last frame's.
		RenderGraph::Engine::Skeleton::BeginFrame();
		{
			const std::lock_guard lock(state.mutex);
			++state.frames[static_cast<std::size_t>(next.kind)];
			if (state.began && (next.kind != state.current.kind || next.dclfDraws != state.current.dclfDraws)) {
				// Kinds change at a menu, a load or a movie: rare, but a flapping test would flood the log.
				if (++state.kindChanges <= 256 || state.kindChanges % 1000 == 0)
					logger::info("[ORG] frame kind: {} (frame {}{})", DiffFrame(state.current, next), next.frame, next.capture ? ", draws the frozen background" : "");
			}
			if (!state.began)
				logger::info("[ORG] frame kind: {} (frame {}, the first frame described)", KindName(next.kind), next.frame);
		}
		// The new frame is current from here: what the description reads per frame slot (the post chain's and the world scopes' last
		// fields for this slot) reads this frame's slot. The frame number other threads see (LoadStartedThisFrame) moves at the end.
		state.previous = state.current;
		state.current = next;
		state.tapped.fill(false);
		// The post chain's fields (read again at the chain's decision event), then each segment's schedule for the frame.
		PostChain::OnBegin(state.current);
		// The recipe is checked again when a build of one of its epochs attached (render owner, before the stages' schedules).
		if (FrameAnchors::SegmentsOn() && FrameAnchors::IsRenderOwner())
			if (auto* host = RenderGraphRuntime::Get().Host())
				Recipes::Check(*host);
		FrameAnchors::BeginFrame(state.current);
		{
			const std::lock_guard lock(state.mutex);
			for (std::uint32_t s = 0; s < kSegments; ++s)
				state.segments[s].declared += state.current.segments[s].declared;
		}
		state.currentFrame.store(state.current.frame, std::memory_order_release);
		state.began = true;
	}

	const Description& Current()
	{
		return Get().current;
	}

	const char* TapErrorName(TapError a_error)
	{
		switch (a_error) {
		case TapError::NotDeclared:
			return "not declared";
		case TapError::Repeated:
			return "repeated";
		case TapError::NotInDescription:
			return "not in the description";
		case TapError::Missing:
			return "missing";
		case TapError::SlotMismatch:
			return "slot mismatch";
		case TapError::StaleRecording:
			return "stale recording";
		case TapError::HostSkipped:
			return "host skipped";
		case TapError::HostMissing:
			return "host missing";
		case TapError::LostToBuild:
			return "lost to a build";
		case TapError::InstanceNotDeclared:
			return "instance not declared";
		case TapError::NoPass:
			return "no CS pass";
		default:
			return "?";
		}
	}

	bool MismatchFatal()
	{
		static const bool fatal = [] {
#if defined(CS_RELEASE_PROFILE)
			bool on = false;
#else
			bool on = true;
#endif
			if (Switches::IsSet("CS_ORG_MISMATCH_FATAL"))
				on = !Switches::Off("CS_ORG_MISMATCH_FATAL");
			logger::info("[ORG] declared frame: a mismatch between the schedule and the engine {} (CS_ORG_MISMATCH_FATAL=0/1)",
				on ? "stops the game" : "is logged once per kind and its work is not drawn");
			return on;
		}();
		return fatal;
	}

	void NoteTapError(TapError a_error, SegmentId a_segment, std::string_view a_what)
	{
		auto& state = Get();
		state.errorsTotal.fetch_add(1, std::memory_order_relaxed);
		const auto where = a_segment < SegmentId::Count ? fmt::format("{} segment", SegmentName(a_segment)) : std::string("hosted work");
		if (MismatchFatal()) {
			const auto message = fmt::format(
				"Community Shaders native mode: the frame did not match its schedule ({}). Frame {} ({}, slot {}), {}: {}. Development builds stop "
				"here (CS_ORG_MISMATCH_FATAL=0 logs it instead).",
				TapErrorName(a_error), state.current.frame, KindName(state.current.kind), state.current.slot, where, a_what);
			logger::critical("[ORG] frame tap error: {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
		}
		const std::lock_guard lock(state.mutex);
		++state.errors[static_cast<std::size_t>(a_error)];
		// One line per kind for the session; the census line has the counts. A test greps "[ORG] frame tap error".
		if (!std::exchange(state.errorLogged[static_cast<std::size_t>(a_error)], true))
			logger::error("[ORG] frame tap error ({}): {}, {}; frame {} ({}, slot {}); its work is not drawn (later errors of this kind are counted, not logged)",
				TapErrorName(a_error), where, a_what, state.current.frame, KindName(state.current.kind), state.current.slot);
	}

	void NoteHosted(const char* a_host, std::uint32_t a_count)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		for (auto& [host, count] : state.hosted)
			if (host == a_host) {
				count += a_count;
				return;
			}
		state.hosted.emplace_back(a_host, a_count);
	}

	bool NoteTap(SegmentId a_segment, std::string_view a_tap)
	{
		auto& state = Get();
		const auto s = static_cast<std::size_t>(a_segment);
		if (state.tapped[s]) {
			NoteTapError(TapError::Repeated, a_segment, a_tap);
			return false;
		}
		state.tapped[s] = true;
		const std::lock_guard lock(state.mutex);
		++state.segments[s].taps;
		return true;
	}

	void NoteRecording(SegmentId a_segment, std::uint64_t a_frame, std::string_view a_reason)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].recordings;
		// Every request names what changed. A steady scene asks for none, so this line is rare; a flapping field would show here.
		WindowedLine(state.recordingLog[static_cast<std::size_t>(a_segment)], fmt::format("recordings of the {} segment", SegmentName(a_segment)), a_frame, [&] {
			logger::info("[ORG] declared frame: the {} segment is recorded for a new description (frame {}): {}", SegmentName(a_segment), a_frame, a_reason);
		});
	}

	void NoteSwitch(SegmentId a_segment, std::uint64_t a_frame, std::uint32_t a_slot, std::uint64_t a_lag, std::string_view a_what)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].switches;
		std::size_t bucket = 0;
		while (bucket + 1 < kLagBuckets.size() && a_lag >= kLagBuckets[bucket + 1])
			++bucket;
		++state.lags[bucket];
		++state.lagsTotal[bucket];
		WindowedLine(state.switchLog[static_cast<std::size_t>(a_segment)], fmt::format("schedule switches of the {} segment", SegmentName(a_segment)), a_frame, [&] {
			logger::info("[ORG] schedule: the {} segment switches in frame {} (slot {}, {} frames after a slot began to wait for it): {}", SegmentName(a_segment), a_frame,
				a_slot, a_lag, a_what);
		});
	}

	void NoteScheduled(SegmentId a_segment, Scheduled a_how)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].scheduled[static_cast<std::size_t>(a_how)];
	}

	void NoteLaggedWork(SegmentId a_segment)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].lagged;
	}

	void NoteAbsentTap(SegmentId a_segment)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].absentTaps;
	}

	void NoteLateRequest(SegmentId a_segment)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].late;
	}

	std::uint64_t TapErrors()
	{
		return Get().errorsTotal.load(std::memory_order_relaxed);
	}

	void LogStats()
	{
		auto& state = Get();
		if (!state.installed)
			return;
		const std::lock_guard lock(state.mutex);
		std::string kinds;
		for (std::uint32_t k = 0; k < kKinds; ++k)
			if (state.frames[k])
				kinds += fmt::format("{}{} {}", kinds.empty() ? "" : ", ", KindName(static_cast<Kind>(k)), state.frames[k]);
		std::string segments;
		for (std::uint32_t s = 0; s < kSegments; ++s) {
			const auto& counts = state.segments[s];
			segments += fmt::format("; {}: declared {}, taps {}, recordings {}, late requests {}, switches {}, current {}, lagging {}, absent {} ({} taps), "
									"lagged work {}",
				SegmentName(static_cast<SegmentId>(s)), counts.declared, counts.taps, counts.recordings, counts.late, counts.switches,
				counts.scheduled[static_cast<std::size_t>(Scheduled::Current)], counts.scheduled[static_cast<std::size_t>(Scheduled::Lagging)],
				counts.scheduled[static_cast<std::size_t>(Scheduled::Absent)], counts.absentTaps, counts.lagged);
		}
		const auto lagLine = [](const auto& a_lags) {
			std::string line;
			for (std::size_t b = 0; b < kLagBuckets.size(); ++b) {
				const bool last = b + 1 == kLagBuckets.size();
				const auto range = last ? fmt::format("{}+", kLagBuckets[b]) :
				                   kLagBuckets[b + 1] == kLagBuckets[b] + 1 ? fmt::format("{}", kLagBuckets[b]) :
				                                                              fmt::format("{}-{}", kLagBuckets[b], kLagBuckets[b + 1] - 1);
				line += fmt::format("{}{}: {}", line.empty() ? "" : ", ", range, a_lags[b]);
			}
			return line;
		};
		segments += fmt::format("; switch lag in frames (window) {}; (session) {}", lagLine(state.lags), lagLine(state.lagsTotal));
		std::string errors;
		std::uint64_t errorCount = 0;
		for (std::uint32_t e = 0; e < kErrors; ++e) {
			errorCount += state.errors[e];
			if (state.errors[e])
				errors += fmt::format("{}{} {}", errors.empty() ? "" : ", ", TapErrorName(static_cast<TapError>(e)), state.errors[e]);
		}
		std::uint64_t frames = 0;
		for (const auto count : state.frames)
			frames += count;
		std::string hosted;
		for (auto& [host, count] : state.hosted) {
			if (count)
				hosted += fmt::format("{}{} {:.2f}", hosted.empty() ? "" : ", ", host, frames ? static_cast<double>(count) / static_cast<double>(frames) : 0.0);
			count = 0;
		}
		logger::info("[ORG] declared frame: frames by kind: {}{}; hosted clears and copies a frame: {}; tap errors {}{}{}{} (total {})", kinds.empty() ? "none" : kinds,
			segments, hosted.empty() ? "none" : hosted, errorCount, errors.empty() ? "" : " (", errors, errors.empty() ? "" : ")",
			state.errorsTotal.load(std::memory_order_relaxed));
		state.frames = {};
		state.segments = {};
		state.errors = {};
		state.lags = {};
	}
}
