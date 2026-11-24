#include "RenderGraph/Frame/FrameDriver.h"

#include "RenderGraph/BuildManager.h"
#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameClock.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"
#include "Utils/Setting.h"

#include <ORGModuleServices/ViolationPolicy.h>

#include "RenderGraph/Backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace RenderGraph::Frame::Driver
{
	namespace
	{
		constexpr std::uint32_t kKinds = static_cast<std::uint32_t>(Kind::Count);
		constexpr std::uint32_t kErrors = static_cast<std::uint32_t>(TapError::Count);

		// Current, lagging and absent, as NoteScheduled counts them.
		constexpr std::uint32_t kScheduled = 3;

		std::size_t ScheduledIndex(FrameAnchors::Reason a_how)
		{
			return a_how == FrameAnchors::Reason::None ? 0 : a_how == FrameAnchors::Reason::Behind ? 1 : 2;
		}

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
			std::vector<std::pair<const char*, std::uint64_t>> hosted;  // by host name, since the last report
			std::atomic<std::uint64_t> loadStart{ 0 };     // the frame a loading screen started in plus one, 0 for none
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
		// Set at a movie tick's start, cleared once its Begin made the Bink frame current.
		thread_local bool t_movieFrameNext = false;

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
			a_state.freezesFrame = FrameClock::BeginFrame();
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
			if (!Engine::GraphMode() || state.freezesFrame != state.current.frame || !FrameAnchors::IsMainThread())
				return live;
			if (live != state.freezes && state.freezeChanges++ < 8)
				logger::info("[ORG] frame: the menu stack's freeze flag changed after frame {}'s Begin ({} -> {}); the frame keeps Begin's answer",
					state.current.frame, state.freezes, live);
			return state.freezes;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	namespace
	{
		/**
		* @brief Renderer::Begin (SE 75460, AE 77245), on whichever thread renders the frame, inside Renderer::Lock. In the graph modes
		* that thread owns rendering until another one begins a frame (Engine::LoadingScreen). Then the frame clock moves, so OnBegin
		* reads the frame it starts. Revisions adopt only on the main thread (D-4): another thread's frames keep the revision adopted last.
		*/
		struct RendererBegin
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_flags)
			{
				{
					ZoneScopedN("CS.Frame.RendererBegin");
					if (Engine::GraphMode())
						FrameAnchors::ClaimRenderOwnership();
					RenderGraphRuntime::OnRendererBegin();
					OnBegin();
				}
				func(a_renderer, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		auto& state = Get();
		state.installed = true;
		Boundary::Install();
		Revision::Install();
		if (Engine::GraphMode())
			logger::info("[ORG] render ownership: Renderer::Begin claims rendering for its thread; the thread that owns rendering adopts revisions; main-thread fragment sources adopt on the main thread only");
		stl::detour_thunk<RendererBegin>(REL::RelocationID(75460, 77245));
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
		outer(std::exchange(t_inMovieTick, true)),
		outerNext(std::exchange(t_movieFrameNext, true))
	{}

	MovieTickScope::~MovieTickScope()
	{
		t_inMovieTick = outer;
		t_movieFrameNext = outerNext;
	}

	bool MovieFrameNext()
	{
		return t_inMovieTick && t_movieFrameNext;
	}

	void OnLoadingScreen(bool a_active)
	{
		auto& state = Get();
		state.loading.store(a_active, std::memory_order_release);
		// A load that starts inside a world frame ends that frame's world work where it is.
		if (a_active)
			state.loadStart.store(FrameClock::BeginFrame() + 1, std::memory_order_release);
	}

	bool LoadStartedThisFrame()
	{
		auto& state = Get();
		return state.loadStart.load(std::memory_order_acquire) == FrameClock::BeginFrame() + 1;
	}

	namespace
	{
		/**
		* @brief The missed-writer check of frame revision plan 6.3, in development builds only. A bound CS::Setting whose value
		* changed with no raise is logged once and is a tap error. It only reports and never feeds a revision.
		*/
		void CheckSettingsWriters()
		{
#if !defined(CS_RELEASE_PROFILE)
			// A bound setting counts once two checks in a row see it unraised.
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

	namespace
	{
		/**
		* @brief The one writer of the engine values the frame observes (plan A7): whether the player is in an interior cell, and
		* the volumetric lighting volume size. Main thread, at each Begin. Writing the same value is no event.
		*/
		void SampleEngine()
		{
			if (const auto* tes = RE::TES::GetSingleton())
				(void)Vars::Set(Vars::Interior(), tes->interiorCell != nullptr, "engine sampler");
			// The VL descriptor (SE 100297, AE 107014) is made on its first read, with the Low preset's static default. The engine's
			// ImageSpace setup (SE 1412dbd00) then sets the INI's quality, after CS's setup. A quality change (SE 100299: the
			// console's command, or CS's feature) sets it again. Inside a frame, so it is read here, at the next Begin.
			static REL::Relocation<const std::byte* (*)()> descriptor{ REL::RelocationID(100297, 107014) };
			if (const auto* vl = descriptor()) {
				Vars::Extent3D size{};
				std::memcpy(&size.width, vl + 0x30, sizeof(std::uint32_t));
				std::memcpy(&size.height, vl + 0x34, sizeof(std::uint32_t));
				std::memcpy(&size.depth, vl + 0x38, sizeof(std::uint32_t));
				(void)Vars::Set(Vars::VolumetricLightingSize(), size, "engine sampler");
			}
		}
	}

	void OnBegin()
	{
		auto& state = Get();
		if (!state.installed)
			return;
		// The frame's adoption point. Versions, builds and registrations changed outside it are counted (rule R1).
		const Boundary::Scope boundary;
		// The thread that owns rendering adopts: the main thread, or a loading screen's or the startup movie's thread while it owns
		// rendering. The startup sequence runs on the movie's thread, and targets made again there register there (decision 70).
		// Epochs that failed since the last Begin go to their owners here, never mid-frame. What they drop asks for builds below.
		{
			ZoneScopedN("CS.Frame.Begin.PollEpochFailures");
			RenderGraphRuntime::Get().PollEpochFailures();
		}
		// The engine values the frame observes, before the adoption, so a change is adopted in this Begin when nothing else waits.
		// Only the main thread reads them: a load changes them on the main thread while another thread renders.
		if (FrameAnchors::IsMainThread()) {
			ZoneScopedN("CS.Frame.Begin.SampleEngine");
			SampleEngine();
		}
		// Every build asked for since the last adoption point is made here (Builds::Ask).
		{
			ZoneScopedN("CS.Frame.Begin.IssueAdoptionRequests");
			Builds::IssueAdoptionRequests();
		}
		// The newest prepared frame revision switches in whole, before anything reads what it changes.
		Revision::AdoptLatest();
		// After the adoption, so the values it adopted are the ones compared.
		{
			ZoneScopedN("CS.Frame.Begin.CheckSettingsWriters");
			CheckSettingsWriters();
		}
		// A declared segment of the last frame whose tap never fired is a tap error.
		if (state.began)
			for (std::uint32_t s = 0; s < kSegments; ++s) {
				const auto& fields = state.current.segments[s];
				if (fields.declared && fields.graph && !state.tapped[s])
					NoteTapError(TapError::Missing, static_cast<SegmentId>(s), "the segment's end tap");
			}
		// CS_ORG_TAP_ERROR_TEST=<frame>: one tap error at the first Begin from that frame on, to test the error policy and the
		// menu's warning. Fatal unless CS_ORG_MISMATCH_FATAL=0 or a release build.
		static const std::uint64_t s_testErrorFrame = Switches::Number("CS_ORG_TAP_ERROR_TEST", 0);
		static bool s_testErrorDone = false;
		// The frame clock already moved, so this reads the frame Begin starts.
		const auto clock = FrameClock::Read();
		if (s_testErrorFrame && clock.beginFrame >= s_testErrorFrame && !std::exchange(s_testErrorDone, true))
			NoteTapError(TapError::Refused, SegmentId::Count, "CS_ORG_TAP_ERROR_TEST asked for one");
		Description next{};
		next.frame = clock.beginFrame;
		next.slot = clock.slot;
		next.parity = static_cast<std::uint32_t>(next.frame & 1);
		{
			ZoneScopedN("CS.Frame.Begin.DecideKind");
			next.kind = DecideKind(state, next.capture);
		}
		// In native mode the fragment sources' build points run here for a world frame, before the schedule is chosen. Guest mode
		// keeps them in the frame. Either way they only stage builds. No anchor runs an epoch of its own. CS stage builds attach in
		// AdoptLatest, and a source's with the fragment it adopts there, in both modes.
		if (Engine::Native::Requested() && FrameAnchors::IsRenderOwner() &&
			(next.kind == Kind::World || (next.kind == Kind::FrozenWorld && next.capture)))
			Revision::FragmentFrameStart(next.frame);
		ZoneNamedN(drawsWorldZone, "CS.Frame.Begin.FragmentsDrawWorld", true);
		next.sceneDraws = (next.kind == Kind::World || next.kind == Kind::FrozenWorld) && Revision::FragmentsDrawWorld();
		// Clears and copies the last frame left waiting are its tap errors, so this runs before the new frame is current.
		{
			ZoneScopedN("CS.Frame.Begin.Skeleton");
			RenderGraph::Engine::Skeleton::BeginFrame();
		}
		{
			const std::lock_guard lock(state.mutex);
			++state.frames[static_cast<std::size_t>(next.kind)];
			if (state.began && (next.kind != state.current.kind || next.sceneDraws != state.current.sceneDraws)) {
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
		// The new frame is current from here, so per-slot reads use this frame's slot. Other threads read the frame clock.
		state.previous = state.current;
		state.current = next;
		// A movie tick's work from here is the Bink frame's, which is now current.
		t_movieFrameNext = false;
		state.tapped.fill(false);
		{
			ZoneScopedN("CS.Frame.Begin.PostChain");
			PostChain::OnBegin(state.current);
		}
		// Recheck the recipe when a build of one of its epochs attached.
		if (Engine::GraphMode() && FrameAnchors::IsRenderOwner())
			if (auto* host = RenderGraphRuntime::Get().Host())
				Recipes::Check(*host);
		{
			ZoneScopedN("CS.Frame.Begin.Anchors");
			FrameAnchors::BeginFrame(state.current);
		}
		{
			ZoneScopedN("CS.Frame.Begin.ImageSpace");
			Engine::ImageSpace::OnFrameBegin();
		}
		{
			const std::lock_guard lock(state.mutex);
			for (std::uint32_t s = 0; s < kSegments; ++s)
				state.segments[s].declared += state.current.segments[s].declared;
		}
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
		case TapError::ClientInputChanged:
			return "client input changed after it drew";
		case TapError::LateChange:
			return "late change";
		case TapError::EngineDraw:
			return "engine draw on D3D11";
		case TapError::EngineWork:
			return "engine work on D3D11";
		case TapError::Uncovered:
			return "uncovered epoch";
		case TapError::SceneMismatch:
			return "scene mismatch";
		case TapError::DrawInputs:
			return "draw inputs incomplete";
		case TapError::EngineLeak:
			return "engine leak";
		case TapError::OutsideLedgers:
			return "work outside the ledgers";
		case TapError::UndeclaredAccess:
			return "undeclared access";
		case TapError::D3D11Path:
			return "D3D11 path in a graph mode";
		default:
			return "?";
		}
	}

	const char* TapErrorMeaning(TapError a_error)
	{
		switch (a_error) {
		case TapError::NotDeclared:
			return "a segment's tap fired in a frame whose description does not contain the segment";
		case TapError::Repeated:
			return "a segment's tap fired a second time in one frame";
		case TapError::NotInDescription:
			return "a client had work at a tap whose passes the frame's description turned off";
		case TapError::Missing:
			return "the frame declared a segment, and its tap did not fire by the next Begin";
		case TapError::SlotMismatch:
			return "a segment's run was not the frame slot's published copy";
		case TapError::StaleRecording:
			return "a recording bound a version that a slot replaced while it was made";
		case TapError::HostSkipped:
			return "a clear or copy waiting for its host epoch saw another epoch submitted first";
		case TapError::HostMissing:
			return "a clear or copy was still waiting for its host epoch at the next Begin";
		case TapError::LostToBuild:
			return "a build of a segment's graph took back its held ticket between two taps";
		case TapError::InstanceNotDeclared:
			return "a step's hook fired more often in one segment run than the frame declared instances of its client";
		case TapError::NoPass:
			return "an engine effect step fired, and CS has no pass for that step";
		case TapError::Refused:
			return "a client could not draw the engine's work at its hook";
		case TapError::Untappable:
			return "a step's client is not in the build its stage runs, and no build with it is coming";
		case TapError::Starved:
			return "a staged build's recordings were asked for again and again, so it would never switch in";
		case TapError::Unbacked:
			return "GPU work reached an engine target that has no memory";
		case TapError::NotExecuted:
			return "a skeleton clear or copy run did not execute what it took";
		case TapError::OwnEpoch:
			return "an anchor would run an epoch of its own";
		case TapError::MissedSettingsWriter:
			return "a feature's size settings changed with no settings event";
		case TapError::OutsideAdoption:
			return "a version was adopted or a build attached outside the frame's adoption point";
		case TapError::ClientInputChanged:
			return "what a client's graphs are built for changed after it first drew";
		case TapError::LateChange:
			return "a registration or structural build request came inside the frame, outside its adoption point";
		case TapError::EngineDraw:
			return "an engine draw reached the D3D11 context, which graph mode has no path for";
		case TapError::EngineWork:
			return "engine work (a clear, copy, map or query) reached the D3D11 context, and graph mode has no place for it";
		case TapError::Uncovered:
			return "an epoch a fragment source must cover was left to the engine while the source draws";
		case TapError::SceneMismatch:
			return "a Drawcall Limit Fix scene check found tracked state that the scene does not match";
		case TapError::DrawInputs:
			return "a Drawcall Limit Fix draw's inputs were incomplete";
		case TapError::EngineLeak:
			return "the engine drew a main-view pass of an object that Drawcall Limit Fix's frame set holds";
		case TapError::OutsideLedgers:
			return "the native immediate context ran work on a resource no graph pass or graph resource orders";
		case TapError::UndeclaredAccess:
			return "CS gave a shader a raw index or ref of a resource no pass declared, so the graph does not order it";
		case TapError::D3D11Path:
			return "a client the graph runs reached its D3D11 path, which graph mode never takes";
		default:
			return "the frame did not match its schedule";
		}
	}

	const char* ModeName()
	{
		if (!Engine::GraphMode())
			return "D3D11 mode";
		if (!Engine::Native::Requested())
			return "guest mode";
		if (::RenderGraph::Backend::IsD3D12())
			return "native D3D12 mode";
		return "native Vulkan mode";
	}

	namespace
	{
		/** @brief "RenderGraph/FrameAnchors.cpp:1985": the path from src/ on, and the line. */
		std::string SiteOf(const std::source_location& a_where)
		{
			std::string_view file = a_where.file_name();
			for (const std::string_view root : { "src/", "src\\" })
				if (const auto at = file.rfind(root); at != std::string_view::npos) {
					file.remove_prefix(at + root.size());
					break;
				}
			return fmt::format("{}:{}", file, a_where.line());
		}

		/**
		* @brief The violation policy: tap errors by TapError kind, fatal with MismatchFatal, and the session's violation warnings.
		* Made at its first use, which reads MismatchFatal.
		*/
		org::services::ViolationPolicy& Violations()
		{
			static org::services::ViolationPolicy s_policy({
				.kinds = kErrors,
				.fatal = MismatchFatal(),
				.onFatal = [](std::string_view a_message) {
					RenderGraph::Fatal("frame tap error", fmt::format("{} Development builds stop here (CS_ORG_MISMATCH_FATAL=0 logs it instead).", a_message));
				},
				.errorLabel = "[ORG] frame tap error",
				.firstNote = "Its work is not drawn. Later errors of this kind are counted, not logged.",
				.warningLabel = "[ORG] rule violation warning raised",
				.warningNote = "The menu shows it until the game exits",
			});
			return s_policy;
		}
	}

	void RaiseViolationWarning(std::string_view a_kind)
	{
		Violations().RaiseWarning(a_kind);
	}

	std::vector<std::string> ViolationWarnings()
	{
		return Violations().Warnings();
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
				on ? "stops the game" : "is logged once per kind, raises a menu warning, and its work is not drawn");
			return on;
		}();
		return fatal;
	}

	void NoteTapError(TapError a_error, SegmentId a_segment, std::string_view a_what, const std::source_location& a_where)
	{
		auto& state = Get();
		const auto where = a_segment < SegmentId::Count ? fmt::format("{} segment", SegmentName(a_segment)) : std::string("hosted work");
		// The full error. A test greps "[ORG] frame tap error".
		const auto message = fmt::format("{}: {} (tap error: {}). Frame {} ({}, slot {}), {}: {}. Raised at {}.", ModeName(),
			TapErrorMeaning(a_error), TapErrorName(a_error), state.current.frame, KindName(state.current.kind), state.current.slot, where, a_what,
			SiteOf(a_where));
		// Fatal with MismatchFatal. Otherwise counted: one line per kind for the session, and the menu warning.
		if (Violations().Note(static_cast<std::uint32_t>(a_error), message) == org::services::ViolationPolicy::Outcome::First)
			RaiseViolationWarning(fmt::format("tap error: {}", TapErrorName(a_error)));
	}

	void NoteUnbackedUse(std::string_view a_target, std::string_view a_what, const std::source_location& a_where)
	{
		if (!MismatchFatal()) {
			static std::mutex lock;
			static std::set<std::string, std::less<>> logged;
			std::scoped_lock guard(lock);
			if (logged.size() < 64 && logged.emplace(a_target).second)
				logger::error("[ORG] unbacked engine target used: {}; the operation is skipped (logged once per target)", a_what);
		}
		NoteTapError(TapError::Unbacked, SegmentId::Count, a_what, a_where);
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

	void NoteScheduled(SegmentId a_segment, FrameAnchors::Reason a_how)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		const auto s = static_cast<std::size_t>(a_segment);
		++state.segments[s].scheduled[ScheduledIndex(a_how)];
		if (a_how != FrameAnchors::Reason::Absent)
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
		if (FrameAnchors::Place(a_client).MayLag()) {
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
}
