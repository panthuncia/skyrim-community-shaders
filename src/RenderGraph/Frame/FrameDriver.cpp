#include "RenderGraph/Frame/FrameDriver.h"

#include "Feature.h"

#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"
#include "Utils/Setting.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace DCLF
{
	// Whether DCLF draws the world now. Render thread.
	bool DrawsWorld();
	// DCLF's build point. It runs before the frame's description so a build attaches before the schedule is chosen.
	void BuildPointAtFrameStart(std::uint64_t a_frame);
}

namespace RenderGraph::Frame::Driver
{
	namespace
	{
		constexpr std::uint32_t kKinds = static_cast<std::uint32_t>(Kind::Count);
		constexpr std::uint32_t kErrors = static_cast<std::uint32_t>(TapError::Count);

		constexpr std::uint32_t kScheduled = static_cast<std::uint32_t>(Scheduled::Count);

		/** @brief Per segment counts since the last report. */
		struct SegmentCounts
		{
			std::uint64_t declared = 0;
			std::uint64_t taps = 0;        // end taps that fired in a declared frame
			std::uint64_t recordings = 0;  // all asked at a Begin
			std::uint64_t late = 0;        // mid-frame re-records, which should never happen
			std::uint64_t switches = 0;    // per frame slot
			std::array<std::uint64_t, kScheduled> scheduled{};
			std::uint64_t absentTaps = 0;  // taps in frames without a schedule
			std::uint64_t lagged = 0;      // work left out because the schedule lagged
			std::uint64_t buildSwitches = 0;  // staged builds switched in whole with their recordings
			std::uint64_t keyBuilds = 0;      // builds asked for because the stage's key changed
			std::uint64_t absentAfter = 0;    // absent frames after the segment first ran
		};

		/** @brief Lag buckets in frames since a slot began to wait for the description. */
		constexpr std::array<std::uint64_t, 7> kLagBuckets{ 0, 1, 2, 3, 4, 8, 16 };

		struct State
		{
			bool installed = false;
			// OnBegin writes these inside Renderer::Lock, the render owner reads them.
			Description current{};
			Description previous{};
			bool began = false;
			std::array<bool, kSegments> tapped{};
			bool captured = false;  // gIsInMenuMode2, so no more capture until no menu freezes the background
			// The freeze test's answer at Begin. Main thread only.
			bool freezes = false;
			std::uint64_t freezesFrame = ~0ull;
			std::uint64_t freezeChanges = 0;     // frames where the engine's own test differed from Begin's
			std::atomic<bool> loading{ false };  // held until the end of SetLoadingScreenActive(false)
			std::mutex mutex;
			std::array<std::uint64_t, kKinds> frames{};
			std::array<SegmentCounts, kSegments> segments{};
			std::array<std::uint64_t, kErrors> errors{};
			std::vector<std::pair<const char*, std::uint64_t>> hosted;  // by host name, since the last report
			std::atomic<std::uint64_t> currentFrame{ 0 };  // for other threads
			std::atomic<std::uint64_t> loadStart{ 0 };     // the frame a loading screen started in plus one, 0 for none
			std::atomic<std::uint64_t> errorsTotal{ 0 };
			std::uint64_t kindChanges = 0;
			std::uint32_t errorLines = 0;
			struct RecordingLog
			{
				std::chrono::steady_clock::time_point start{};
				std::uint32_t lines = 0;
				std::uint64_t dropped = 0;
			};
			std::array<RecordingLog, kSegments> recordingLog{};
			std::array<RecordingLog, kSegments> switchLog{};
			std::array<std::uint64_t, kLagBuckets.size()> lags{};
			std::array<std::uint64_t, kLagBuckets.size()> lagsTotal{};
			std::array<bool, kErrors> errorLogged{};  // one line per kind for the session
			std::array<bool, kSegments> everPresent{};  // session
			std::array<std::uint64_t, kSegments> absentAfterTotal{};  // session
			std::unordered_map<std::string, std::uint64_t> untappable;  // per client, the frame its work last lagged with nothing on its way
		};

		/** @brief Logs up to 16 lines a segment in 5 s and counts the rest, logging the count once the window passes. */
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

