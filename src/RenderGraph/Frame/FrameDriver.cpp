#include "RenderGraph/Frame/FrameDriver.h"

#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace DCLF
{
	// Features/DrawcallLimitFix/Core/Scene/FrameView: whether Drawcall Limit Fix draws the world now. Render thread.
	bool DrawsWorld();
}

namespace RenderGraph::Frame::Driver
{
	namespace
	{
		constexpr std::uint32_t kKinds = static_cast<std::uint32_t>(Kind::Count);
		constexpr std::uint32_t kErrors = static_cast<std::uint32_t>(TapError::Count);

		/** @brief Per segment, since the last report: taps, recordings asked for, and the taps that waited for one. */
		struct SegmentCounts
		{
			std::uint64_t declared = 0;    // frames whose description contained the segment
			std::uint64_t taps = 0;        // end taps that fired in such a frame
			std::uint64_t recordings = 0;  // recordings asked for (a description with none yet)
			std::uint64_t waits = 0;       // taps that waited for their recording
			double waitUs = 0.0;
		};

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
		};

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
		next.dclfDraws = (next.kind == Kind::World || next.kind == Kind::FrozenWorld) && DCLF::DrawsWorld();
		// The post chain's fields (read again at the chain's decision event), then the segments' fields, and a recording asked for each
		// declared one that has none.
		PostChain::OnBegin(next);
		FrameAnchors::BeginFrame(next);
		// Hosted clears and copies the last frame left waiting are tap errors.
		RenderGraph::Engine::Skeleton::BeginFrame();
		{
			const std::lock_guard lock(state.mutex);
			++state.frames[static_cast<std::size_t>(next.kind)];
			for (std::uint32_t s = 0; s < kSegments; ++s)
				state.segments[s].declared += next.segments[s].declared;
			if (state.began && (next.kind != state.current.kind || next.dclfDraws != state.current.dclfDraws)) {
				// Kinds change at a menu, a load or a movie: rare, but a flapping test would flood the log.
				if (++state.kindChanges <= 256 || state.kindChanges % 1000 == 0)
					logger::info("[ORG] frame kind: {} (frame {}{})", DiffFrame(state.current, next), next.frame, next.capture ? ", draws the frozen background" : "");
			}
			if (!state.began)
				logger::info("[ORG] frame kind: {} (frame {}, the first frame described)", KindName(next.kind), next.frame);
		}
		state.previous = state.current;
		state.current = next;
		state.currentFrame.store(next.frame, std::memory_order_release);
		state.tapped.fill(false);
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
		case TapError::PostNotDeclared:
			return "post not declared";
		case TapError::PostStateMismatch:
			return "post state mismatch";
		default:
			return "?";
		}
	}

	void NoteTapError(TapError a_error, SegmentId a_segment, std::string_view a_what)
	{
		auto& state = Get();
		state.errorsTotal.fetch_add(1, std::memory_order_relaxed);
		const std::lock_guard lock(state.mutex);
		++state.errors[static_cast<std::size_t>(a_error)];
		// The first 64 lines, then one in a thousand: a test greps "[ORG] frame tap error"; the census line has the counts.
		if (++state.errorLines <= 64 || state.errorLines % 1000 == 0)
			logger::error("[ORG] frame tap error ({}): {}, {}; frame {} ({}, slot {})", TapErrorName(a_error),
				a_segment < SegmentId::Count ? fmt::format("{} segment", SegmentName(a_segment)) : std::string("hosted work"), a_what, state.current.frame,
				KindName(state.current.kind), state.current.slot);
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
		auto& counts = state.segments[static_cast<std::size_t>(a_segment)];
		++counts.recordings;
		// Every request names what changed. A steady scene asks for none, so this line is rare; a flapping field would show here.
		// Each segment logs at most kLines requests in a window of kWindow, so startup cannot use up the lines for later: every
		// re-record after it is logged. The requests over the limit are counted and logged once the window has passed.
		constexpr std::uint32_t kLines = 16;
		constexpr auto kWindow = std::chrono::seconds(5);
		auto& log = state.recordingLog[static_cast<std::size_t>(a_segment)];
		const auto now = std::chrono::steady_clock::now();
		if (now - log.start >= kWindow) {
			if (log.dropped)
				logger::info("[ORG] declared frame: {} more recordings of the {} segment were not logged in the {} s before frame {}", log.dropped,
					SegmentName(a_segment), std::chrono::duration_cast<std::chrono::seconds>(kWindow).count(), a_frame);
			log = { now, 0, 0 };
		}
		if (++log.lines <= kLines)
			logger::info("[ORG] declared frame: the {} segment is recorded for a new description (frame {}): {}", SegmentName(a_segment), a_frame,
				a_reason);
		else
			++log.dropped;
	}

	void NoteRecordingWait(SegmentId a_segment, double a_waitUs)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		auto& counts = state.segments[static_cast<std::size_t>(a_segment)];
		++counts.waits;
		counts.waitUs += a_waitUs;
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
			segments += fmt::format("; {}: declared {}, taps {}, recordings {}, waits {} ({:.0f} us)", SegmentName(static_cast<SegmentId>(s)), counts.declared,
				counts.taps, counts.recordings, counts.waits, counts.waitUs);
		}
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
	}
}