		/** @brief BSMoviePlayer::Tick (SE 87890, AE 90259) when the Bink hooks do not detour it. A frame it begins is a Bink frame. */
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
		 * @brief The engine's own frozen-background test. MenuManager (SE 79938, AE 82075) looks for kFreezeFrameBackground (0x20).
		 * Main's frame (SE 35556 / AE 36555) calls it right after Begin.
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
		 * @brief The kind of frame Begin starts on this thread. Begin is called by Main's frame (SE 35556), the loading loop (38085)
		 * and BSMoviePlayer::Tick. The world draws when no menu freezes it and the player's Get3D2 (vtable +0x380) is set.
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
			a_state.freezes = FreezesBackground();
			a_state.freezesFrame = a_state.current.frame + 1;
			if (!a_state.freezes) {
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

	/**
	 * @brief Makes Main's second freeze test return what Begin decided, so the engine draws the frame the description has.
	 * Another thread can push a menu between the two tests through the UI message queue.
	 */
	struct FreezeTestAtBegin
	{
		static bool thunk(RE::UI* a_ui)
		{
			const bool live = func(a_ui);
			auto& state = Get();
			if (!Engine::Native::GraphMode() || state.freezesFrame != state.current.frame || !FrameAnchors::IsMainThread())
				return live;
			if (live != state.freezes && state.freezeChanges++ < 8)
				logger::info("[ORG] frame: the menu stack's freeze flag changed after frame {}'s Begin ({} -> {}); the frame keeps Begin's answer",
					state.current.frame, state.freezes, live);
			return state.freezes;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Install()
	{
		auto& state = Get();
		state.installed = true;
		Boundary::Install();
		Revision::Install();
		if (!REL::Module::IsVR())
			stl::write_thunk_call<FreezeTestAtBegin>(REL::RelocationID(35556, 36555).address() + REL::Relocate(0x211, 0x217));
		// The Bink hooks mark their own ticks. Without them this detour only reads the kind.
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
		// A load that starts inside a world frame ends that frame's world work where it is.
		if (a_active)
			state.loadStart.store(state.currentFrame.load(std::memory_order_acquire) + 1, std::memory_order_release);
	}

	bool LoadStartedThisFrame()
	{
		auto& state = Get();
		return state.loadStart.load(std::memory_order_acquire) == state.currentFrame.load(std::memory_order_acquire) + 1;
	}

	namespace
	{
		/**
		* @brief The missed-writer check of frame revision plan 6.3, in development builds only. A feature's size settings
		* that changed with no settings event, or a bound CS::Setting whose value changed with no raise, is logged once and is a
		* tap error. It only reports and never feeds a revision. CS_ORG_MISSED_WRITER_CHECK=0 turns it off.
		*/
		void CheckSettingsWriters()
		{
#if !defined(CS_RELEASE_PROFILE)
			static const bool enabled = [] {
				const bool on = !Switches::Off("CS_ORG_MISSED_WRITER_CHECK");
				logger::info("[ORG] missed settings writer check {} (CS_ORG_MISSED_WRITER_CHECK=0/1)", on ? "on" : "off");
				return on;
			}();
			if (!enabled)
				return;
			for (const auto& field : Feature::FindMissedSettingsWriters()) {
				logger::error("[ORG] missed settings writer: {} changed with no event", field);
				NoteTapError(TapError::MissedSettingsWriter, SegmentId::Count, fmt::format("{} changed with no settings event", field));
			}
			// A bound setting counts once two checks in a row see it unraised, as a feature's projection does.
			static std::vector<CS::SettingVariableId> unraisedBefore;
			static std::set<CS::SettingVariableId> reported;
			auto unraised = CS::SettingRegistry::Unraised();
			for (const auto id : unraised)
				if (std::ranges::contains(unraisedBefore, id) && reported.insert(id).second) {
					logger::error("[ORG] missed settings writer: {} changed with no event", CS::SettingRegistry::Name(id));
					NoteTapError(TapError::MissedSettingsWriter, SegmentId::Count,
						fmt::format("the setting {} changed with no settings event", CS::SettingRegistry::Name(id)));
				}
			unraisedBefore = std::move(unraised);
#endif
		}
	}

	void OnBegin()
	{
		auto& state = Get();
		if (!state.installed)
			return;
		// The frame's adoption point. Versions, builds and registrations changed outside it are counted (rule R1).
		const Boundary::Scope boundary;
		// The newest prepared frame revision switches in whole, before anything reads what it changes.
		Revision::AdoptLatest();
		// After the adoption, so the values it adopted are the ones compared.
		CheckSettingsWriters();
		// A declared segment of the last frame whose tap never fired is a tap error.
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
		// In native mode DCLF's build point runs here for a world frame, before the schedule is chosen. Guest mode keeps it at
		// Main::Draw (DCLF's BeginFrame). Either way it only stages builds. No anchor runs an epoch of its own. CS stage builds attach
		// in AdoptLatest, and DCLF's with the snapshot it adopts there, in both modes (plan step 10a).
		if (Engine::Native::Requested() && FrameAnchors::IsRenderOwner() &&
			(next.kind == Kind::World || (next.kind == Kind::FrozenWorld && next.capture)))
			DCLF::BuildPointAtFrameStart(next.frame);
		next.dclfDraws = (next.kind == Kind::World || next.kind == Kind::FrozenWorld) && DCLF::DrawsWorld();
		// Clears and copies the last frame left waiting are its tap errors, so this runs before the new frame is current.
		RenderGraph::Engine::Skeleton::BeginFrame();
		{
			const std::lock_guard lock(state.mutex);
			++state.frames[static_cast<std::size_t>(next.kind)];
			if (state.began && (next.kind != state.current.kind || next.dclfDraws != state.current.dclfDraws)) {
				// Kind changes are rare, but a flapping test would flood the log.
				if (++state.kindChanges <= 256 || state.kindChanges % 1000 == 0) {
					const auto recipe = Recipes::DiffKinds(state.current.kind, state.current.capture, next.kind, next.capture);
					logger::info("[ORG] frame kind: {} (frame {}{}){}{}", DiffFrame(state.current, next), next.frame, next.capture ? ", draws the frozen background" : "",
						recipe.empty() ? "" : "; ", recipe);
				}
			}
			if (!state.began)
				logger::info("[ORG] frame kind: {} (frame {}, the first frame described)", KindName(next.kind), next.frame);
		}
		// The new frame is current from here, so per-slot reads use this frame's slot. Other threads see the new frame number at the end.
		state.previous = state.current;
		state.current = next;
		state.tapped.fill(false);
		if (FrameAnchors::IsRenderOwner())
			RenderGraphRuntime::Get().NoteFrameBegin(next.frame);
		PostChain::OnBegin(state.current);
		// Recheck the recipe when a build of one of its epochs attached.
		if (Engine::Native::GraphMode() && FrameAnchors::IsRenderOwner())
			if (auto* host = RenderGraphRuntime::Get().Host())
				Recipes::Check(*host);
		FrameAnchors::BeginFrame(state.current);
		Engine::ImageSpace::OnFrameBegin();
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
		case TapError::Refused:
			return "refused";
		case TapError::Untappable:
			return "not tappable";
		case TapError::Starved:
			return "staged build starved";
		case TapError::Unbacked:
			return "unbacked engine target used";
		case TapError::NotExecuted:
			return "run not executed";
		case TapError::OwnEpoch:
			return "own epoch";
		case TapError::MissedSettingsWriter:
			return "missed settings writer";
		case TapError::OutsideAdoption:
			return "outside the adoption point";
		case TapError::DclfInputChanged:
			return "DCLF input changed after it drew";
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
		// One line per kind for the session. A test greps "[ORG] frame tap error".
		if (!std::exchange(state.errorLogged[static_cast<std::size_t>(a_error)], true))
			logger::error("[ORG] frame tap error ({}): {}, {}; frame {} ({}, slot {}); its work is not drawn (later errors of this kind are counted, not logged)",
				TapErrorName(a_error), where, a_what, state.current.frame, KindName(state.current.kind), state.current.slot);
	}

	void NoteUnbackedUse(std::string_view a_target, std::string_view a_what)
	{
		if (!MismatchFatal()) {
			static std::mutex lock;
			static std::set<std::string, std::less<>> logged;
			std::scoped_lock guard(lock);
			if (logged.size() < 64 && logged.emplace(a_target).second)
				logger::error("[ORG] unbacked engine target used: {}; the operation is skipped (logged once per target)", a_what);
		}
		NoteTapError(TapError::Unbacked, SegmentId::Count, a_what);
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
		// A steady scene asks for no recordings, so a flapping field shows up here.
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
		const auto s = static_cast<std::size_t>(a_segment);
		++state.segments[s].scheduled[static_cast<std::size_t>(a_how)];
		if (a_how != Scheduled::Absent)
			state.everPresent[s] = true;
		else if (state.everPresent[s]) {
			++state.segments[s].absentAfter;
			++state.absentAfterTotal[s];
		}
	}

	void NoteBuildSwitch(SegmentId a_segment)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].buildSwitches;
	}

	void NoteKeyBuild(SegmentId a_segment)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].keyBuilds;
	}

	void NoteLaggedWork(SegmentId a_segment)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.segments[static_cast<std::size_t>(a_segment)].lagged;
	}

	void NoteStepLag(SegmentId a_segment, std::string_view a_client, std::string_view a_what)
	{
		auto& state = Get();
		auto& seen = state.untappable[std::string(a_client)];
		if (FrameAnchors::TapMayLag(a_client)) {
			seen = 0;
			NoteLaggedWork(a_segment);
			return;
		}
		// Work a build left out lags one frame while the next description asks for it.
		// If nothing is on its way by the next frame, it would never be drawn.
		const auto frame = state.current.frame;
		if (seen + 1 != frame) {
			if (seen != frame)
				seen = frame;
			NoteLaggedWork(a_segment);
			return;
		}
		NoteTapError(TapError::Untappable, a_segment,
			fmt::format("{}: its client {} is not in the build the stage runs, and in the frame after the engine first drew it the stage runs its "
						"current schedule with no build of the client on its way; it would never be drawn",
				a_what, a_client));
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
									"lagged work {}, build switches {}, key builds {}, absent after it first ran {} (session {})",
				SegmentName(static_cast<SegmentId>(s)), counts.declared, counts.taps, counts.recordings, counts.late, counts.switches,
				counts.scheduled[static_cast<std::size_t>(Scheduled::Current)], counts.scheduled[static_cast<std::size_t>(Scheduled::Lagging)],
				counts.scheduled[static_cast<std::size_t>(Scheduled::Absent)], counts.absentTaps, counts.lagged, counts.buildSwitches, counts.keyBuilds,
				counts.absentAfter, state.absentAfterTotal[s]);
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
		Boundary::LogStats();
		Revision::LogStats();
	}
}
