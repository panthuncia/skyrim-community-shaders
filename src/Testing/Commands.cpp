// The command and wait registries, and the generic commands, which work with every feature off. A step names a command by "do" and
// passes "arg". Features register their own (DCLF's under "dclf:").
#include "Snapshot/EngineReads.h"
#include "Testing/Internal.h"

#include "CSEditor/EditorWindow.h"
#include "Feature.h"
#include "Features/Effects11.h"
#include "Features/Effects11/SettingManager.h"
#include "Features/HDRDisplay.h"
#include "Features/PostProcessing.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Menu.h"
#include "Profiler.h"
#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Frame/Recipe.h"
#include "ShaderCache.h"
#include "Snapshot/Publication.h"
#include "State.h"
#include "Utils/Climate.h"
#include "Utils/GameThread.h"
#include "Vram/MemProbe.h"
#include "Vram/SpillLab.h"
#include "Vram/VramLedger.h"

#include "Features/DrawcallLimitFix/Diagnostics/Capture/Capture.h"
#include "Features/DrawcallLimitFix/Diagnostics/TelemetryProfile.h"

#include <bit>
#include <charconv>
#include <set>

namespace Testing
{
	namespace
	{
		using PrefixCommand = std::function<bool(const std::string&, const std::string&, std::string&)>;

		struct Registry
		{
			std::mutex mutex;
			std::map<std::string, Command, std::less<>> commands;
			std::vector<std::pair<std::string, PrefixCommand>> prefixes;
			std::map<std::string, std::function<bool()>, std::less<>> waits;
			std::map<std::string, Command, std::less<>> featureCommands;  // "<owner>.<name>", for the "call" step
			std::set<std::string, std::less<>> signals;  // "signal" commands not yet taken by a wait
		};

		Registry& Commands()
		{
			static Registry registry;
			return registry;
		}

		// Frames left of a move or turn command, and its amount a frame. Render thread.
		struct Motion
		{
			std::uint32_t frames = 0;
			float amount = 0.0f;
		};
		Motion moving, turning;

		/**
		 * @brief "window drag:<W>x<H>:<frames>": a frame drag as Windows sends it. WM_ENTERSIZEMOVE, then one SetWindowPos a frame from
		 * the window's size to <W>x<H> (the client area), then WM_EXITSIZEMOVE on the frame after the last step. Render thread.
		 */
		struct Drag
		{
			HWND window = nullptr;
			LONG fromWidth = 0, fromHeight = 0;  // the window's outer size when the drag started
			LONG toWidth = 0, toHeight = 0;      // the outer size for the client area asked for
			std::uint32_t frames = 0;
			std::uint32_t step = 0;  // steps sent so far
			bool active = false;
		};
		Drag dragging;

		/** @brief One frame of a drag. */
		void TickDrag()
		{
			auto& drag = dragging;
			if (!drag.active)
				return;
			if (drag.step >= drag.frames) {
				const bool sent = ::PostMessageW(drag.window, WM_EXITSIZEMOVE, 0, 0);
				logger::info("[TEST] window drag: ended at {}x{} after {} steps ({})", drag.toWidth, drag.toHeight, drag.frames, sent ? "sent" : "failed");
				drag.active = false;
				return;
			}
			++drag.step;
			const auto lerp = [&](LONG a_from, LONG a_to) {
				return a_from + static_cast<LONG>((static_cast<std::int64_t>(a_to) - a_from) * drag.step / drag.frames);
			};
			::SetWindowPos(drag.window, nullptr, 0, 0, lerp(drag.fromWidth, drag.toWidth), lerp(drag.fromHeight, drag.toHeight),
				SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
		}

		/** @brief The a_index-th nearest actor (by distance to the player) that a_filter accepts, or null. Main thread. */
		template <class Filter>
		std::pair<RE::Actor*, float> NearestActor(std::size_t a_index, Filter&& a_filter)
		{
			auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton());
			auto* lists = CS_ENGINE_READ(Data, Other, RE::ProcessLists::GetSingleton());
			if (!player || !lists)
				return {};
			std::vector<std::pair<float, RE::Actor*>> actors;
			for (auto& handle : lists->highActorHandles) {
				auto actor = handle.get();
				if (actor && actor.get() != player && a_filter(*actor))
					actors.emplace_back(actor->GetPosition().GetDistance(player->GetPosition()), actor.get());
			}
			std::sort(actors.begin(), actors.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			return a_index < actors.size() ? std::pair{ actors[a_index].second, actors[a_index].first } : std::pair<RE::Actor*, float>{};
		}

		bool IsFemale(RE::Actor& a_actor)
		{
			auto* base = a_actor.GetActorBase();
			return base && !a_actor.IsDead() && base->IsFemale();
		}

		/** @brief Places the player a_distance in front of a_actor and a_side to its right, facing it. Main thread. */
		RE::NiPoint3 PlaceInFront(RE::PlayerCharacter* a_player, RE::Actor* a_actor, float a_distance, float a_side)
		{
			const float heading = a_actor->GetAngleZ();
			auto position = a_actor->GetPosition();
			position.x += std::sin(heading) * a_distance + std::cos(heading) * a_side;
			position.y += std::cos(heading) * a_distance - std::sin(heading) * a_side;
			a_player->SetPosition(position, true);
			a_player->SetHeading(heading + RE::NI_PI);
			return position;
		}

		// The commands' game work runs through the game-thread queue, on the main thread after the frame's jobs (MainTick). There the
		// engine is on one thread and its serving thread is idle, so a coc shows the loading screen as the console's does. SKSE's
		// tasks run in a frame job ("Main render end"): a coc there loads the cells inside the frame-job wait, with no loading
		// screen and no Present.
		void AddTask(std::function<void()> a_task)
		{
			GameThread::Post("test: a command's game work", std::move(a_task));
		}

		/**
		 * @brief "cast": <spell form ID>,<left|right>[,<hold seconds>]. The player casts a spell from a hand as a player does: the spell
		 * equipped and the hands drawn, then the hand's attack button pressed, held and released as mouse events. One cast at a time.
		 */
		class Cast
		{
		public:
			static bool Start(const std::string& a_arg, std::string& a_error)
			{
				char hand[16]{};
				unsigned formId = 0;
				float hold = 1.0f;
				if (std::sscanf(a_arg.c_str(), "%x,%15[a-z],%f", &formId, hand, &hold) < 2 || (std::strcmp(hand, "left") && std::strcmp(hand, "right"))) {
					a_error = fmt::format("'{}' is not <spell form ID>,<left|right>[,<hold seconds>]", a_arg);
					return false;
				}
				state = { formId, std::strcmp(hand, "left") == 0, hold, std::chrono::steady_clock::now(), Phase::Equip };
				return true;
			}

			static void OnFrame()
			{
				if (state.phase == Phase::Idle)
					return;
				const float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - state.start).count();
				switch (state.phase) {
				case Phase::Equip:
					AddTask([formId = state.formId, left = state.left] {
						auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton());
						auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(formId);
						// Skyrim.esm's LeftHand and RightHand equip slots.
						const auto* slot = RE::TESForm::LookupByID<RE::BGSEquipSlot>(left ? 0x13F43 : 0x13F42);
						if (!player || !spell || !slot) {
							logger::warn("[TEST] cast: no spell {:08X}", formId);
							return;
						}
						RE::ActorEquipManager::GetSingleton()->EquipSpell(player, spell, slot);
						player->DrawWeaponMagicHands(true);
					});
					state.phase = Phase::Draw;
					return;
				case Phase::Draw:
					if (since < kDrawSeconds)
						return;
					Send(state.left, 1.0f, 0.0f);
					state.phase = Phase::Hold;
					return;
				case Phase::Hold:
					{
						const float held = since - kDrawSeconds;
						if (held < state.hold) {
							Send(state.left, 1.0f, held);
							return;
						}
						Send(state.left, 0.0f, held);
						logger::info("[TEST] cast: {:08X} released after {:.2f} s", state.formId, held);
						state.phase = Phase::Idle;
						return;
					}
				default:
					return;
				}
			}

		private:
			enum class Phase
			{
				Idle,
				Equip,
				Draw,
				Hold
			};
			struct State
			{
				std::uint32_t formId = 0;
				bool left = false;
				float hold = 0.0f;
				std::chrono::steady_clock::time_point start;
				Phase phase = Phase::Idle;
			};
			static constexpr float kDrawSeconds = 1.5f;
			static inline State state;

			/** @brief One mouse button event for the hand's attack (the left button is the left hand's), on the main thread. */
			static void Send(bool a_left, float a_value, float a_held)
			{
				AddTask([a_left, a_value, a_held] {
					auto* events = RE::UserEvents::GetSingleton();
					auto* devices = RE::BSInputDeviceManager::GetSingleton();
					if (!events || !devices)
						return;
					// The input queue owns its events and reuses them, so this one is left to the heap.
					RE::InputEvent* event = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kMouse, a_left ? events->leftAttack : events->rightAttack, a_left ? 0u : 1u, a_value, a_held);
					if (event)
						devices->SendEvent(&event);
				});
			}
		};

		/** @brief "<frames>:<amount>": a move or turn spread over that many frames. */
		bool ParseMotion(const std::string& a_arg, Motion& a_motion, std::string& a_error)
		{
			unsigned frames = 0;
			float amount = 0.0f;
			if (std::sscanf(a_arg.c_str(), "%u:%f", &frames, &amount) != 2) {
				a_error = fmt::format("'{}' is not <frames>:<amount per frame>", a_arg);
				return false;
			}
			a_motion = { frames, amount };
			return true;
		}

		bool ConsoleCommand(const std::string& a_line, std::string& a_error)
		{
			if (a_line.empty()) {
				a_error = "no console command";
				return false;
			}
			AddTask([a_line] {
				const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
				if (auto* script = factory ? static_cast<RE::Script*>(factory->Create()) : nullptr) {
					script->SetCommand(a_line);
					script->CompileAndRun(nullptr);
					delete script;
				}
			});
			return true;
		}

		/** @brief The model path of a load screen's object, or empty. */
		std::string LoadScreenModel(const RE::TESLoadScreen::LoadNIFData* a_data)
		{
			const auto* nif = a_data ? a_data->loadNIF : nullptr;
			const auto* model = nif ? nif->As<RE::TESModel>() : nullptr;
			return model && model->GetModel() ? std::string(model->GetModel()) : std::string{};
		}

		/** @brief The open loading menu's load screen model, or empty. */
		std::string ShownLoadScreenModel()
		{
			auto* ui = CS_ENGINE_READ(UI, Other, RE::UI::GetSingleton());
			const auto menu = ui ? ui->GetMenu<RE::LoadingMenu>() : nullptr;
			const auto* screen = menu ? menu->GetRuntimeData().loadScreen : nullptr;
			return screen ? LoadScreenModel(screen->loadNIFData) : std::string{};
		}

		/** @brief Each load screen's own object, kept from the first call, as "loadscreen" points every screen at one. */
		std::map<RE::TESLoadScreen*, RE::TESLoadScreen::LoadNIFData*>& OwnLoadScreens(RE::TESDataHandler& a_data)
		{
			static std::map<RE::TESLoadScreen*, RE::TESLoadScreen::LoadNIFData*> own;
			if (own.empty())
				for (auto* screen : a_data.GetFormArray<RE::TESLoadScreen>())
					if (screen)
						own[screen] = screen->loadNIFData;
			return own;
		}

		/**
		 * @brief "loadscreens <frames>[,<model substring>]": the Mist Menu shows every distinct load screen model (or each whose path holds
		 * the substring, any case) in turn, <frames> frames each, as LoadingMenu
		 * shows the one it picks (AE 0x140935870 calls the setup below with the screen's object data). Run it at the main menu, where the
		 * Mist Menu is open and no load replaces the model. Raises signal 900 (a step's wait = "signal:900") once the last model had
		 * its frames.
		 */
		class LoadScreenCycle
		{
		public:
			static bool Start(const std::string& a_arg, std::string& a_error)
			{
				const auto comma = a_arg.find(',');
				const std::string count = a_arg.substr(0, comma);
				const auto lower = [](std::string a_text) {
					std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					return a_text;
				};
				const std::string filter = comma == std::string::npos ? std::string{} : lower(a_arg.substr(comma + 1));
				std::uint32_t frames = 0;
				const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), frames);
				auto* data = CS_ENGINE_READ(Data, Other, RE::TESDataHandler::GetSingleton());
				if (error != std::errc{} || end != count.data() + count.size() || !frames || !data) {
					a_error = fmt::format("loadscreens '{}' needs the frames each model shows (and a model substring) and loaded data", a_arg);
					return false;
				}
				// One entry per model path, in path order, so two runs show the same sequence.
				std::map<std::string, const RE::TESLoadScreen::LoadNIFData*> byModel;
				for (const auto& [screen, nif] : OwnLoadScreens(*data))
					if (const auto model = LoadScreenModel(nif); !model.empty() && lower(model).find(filter) != std::string::npos)
						byModel.try_emplace(model, nif);
				std::scoped_lock lock(mutex);
				models.clear();
				for (const auto& [model, nif] : byModel)
					models.emplace_back(model, nif);
				framesEach = frames;
				left = 0;
				next = 0;
				done = models.empty();
				logger::info("[TEST] loadscreens: {} distinct models, {} frames each", models.size(), frames);
				return true;
			}

			/** @brief Once a frame: the next model when the current one had its frames. */
			static void OnFrame()
			{
				std::scoped_lock lock(mutex);
				if (done || models.empty() || (left && --left))
					return;
				if (next == models.size()) {
					done = true;
					logger::info("[TEST] loadscreens: all {} models shown", models.size());
					auto& registry = Commands();
					std::scoped_lock signalLock(registry.mutex);
					registry.signals.insert("900");
					return;
				}
				const auto index = next++;
				left = framesEach;
				AddTask([index, model = models[index].first, nif = models[index].second, count = models.size()] {
					logger::info("[TEST] loadscreens: model {} of {}: {}", index + 1, count, model);
					Show(*nif);
				});
			}

		private:
			/** @brief Main thread. The Mist Menu's load screen model setup (SE 51454, AE 52313) with a screen's object data. */
			static void Show(const RE::TESLoadScreen::LoadNIFData& a_nif)
			{
				auto* mist = RE::MistMenu::GetSingleton();
				const auto* model = a_nif.loadNIF ? a_nif.loadNIF->As<RE::TESModel>() : nullptr;
				if (!mist || !model) {
					logger::warn("[TEST] loadscreens: no Mist Menu or no model");
					return;
				}
				// The setup does nothing while a model shows; it detaches the shown one itself.
				mist->GetRuntimeData().showLoadScreen = false;
				// TESLoadScreen's rotation in degrees, as LoadingMenu's pick turns it into radians (AE 0x14033af60, the exe's 0x3C8EFA35).
				constexpr float kRadians = std::bit_cast<float>(0x3C8EFA35u);
				const float rotation[3]{ a_nif.rotationConstraints[0] * kRadians, a_nif.rotationConstraints[1] * kRadians, a_nif.rotationConstraints[2] * kRadians };
				const char* cameraPath = a_nif.cameraPath.GetModel();
				using Setup = void (*)(const RE::TESModel*, float, const float*, const float*, const char*, float, float);
				static const REL::Relocation<Setup> setup{ RELOCATION_ID(51454, 52313) };
				setup(model, a_nif.initialScale, rotation, a_nif.initialTranslationOffset, cameraPath ? cameraPath : "", static_cast<float>(a_nif.rotationOffsetConstraints[0]),
					static_cast<float>(a_nif.rotationOffsetConstraints[1]));
			}

			static inline std::mutex mutex;
			static inline std::vector<std::pair<std::string, const RE::TESLoadScreen::LoadNIFData*>> models;
			static inline std::uint32_t framesEach = 0, left = 0;
			static inline std::size_t next = 0;
			static inline bool done = false;
		};

		/**
		 * @brief The "loadscreen" command with a model path substring. Every load screen shows the object of the first one whose model holds
		 * it, so the next loads show that model whichever screen the game picks.
		 */
		bool ForceLoadScreen(const std::string& a_arg, std::string& a_error)
		{
			auto* data = CS_ENGINE_READ(Data, Other, RE::TESDataHandler::GetSingleton());
			if (!data || a_arg.empty()) {
				a_error = "a load screen needs a model substring and loaded data";
				return false;
			}
			const auto lower = [](std::string a_text) {
				std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return a_text;
			};
			auto& screens = data->GetFormArray<RE::TESLoadScreen>();
			auto& own = OwnLoadScreens(*data);
			RE::TESLoadScreen::LoadNIFData* chosen = nullptr;
			std::string model;
			for (auto* screen : screens) {
				if (!screen)
					continue;
				if (lower(model = LoadScreenModel(own[screen])).find(lower(a_arg)) != std::string::npos) {
					chosen = own[screen];
					break;
				}
			}
			if (!chosen) {
				a_error = fmt::format("no load screen's model holds '{}'", a_arg);
				return false;
			}
			for (auto* screen : screens)
				if (screen)
					screen->loadNIFData = chosen;
			logger::info("[TEST] loadscreen: {} load screens show {}", screens.size(), model);
			return true;
		}

		bool Shot(const std::string& a_arg, std::string& a_error)
		{
			if (a_arg.empty()) {
				a_error = "a shot needs a name";
				return false;
			}
			if (const auto model = ShownLoadScreenModel(); !model.empty())
				logger::info("[TEST] shot {} with the loading menu showing {}", a_arg, model);
			auto directory = RunDirectory();
			directory = directory.empty() ? logger::log_directory().value_or(std::filesystem::path{}) / "dclf-shots" : directory / "shots";
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			// Decision 51: inside a game update the shot renders the next update's snapshot, the first with this step's work in it (the
			// steps' game work runs after the update). A repeated or older snapshot is not shot. Outside an update (the main menu, a
			// loading screen), the next frame.
			const std::uint64_t minSnapshot = Snapshot::InGameUpdate() ? Snapshot::LastGameId() + 1 : 0;
			globals::features::screenshotFeature.RequestCapture(directory / (a_arg + ".png"), minSnapshot);
			logger::info("[TEST] shot {} asked at game snapshot {}: renders snapshot {} or newer", a_arg, Snapshot::LastGameId(), minSnapshot);
			auto& run = Run();
			std::scoped_lock lock(run.mutex);
			run.shots.push_back(a_arg);
			return true;
		}

		bool Window(const std::string& a_arg, std::string& a_error)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			const HWND window = globals::d3d::swapChain && SUCCEEDED(globals::d3d::swapChain->GetDesc(&desc)) ? desc.OutputWindow : nullptr;
			if (!window) {
				a_error = "the game has no swap chain window yet";
				return false;
			}
			unsigned width = 0, height = 0, frames = 0;
			bool done = false;
			if (a_arg.starts_with("drag:")) {
				if (std::sscanf(a_arg.c_str(), "drag:%ux%u:%u", &width, &height, &frames) != 3 || !width || !height || !frames) {
					a_error = fmt::format("'{}' is not drag:<W>x<H>:<frames>", a_arg);
					return false;
				}
				if (dragging.active) {
					a_error = "a window drag is still running";
					return false;
				}
				RECT outer{};
				if (!::GetWindowRect(window, &outer)) {
					a_error = "the window's size cannot be read";
					return false;
				}
				RECT rect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
				::AdjustWindowRectEx(&rect, static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_STYLE)), FALSE, static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE)));
				dragging = { window, outer.right - outer.left, outer.bottom - outer.top, rect.right - rect.left, rect.bottom - rect.top, frames, 0, true };
				// Posted before the first step, so the window thread sees the drag start first, as in a real drag.
				done = ::PostMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
			} else if (a_arg == "minimize" || a_arg == "restore") {
				done = ::PostMessageW(window, WM_SYSCOMMAND, a_arg == "minimize" ? SC_MINIMIZE : SC_RESTORE, 0);
			} else if (a_arg == "background") {
				done = ::SetForegroundWindow(::GetShellWindow());
			} else if (a_arg == "foreground") {
				done = ::SetForegroundWindow(window);
			} else if (std::sscanf(a_arg.c_str(), "%ux%u", &width, &height) == 2 && width && height) {
				// The client area at that size, the frame around it as the window's style draws it.
				RECT rect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
				::AdjustWindowRectEx(&rect, static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_STYLE)), FALSE, static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE)));
				done = ::SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
			} else {
				a_error = fmt::format("'{}' is not minimize, restore, background, foreground, <W>x<H> or drag:<W>x<H>:<frames>", a_arg);
				return false;
			}
			logger::info("[TEST] window {}: {}", a_arg, done ? "sent" : "failed");
			return true;
		}

		/** @brief A weather by editor id, else by hex form id. Null with a_error set when there is none. */
		RE::TESWeather* FindWeather(const std::string& a_arg, std::string& a_error)
		{
			RE::TESWeather* weather = nullptr;
			std::string known;
			if (auto* data = CS_ENGINE_READ(Data, Other, RE::TESDataHandler::GetSingleton())) {
				for (auto* candidate : data->GetFormArray<RE::TESWeather>()) {
					if (!candidate)
						continue;
					const char* name = candidate->GetFormEditorID();
					if (name && *name && _stricmp(name, a_arg.c_str()) == 0)
						weather = candidate;
					known += fmt::format("{}{:08X} {}", known.empty() ? "" : ", ", candidate->GetFormID(), name && *name ? name : "?");
				}
			}
			if (!weather) {
				char* end = nullptr;
				const auto id = static_cast<RE::FormID>(std::strtoul(a_arg.c_str(), &end, 16));
				if (end && *end == '\0')
					weather = RE::TESForm::LookupByID<RE::TESWeather>(id);
			}
			if (!weather)
				a_error = fmt::format("no weather '{}'. Loaded weathers: {}", a_arg, known);
			return weather;
		}

		bool Weather(const std::string& a_arg, std::string& a_error)
		{
			auto* editor = EditorWindow::GetSingleton();
			if (a_arg == "off") {
				editor->UnlockWeather();
				return true;
			}
			auto* weather = FindWeather(a_arg, a_error);
			if (!weather)
				return false;
			logger::info("[TEST] weather {:08X} {}", weather->GetFormID(), weather->GetFormEditorID() ? weather->GetFormEditorID() : "?");
			// The CS editor's lock holds it against scripts and climate timers; the console's fw depends on frame timing.
			editor->LockWeatherForUser(weather);
			return true;
		}

		bool SetFeature(const std::string& a_arg, std::string& a_error)
		{
			// "<ShortName>=0|1".
			const auto equals = a_arg.rfind('=');
			if (equals == std::string::npos) {
				a_error = fmt::format("'{}' is not <ShortName>=0|1", a_arg);
				return false;
			}
			const std::string name = a_arg.substr(0, equals);
			const std::string value = a_arg.substr(equals + 1);
			const auto& list = Feature::GetFeatureList();
			if (std::ranges::none_of(list, [&](Feature* a_feature) { return a_feature && a_feature->GetShortName() == name; })) {
				a_error = fmt::format("no feature '{}'", name);
				return false;
			}
			globals::state->SetFeatureDisabled(name, value == "0");
			return true;
		}

		// A key segment names an object member, or with "[n]" an array element. A member name matches with its spaces
		// dropped ("CODBloom" names "COD Bloom"). A segment absent from an object passes through the object's "settings"
		// member, which is where PostProcessing keeps each sub-effect's settings.
		json* FindSettingMember(json& a_object, std::string_view a_name, std::string& a_path)
		{
			if (!a_object.is_object())
				return nullptr;
			if (const auto exact = a_object.find(std::string(a_name)); exact != a_object.end()) {
				a_path += a_name;
				return &*exact;
			}
			const auto withoutSpaces = [](std::string_view a_text) {
				std::string out;
				std::ranges::copy_if(a_text, std::back_inserter(out), [](char a_char) { return a_char != ' '; });
				return out;
			};
			json* match = nullptr;
			for (auto& [key, member] : a_object.items()) {
				if (withoutSpaces(key) != a_name)
					continue;
				if (match)
					return nullptr;  // ambiguous
				match = &member;
				a_path += key;
			}
			if (match)
				return match;
			if (const auto nested = a_object.find("settings"); nested != a_object.end() && nested->is_object()) {
				const auto size = a_path.size();
				a_path += "settings.";
				if (auto* found = FindSettingMember(*nested, a_name, a_path))
					return found;
				a_path.resize(size);
			}
			return nullptr;
		}

		/** @brief The value a_key ("a.b[3].c") names inside a_settings, or null. a_path gets the key with the names as stored. */
		json* FindSettingByKey(json& a_settings, std::string_view a_key, std::string& a_path)
		{
			json* current = &a_settings;
			std::size_t at = 0;
			while (at <= a_key.size()) {
				const auto end = std::min(a_key.find('.', at), a_key.size());
				const auto segment = a_key.substr(at, end - at);
				const auto bracket = segment.find('[');
				const auto name = segment.substr(0, bracket);
				if (name.empty())
					return nullptr;
				if (!a_path.empty())
					a_path += '.';
				current = FindSettingMember(*current, name, a_path);
				if (!current)
					return nullptr;
				// Then each "[n]" in turn.
				for (auto open = bracket; open != std::string_view::npos; open = segment.find('[', open)) {
					const auto close = segment.find(']', open);
					if (close == std::string_view::npos)
						return nullptr;
					std::size_t index = 0;
					const auto digits = segment.substr(open + 1, close - open - 1);
					const auto [parsedEnd, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
					if (digits.empty() || error != std::errc{} || parsedEnd != digits.data() + digits.size() || !current->is_array() || index >= current->size())
						return nullptr;
					current = &(*current)[index];
					a_path += fmt::format("[{}]", index);
					open = close + 1;
					if (open < segment.size() && segment[open] != '[')
						return nullptr;
				}
				at = end + 1;
			}
			return current;
		}

		/** @brief "<ShortName>={...}": a_object merged into the feature's settings JSON, applied once, as a preset button writes. */
		/** @brief Whether a_value can replace a_current: the same JSON type, any number for a number. A null setting takes anything. */
		bool SameSettingType(const json& a_current, const json& a_value)
		{
			if (a_current.is_null())
				return true;
			if (a_current.is_number() && a_value.is_number())
				return true;
			return a_current.type() == a_value.type();
		}

		/** @brief Loads a_settings into a_feature. A throw puts a_previous back and reports it as a_error. */
		bool ApplySettings(Feature& a_feature, json& a_settings, json& a_previous, std::string& a_error)
		{
			try {
				a_feature.LoadSettings(a_settings);
				return true;
			} catch (const std::exception& e) {
				a_error = fmt::format("{} refused the settings: {}", a_feature.GetShortName(), e.what());
			}
			try {
				a_feature.LoadSettings(a_previous);
			} catch (...) {
				a_error += " (its previous settings could not be put back)";
			}
			return false;
		}

		bool SetSettings(const std::string& a_name, const std::string& a_object, std::string& a_error)
		{
			const auto& list = Feature::GetFeatureList();
			const auto found = std::ranges::find_if(list, [&](Feature* a_feature) { return a_feature && a_feature->GetShortName() == a_name; });
			if (found == list.end()) {
				a_error = fmt::format("no feature '{}'", a_name);
				return false;
			}
			const auto patch = json::parse(a_object, nullptr, false);
			if (patch.is_discarded() || !patch.is_object() || patch.empty()) {
				a_error = fmt::format("'{}' is not a JSON object of settings", a_object);
				return false;
			}
			json settings;
			(*found)->SaveSettings(settings);
			if (!settings.is_object()) {
				a_error = fmt::format("{} has no settings object", a_name);
				return false;
			}
			json previous = settings;
			for (const auto& [key, value] : patch.items()) {
				if (!settings.contains(key)) {
					a_error = fmt::format("{} has no setting '{}'", a_name, key);
					return false;
				}
				if (!SameSettingType(settings[key], value)) {
					a_error = fmt::format("{}.{} holds a {}, not a {}", a_name, key, settings[key].type_name(), value.type_name());
					return false;
				}
				settings[key] = value;
			}
			if (!ApplySettings(**found, settings, previous, a_error))
				return false;
			logger::info("[TEST] settings {} = {}", a_name, patch.dump());
			return true;
		}

		bool SetSetting(const std::string& a_arg, std::string& a_error)
		{
			// "<ShortName>.<key>=<JSON value>", one setting written as a menu widget writes it (through the feature's settings JSON).
			// The key may be a path into nested objects and arrays, "PostProcessing.CODBloom.MipBlendFactor[3]=0.2".
			// "<ShortName>={...}" writes several settings at once, as a preset button does: the object is merged into the settings.
			const auto equals = a_arg.find('=');
			if (const auto dot = a_arg.find('.'); equals != std::string::npos && (dot == std::string::npos || dot > equals))
				return SetSettings(a_arg.substr(0, equals), a_arg.substr(equals + 1), a_error);
			const auto dot = a_arg.find('.');
			if (dot == std::string::npos || equals == std::string::npos || equals < dot) {
				a_error = fmt::format("'{}' is not <feature>.<key>=<value> or <feature>={{...}}", a_arg);
				return false;
			}
			const auto name = a_arg.substr(0, dot);
			const auto key = a_arg.substr(dot + 1, equals - dot - 1);
			const auto& list = Feature::GetFeatureList();
			const auto found = std::ranges::find_if(list, [&](Feature* a_feature) { return a_feature && a_feature->GetShortName() == name; });
			if (found == list.end()) {
				a_error = fmt::format("no feature '{}'", name);
				return false;
			}
			json settings;
			(*found)->SaveSettings(settings);
			std::string path;
			json* target = nullptr;
			if (settings.is_object() && settings.contains(key)) {
				target = &settings[key];  // the flat form, whatever the key holds
				path = key;
			} else {
				target = FindSettingByKey(settings, key, path);
			}
			if (!target) {
				a_error = fmt::format("{} has no setting '{}'", name, key);
				return false;
			}
			const auto value = json::parse(a_arg.substr(equals + 1), nullptr, false);
			if (value.is_discarded()) {
				a_error = fmt::format("'{}' is not a JSON value", a_arg.substr(equals + 1));
				return false;
			}
			if (!SameSettingType(*target, value)) {
				a_error = fmt::format("{}.{} holds a {}, not a {}", name, path, target->type_name(), value.type_name());
				return false;
			}
			json previous = settings;
			*target = value;
			if (!ApplySettings(**found, settings, previous, a_error))
				return false;
			logger::info("[TEST] setting {}.{} = {}", name, path, value.dump());
			return true;
		}

		void RegisterGeneric()
		{
			RegisterCommand("console", ConsoleCommand);
			RegisterCommand("exit", [](const std::string&, std::string&) {
				RequestExit();
				return true;
			});
			RegisterCommand("shot", Shot);
			RegisterCommand("loadscreen", ForceLoadScreen);
			RegisterCommand("loadscreens", LoadScreenCycle::Start);
			RegisterCommand("window", Window);
			RegisterCommand("weather", Weather);
			RegisterCommand("feature", SetFeature);
			RegisterCommand("setting", SetSetting);
			// "timestep": the game timer's fixed step in milliseconds (BSTimer's clamp, what iFPSClamp sets), 0 for the wall clock. With a
			// fixed step every update advances the same game time, so runs at different render rates reach the same game time per frame.
			RegisterCommand("timestep", [](const std::string& a_arg, std::string& a_error) {
				float ms = 0.0f;
				const auto [end, error] = std::from_chars(a_arg.data(), a_arg.data() + a_arg.size(), ms);
				auto* timer = RE::BSTimer::GetSingleton();
				if (error != std::errc{} || end != a_arg.data() + a_arg.size() || ms < 0.0f || !timer) {
					a_error = fmt::format("timestep '{}' is not a step in milliseconds (0 for the wall clock)", a_arg);
					return false;
				}
				timer->clamp = ms;
				timer->clampRemainder = 0.0f;
				logger::info("[TEST] timestep: the game timer steps {}", ms > 0.0f ? fmt::format("{} ms an update", ms) : std::string("by the wall clock"));
				return true;
			});
			// "profiler": "on" keeps GPU and CPU pass timing running as the performance overlay does; "dump" logs every pass it holds.
			RegisterCommand("profiler", [](const std::string& a_arg, std::string& a_error) {
				if (a_arg == "on") {
					globals::profiler->KeepActive(1u << 30);
					return true;
				}
				if (a_arg != "dump") {
					a_error = "profiler takes on or dump";
					return false;
				}
				const auto stats = globals::profiler->GetStatistics();
				logger::info("[TEST] profiler: {} passes, {:.3f} ms GPU in the last frame", stats.size(), globals::profiler->GetTotalTimeMs());
				for (const auto& pass : stats)
					logger::info("[TEST] profiler pass '{}': GPU {:.3f} ms (avg {:.3f}), CPU {:.3f} ms, valid {}", pass.name, pass.gpuTimeMs, pass.avgMs, pass.cpuTimeMs, pass.valid);
				return true;
			});
			// "call": <owner>.<name>[=<arg>], a feature's registered test command (RegisterFeatureCommand).
			RegisterCommand("call", [](const std::string& a_arg, std::string& a_error) {
				const auto equals = a_arg.find('=');
				const std::string target = a_arg.substr(0, equals);
				const auto dot = target.find('.');
				if (dot == std::string::npos || dot == 0 || dot + 1 == target.size()) {
					a_error = fmt::format("'{}' is not <owner>.<name>[=<arg>]", a_arg);
					return false;
				}
				const std::string arg = equals == std::string::npos ? std::string{} : a_arg.substr(equals + 1);
				return RunFeatureCommand(std::string_view(target).substr(0, dot), std::string_view(target).substr(dot + 1), arg, a_error);
			});
			RegisterCommand("cast", Cast::Start);
			RegisterCommand("move", [](const std::string& a_arg, std::string& a_error) { return ParseMotion(a_arg, moving, a_error); });
			RegisterCommand("turn", [](const std::string& a_arg, std::string& a_error) { return ParseMotion(a_arg, turning, a_error); });
			RegisterCommand("signal", [](const std::string& a_arg, std::string&) {
				auto& registry = Commands();
				std::scoped_lock lock(registry.mutex);
				registry.signals.insert(a_arg);
				return true;
			});
			RegisterCommand("mark", [](const std::string& a_arg, std::string&) {
				MemProbe::Mark(a_arg, Run().frame);
				return true;
			});
			RegisterCommand("vram-json", [](const std::string&, std::string&) {
				const auto path = Vram::WriteJson(logger::log_directory().value_or(std::filesystem::path{}));
				logger::info("[TEST] VRAM attribution written to {}", path.string());
				return true;
			});
			// "pov": first|third, as the POV key switches it; no console command does.
			RegisterCommand("pov", [](const std::string& a_arg, std::string& a_error) {
				if (a_arg != "first" && a_arg != "third") {
					a_error = "not first or third";
					return false;
				}
				AddTask([first = a_arg == "first"] {
					if (auto* camera = CS_ENGINE_READ(Camera, Other, RE::PlayerCamera::GetSingleton()))
						first ? camera->ForceFirstPerson() : camera->ForceThirdPerson();
				});
				return true;
			});
			// "draw": 1|0, the player's weapons or magic hands drawn or sheathed.
			RegisterCommand("draw", [](const std::string& a_arg, std::string&) {
				AddTask([draw = a_arg != "0"] {
					if (auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton()))
						player->DrawWeaponMagicHands(draw);
				});
				return true;
			});
			// "face-npc": <n>[,<distance>[,<side>]], the player placed in front of the n-th nearest actor in its cell, facing it.
			RegisterCommand("face-npc", [](const std::string& a_arg, std::string&) {
				std::size_t index = 0;
				float distance = 110.0f, side = 0.0f;
				std::sscanf(a_arg.c_str(), "%zu,%f,%f", &index, &distance, &side);
				AddTask([index, distance, side] {
					auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton());
					const auto [actor, away] = NearestActor(index, [&](RE::Actor& a_actor) { return !a_actor.IsDead() && player && a_actor.GetParentCell() == player->GetParentCell(); });
					if (!actor) {
						logger::info("[TEST] face-npc: no actor at {}", index);
						return;
					}
					const auto position = PlaceInFront(player, actor, distance, side);
					logger::info("[TEST] face-npc: {} ({:08X}) at {} units, player placed at ({:.0f} {:.0f} {:.0f})", actor->GetDisplayFullName(), actor->GetFormID(), away,
						position.x, position.y, position.z);
				});
				return true;
			});
			// "face-female": <n>,<distance>,<side>[,strip], as face-npc among the female actors; strip unequips her worn armor.
			RegisterCommand("face-female", [](const std::string& a_arg, std::string&) {
				std::size_t index = 0;
				float distance = 110.0f, side = 0.0f;
				char flag[16]{};
				std::sscanf(a_arg.c_str(), "%zu,%f,%f,%15s", &index, &distance, &side, flag);
				const bool strip = std::strcmp(flag, "strip") == 0;
				AddTask([index, distance, side, strip] {
					auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton());
					auto* actor = NearestActor(index, IsFemale).first;
					if (!player || !actor) {
						logger::info("[TEST] face-female: no female actor at {}", index);
						return;
					}
					std::uint32_t stripped = 0;
					if (strip) {
						auto* equip = RE::ActorEquipManager::GetSingleton();
						for (auto& [object, entry] : actor->GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); }))
							if (entry.second && entry.second->IsWorn()) {
								equip->UnequipObject(actor, object);
								++stripped;
							}
					}
					const auto position = PlaceInFront(player, actor, distance, side);
					logger::info("[TEST] face-female: {} ({:08X}), {} armor pieces unequipped, player placed at ({:.0f} {:.0f} {:.0f})", actor->GetDisplayFullName(),
						actor->GetFormID(), stripped, position.x, position.y, position.z);
				});
				return true;
			});
			// "morph-female": <n>,<morph>[,<value>], a RaceMenu body morph on the n-th nearest female actor, through skee's Papyrus functions.
			RegisterCommand("morph-female", [](const std::string& a_arg, std::string& a_error) {
				std::size_t index = 0;
				char morph[64]{};
				float value = 1.0f;
				if (std::sscanf(a_arg.c_str(), "%zu,%63[^,],%f", &index, morph, &value) < 2) {
					a_error = fmt::format("'{}' is not <n>,<morph>[,<value>]", a_arg);
					return false;
				}
				AddTask([index, morphName = std::string(morph), value] {
					auto* actor = NearestActor(index, IsFemale).first;
					auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
					if (!actor || !vm) {
						logger::info("[TEST] morph-female: no female actor at {}", index);
						return;
					}
					RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
					RE::TESObjectREFR* reference = actor;
					const bool set = vm->DispatchStaticCall("NiOverride", "SetBodyMorph",
						RE::MakeFunctionArguments(std::move(reference), RE::BSFixedString(morphName), RE::BSFixedString("DCLFTest"), float(value)), none);
					reference = actor;
					const bool update = vm->DispatchStaticCall("NiOverride", "UpdateModelWeight", RE::MakeFunctionArguments(std::move(reference)), none);
					logger::info("[TEST] morph-female: {} ({:08X}) {} = {}: SetBodyMorph {}, UpdateModelWeight {}", actor->GetDisplayFullName(), actor->GetFormID(), morphName,
						value, set ? "queued" : "not found", update ? "queued" : "not found");
				});
				return true;
			});
			// "npc-alpha": <n>,<alpha>, Actor::SetAlpha on the n-th nearest actor in the player's cell.
			RegisterCommand("npc-alpha", [](const std::string& a_arg, std::string&) {
				const auto index = static_cast<std::size_t>(std::strtoul(a_arg.c_str(), nullptr, 10));
				const auto comma = a_arg.find(',');
				const float alpha = comma == std::string::npos ? 1.0f : std::strtof(a_arg.c_str() + comma + 1, nullptr);
				AddTask([index, alpha] {
					auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton());
					auto* actor = NearestActor(index, [&](RE::Actor& a_actor) { return player && a_actor.GetParentCell() == player->GetParentCell(); }).first;
					if (actor) {
						actor->SetAlpha(alpha);
						logger::info("[TEST] npc-alpha: {} ({:08X}) set to {}", actor->GetDisplayFullName(), actor->GetFormID(), alpha);
					}
				});
				return true;
			});
			// "shot-ui": a capture as the menu's button takes it, of the next frame's clean scene, into the user's screenshot folder.
			RegisterCommand("shot-ui", [](const std::string&, std::string&) {
				globals::features::screenshotFeature.captureRequested = true;
				return true;
			});
			// "cs-menu": open|close, Community Shaders' own menu. font=<px> sets a fixed font size (a font reload) and font=saved puts the
			// saved size back, so closing the menu saves the user's settings unchanged. Each acts at the next ImGui frame's start, as
			// the menu's own input does.
			RegisterCommand("cs-menu", [](const std::string& a_arg, std::string& a_error) {
				static std::optional<std::pair<bool, float>> savedFont;
				auto* menu = Menu::GetSingleton();
				if (a_arg == "open" || a_arg == "close") {
					menu->RequestOpen(a_arg == "open");
				} else if (a_arg == "font=saved") {
					menu->PostUiTask([menu] {
						if (savedFont)
							std::tie(menu->GetSettings().UseResolutionFont, menu->GetSettings().Theme.FontSize) = *std::exchange(savedFont, std::nullopt);
					});
				} else if (a_arg.starts_with("font=")) {
					menu->PostUiTask([menu, size = std::strtof(a_arg.c_str() + 5, nullptr)] {
						auto& settings = menu->GetSettings();
						if (!savedFont)
							savedFont = std::pair{ settings.UseResolutionFont, settings.Theme.FontSize };
						settings.UseResolutionFont = false;
						settings.Theme.FontSize = size;
					});
				} else {
					a_error = "cs-menu takes open, close, font=<px> or font=saved";
					return false;
				}
				return true;
			});
			// "cs-ui": an action taken as the CS menu's and editor's widgets take it, inside the next ImGui frame, so its engine writes
			// go through the game-thread queue: setting <ShortName>.<key>=<JSON>, hour=<h>, timescale=<x>, pause, resume,
			// lock=<weather editor id or hex form id>, unlock. Check the result with "cs-ui-expect" a few frames later.
			RegisterCommand("cs-ui", [](const std::string& a_arg, std::string& a_error) {
				auto* menu = Menu::GetSingleton();
				auto* editor = EditorWindow::GetSingleton();
				float value = 0.0f;
				if (a_arg.starts_with("setting ")) {
					menu->PostUiTask([line = a_arg.substr(8)] {
						std::string error;
						if (!SetSetting(line, error))
							logger::error("[TEST] cs-ui setting {}: {}", line, error);
					});
				} else if (std::sscanf(a_arg.c_str(), "hour=%f", &value) == 1) {
					menu->PostUiTask([editor, value] { editor->SetGameHour(value); });
				} else if (std::sscanf(a_arg.c_str(), "timescale=%f", &value) == 1) {
					menu->PostUiTask([editor, value] { editor->SetTimeScale(value); });
				} else if (a_arg == "pause" || a_arg == "resume") {
					menu->PostUiTask([editor, pause = a_arg == "pause"] { pause ? editor->PauseTime() : editor->ResumeTime(); });
				} else if (a_arg.starts_with("lock=")) {
					auto* weather = FindWeather(a_arg.substr(5), a_error);
					if (!weather)
						return false;
					menu->PostUiTask([editor, weather] { editor->LockWeatherForUser(weather); });
				} else if (a_arg == "unlock") {
					menu->PostUiTask([editor] { editor->ReleaseUserWeatherLock(); });
				} else {
					a_error = "cs-ui takes setting <ShortName>.<key>=<JSON>, hour=<h>, timescale=<x>, pause, resume, lock=<weather> or unlock";
					return false;
				}
				logger::info("[TEST] cs-ui {}: queued for the ImGui frame", a_arg);
				return true;
			});
			// "cs-ui-expect": fails unless the game state is as a "cs-ui" action left it: hour=<h> (within 0.05), timescale=<x>,
			// paused, running, lock=<weather>, unlocked, setting <ShortName>.<key>=<JSON> (the feature's saved value), ui-writes=0
			// (no engine write from the ImGui frame went around the game-thread queue).
			RegisterCommand("cs-ui-expect", [](const std::string& a_arg, std::string& a_error) {
				auto* editor = EditorWindow::GetSingleton();
				auto calendar = Util::Climate::GetCalendar();
				auto* sky = CS_ENGINE_READ(Sky, Other, globals::game::sky);
				float value = 0.0f;
				std::string seen;
				bool holds = false;
				if (std::sscanf(a_arg.c_str(), "hour=%f", &value) == 1) {
					const float hour = calendar && calendar->gameHour ? calendar->gameHour->value : -1.0f;
					holds = std::abs(hour - value) < 0.05f;
					seen = fmt::format("the game hour is {:.3f}", hour);
				} else if (std::sscanf(a_arg.c_str(), "timescale=%f", &value) == 1) {
					const float scale = calendar && calendar->timeScale ? calendar->timeScale->value : -1.0f;
					holds = std::abs(scale - value) < 0.001f;
					seen = fmt::format("the timescale is {:.3f}", scale);
				} else if (a_arg == "paused" || a_arg == "running") {
					const float scale = calendar && calendar->timeScale ? calendar->timeScale->value : -1.0f;
					holds = (a_arg == "paused") == (editor->IsTimePaused() && scale == 0.0f);
					seen = fmt::format("time is {} with timescale {:.3f}", editor->IsTimePaused() ? "paused" : "running", scale);
				} else if (a_arg.starts_with("lock=")) {
					auto* weather = FindWeather(a_arg.substr(5), a_error);
					if (!weather)
						return false;
					auto* locked = editor->GetLockedWeather();
					holds = editor->IsWeatherLockedByUser(weather) && sky && sky->currentWeather == weather;
					seen = fmt::format("the lock holds {:08X}, the sky shows {:08X}", locked ? locked->GetFormID() : 0, sky && sky->currentWeather ? sky->currentWeather->GetFormID() : 0);
				} else if (a_arg == "unlocked") {
					holds = !editor->IsWeatherLocked();
					seen = holds ? "no lock" : "a weather is locked";
				} else if (a_arg.starts_with("setting ")) {
					const auto line = a_arg.substr(8);
					const auto dot = line.find('.');
					const auto equals = line.find('=');
					const auto& list = Feature::GetFeatureList();
					const auto found = dot == std::string::npos || equals == std::string::npos || equals < dot ?
					                       list.end() :
					                       std::ranges::find_if(list, [&](Feature* a_feature) { return a_feature && a_feature->GetShortName() == line.substr(0, dot); });
					if (found == list.end()) {
						a_error = fmt::format("'{}' is not setting <ShortName>.<key>=<JSON> of a loaded feature", a_arg);
						return false;
					}
					json settings;
					(*found)->SaveSettings(settings);
					std::string path;
					const json* current = FindSettingByKey(settings, line.substr(dot + 1, equals - dot - 1), path);
					const auto expected = json::parse(line.substr(equals + 1), nullptr, false);
					holds = current && !expected.is_discarded() && *current == expected;
					seen = current ? fmt::format("{} is {}", path, current->dump()) : std::string("no such setting");
				} else if (a_arg == "ui-writes=0") {
					const auto missed = GameThread::MissedWrites();
					holds = missed == 0;
					seen = fmt::format("{} engine writes from the ImGui frame went around the queue", missed);
				} else {
					a_error = "cs-ui-expect takes hour=<h>, timescale=<x>, paused, running, lock=<weather>, unlocked, setting <ShortName>.<key>=<JSON> or ui-writes=0";
					return false;
				}
				logger::info("[TEST] cs-ui-expect {}: {} ({})", a_arg, holds ? "holds" : "does not hold", seen);
				if (!holds)
					a_error = fmt::format("cs-ui-expect {}: {}", a_arg, seen);
				return holds;
			});
			// "menu": <menu name>=show|hide, through the UI message queue ("Journal Menu" is the one Escape opens).
			RegisterCommand("menu", [](const std::string& a_arg, std::string& a_error) {
				const auto equals = a_arg.rfind('=');
				auto* queue = RE::UIMessageQueue::GetSingleton();
				if (equals == std::string::npos || !queue) {
					a_error = fmt::format("'{}' is not <menu name>=show|hide", a_arg);
					return false;
				}
				queue->AddMessage(RE::BSFixedString(a_arg.substr(0, equals)), a_arg.substr(equals + 1) == "show" ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
				return true;
			});
			// "lockpick": the nearest door or container within 4096 units of the player gets a novice lock, the player a lockpick, and the
			// player activates it, so the lockpicking menu opens with its 3D lock (UI3DSceneManager's scheme 7).
			RegisterCommand("lockpick", [](const std::string&, std::string&) {
				AddTask([] {
					auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton());
					auto* tes = CS_ENGINE_READ(TES, Other, RE::TES::GetSingleton());
					if (!player || !tes)
						return;
					RE::TESObjectREFR* nearest = nullptr;
					float best = 0.0f;
					tes->ForEachReferenceInRange(player, 4096.0f, [&](RE::TESObjectREFR* a_ref) {
						const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
						if (base && (base->Is(RE::FormType::Door) || base->Is(RE::FormType::Container)) && !a_ref->IsDisabled()) {
							const float distance = a_ref->GetPosition().GetSquaredDistance(player->GetPosition());
							if (!nearest || distance < best) {
								nearest = a_ref;
								best = distance;
							}
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
					auto* lock = nearest ? (nearest->GetLock() ? nearest->GetLock() : nearest->AddLock()) : nullptr;
					if (!lock) {
						logger::warn("[TEST] lockpick: no door or container near the player to lock");
						return;
					}
					lock->baseLevel = 1;
					lock->key = nullptr;
					lock->SetLocked(true);
					if (auto* pick = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000A))
						player->AddObjectToContainer(pick, nullptr, 1, nullptr);
					nearest->ActivateRef(player, 0, nullptr, 0, false);
					logger::info("[TEST] lockpick: {:08X} ({}) locked and activated", nearest->GetFormID(), nearest->GetName());
				});
				return true;
			});
			// "book": [<form id, hex>], a book opened in the book menu as reading it from the inventory does; without an id, the first
			// book that teaches nothing, is no note and has a book model (a Clutter\Books file named for a book), so it opens with two pages.
			RegisterCommand("book", [](const std::string& a_arg, std::string&) {
				static constexpr auto IsBookModel = [](const char* a_model) {
					std::string model = a_model ? a_model : "";
					std::ranges::transform(model, model.begin(), [](char a_c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(a_c))); });
					// Notes and recipes keep their models under Clutter\Books too, so the file name decides.
					const auto name = model.substr(model.find_last_of("\\/") + 1);
					return model.find("books\\") != std::string::npos && name.find("book") != std::string::npos && name.find("note") == std::string::npos;
				};
				const auto id = a_arg.empty() ? 0u : static_cast<RE::FormID>(std::strtoul(a_arg.c_str(), nullptr, 16));
				AddTask([id] {
					RE::TESObjectBOOK* book = id ? RE::TESForm::LookupByID<RE::TESObjectBOOK>(id) : nullptr;
					if (!id)
						if (auto* data = CS_ENGINE_READ(Data, Other, RE::TESDataHandler::GetSingleton()))
							for (auto* candidate : data->GetFormArray<RE::TESObjectBOOK>())
								if (candidate && !candidate->TeachesSpell() && !candidate->TeachesSkill() && !candidate->IsNoteScroll() &&
									IsBookModel(candidate->GetModel())) {
									book = candidate;
									break;
								}
					if (!book) {
						logger::warn("[TEST] book: no book {:08X}", id);
						return;
					}
					RE::BookMenu::OpenMenuFromBaseForm(book);
					logger::info("[TEST] book: {:08X} ({}) opened", book->GetFormID(), book->GetName());
				});
				return true;
			});
			// "page": next|prev, the book menu's page turn: the NextPage or PrevPage user event BookMenu::ProcessMessage turns a page for.
			RegisterCommand("page", [](const std::string& a_arg, std::string& a_error) {
				if (a_arg != "next" && a_arg != "prev") {
					a_error = fmt::format("'{}' is not next or prev", a_arg);
					return false;
				}
				AddTask([next = a_arg == "next"] {
					RE::BSUIMessageData::SendUIStringMessage(RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUserEvent, next ? "NextPage" : "PrevPage");
					logger::info("[TEST] page: {} sent to the book menu", next ? "NextPage" : "PrevPage");
				});
				return true;
			});
			// "uievent": <menu>=<user event>, a user event sent to an open menu, as its input would: MapMenu=LocalMap shows or hides the
			// local map (MapMenu::ProcessMessage, AE 0x140984a80).
			RegisterCommand("uievent", [](const std::string& a_arg, std::string& a_error) {
				const auto split = a_arg.find('=');
				if (split == std::string::npos || split == 0 || split + 1 == a_arg.size()) {
					a_error = fmt::format("'{}' is not <menu>=<user event>", a_arg);
					return false;
				}
				AddTask([menu = a_arg.substr(0, split), userEvent = a_arg.substr(split + 1)] {
					RE::BSUIMessageData::SendUIStringMessage(menu.c_str(), RE::UI_MESSAGE_TYPE::kUserEvent, userEvent.c_str());
					logger::info("[TEST] uievent: {} sent to {}", userEvent, menu);
				});
				return true;
			});
			// "key": <menu user event>, a key press queued as the keyboard's: Right, Left, Up, Down or Accept. Menus act on the user event,
			// so this moves an open menu's selection (the inventory's item list, for its 3D item). Only the press is sent.
			RegisterCommand("key", [](const std::string& a_arg, std::string& a_error) {
				static constexpr std::array<std::pair<std::string_view, std::uint32_t>, 5> kKeys{ { { "Right", 0xCD }, { "Left", 0xCB }, { "Up", 0xC8 },
					{ "Down", 0xD0 }, { "Accept", 0x1C } } };
				const auto key = std::ranges::find(kKeys, std::string_view(a_arg), &std::pair<std::string_view, std::uint32_t>::first);
				if (key == kKeys.end()) {
					a_error = fmt::format("'{}' is not Right, Left, Up, Down or Accept", a_arg);
					return false;
				}
				// Sent as "cast" sends its buttons, on the main thread. The input queue's own events are not used: its cached event
				// layout does not match this runtime (AddButtonEvent crashed in 69192).
				AddTask([userEvent = std::string(a_arg), code = key->second] {
					auto* devices = RE::BSInputDeviceManager::GetSingleton();
					if (!devices)
						return;
					// Left to the heap, as "cast" leaves its events.
					RE::InputEvent* event = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString(userEvent.c_str()), code, 1.0f, 0.0f);
					if (event)
						devices->SendEvent(&event);
				});
				return true;
			});
			// "save": <name>; "load": <name>|recent. Through the save manager: the console's save and load do nothing as a compiled script.
			RegisterCommand("save", [](const std::string& a_arg, std::string& a_error) {
				auto* manager = RE::BGSSaveLoadManager::GetSingleton();
				if (!manager || a_arg.empty()) {
					a_error = "no save manager or no name";
					return false;
				}
				manager->Save(a_arg.c_str());
				return true;
			});
			RegisterCommand("load", [](const std::string& a_arg, std::string& a_error) {
				auto* manager = RE::BGSSaveLoadManager::GetSingleton();
				if (!manager || a_arg.empty()) {
					a_error = "no save manager or no name";
					return false;
				}
				if (a_arg == "recent")
					manager->LoadMostRecentSaveGame();
				else
					manager->Load(a_arg.c_str());
				return true;
			});
			// "bink": <file>, a Bink movie from Data\Video, played at the main thread's next frame-job wait.
			RegisterCommand("bink", [](const std::string& a_arg, std::string& a_error) {
				if (a_arg.empty()) {
					a_error = "no file";
					return false;
				}
				RenderGraph::Engine::Bink::RequestPlay(a_arg);
				return true;
			});
			// "fg": off|fsr|dlss, frame generation as the Upscaling menu switches it.
			RegisterCommand("fg", [](const std::string& a_arg, std::string& a_error) {
				if (a_arg != "off" && a_arg != "fsr" && a_arg != "dlss") {
					a_error = "not off, fsr or dlss";
					return false;
				}
				Upscaling::RequestFrameGenTest(a_arg == "off" ? 0 : a_arg == "fsr" ? 1 : 2);
				return true;
			});
			// "upscale": none|taa|fsr|dlss|xess, the upscale method as the Upscaling menu sets it.
			RegisterCommand("upscale", [](const std::string& a_arg, std::string& a_error) {
				static constexpr std::array<std::string_view, 5> kMethods{ "none", "taa", "fsr", "dlss", "xess" };
				const auto found = std::ranges::find(kMethods, a_arg);
				if (found == kMethods.end()) {
					a_error = "not none, taa, fsr, dlss or xess";
					return false;
				}
				Upscaling::RequestUpscaleTest(static_cast<int>(found - kMethods.begin()));
				return true;
			});
			// "pp": <effect type>=0|1, a Post Processing effect's menu toggle.
			RegisterCommand("pp", [](const std::string& a_arg, std::string& a_error) {
				const auto separator = a_arg.rfind('=');
				const auto type = a_arg.substr(0, separator);
				bool found = false;
				for (auto& pipe : globals::features::postProcessing.pipeline)
					if (pipe && pipe->GetType() == type) {
						pipe->enabled = separator == std::string::npos || a_arg.substr(separator + 1) != "0";
						found = true;
					}
				if (!found)
					a_error = fmt::format("no Post Processing effect '{}'", type);
				return found;
			});
			// "e11": <CATEGORY>.<Key>=<value>, an Effects11 setting as its editor writes it (a bool as true|false|1|0, else a float),
			// "EFFECT.EnableBloom=false". "e11": clear-shader-cache, the menu's shader cache clear for Effects11.
			RegisterCommand("e11", [](const std::string& a_arg, std::string& a_error) {
				auto& effects11 = globals::features::effects11;
				if (!effects11.loaded) {
					a_error = "Effects11 is not loaded";
					return false;
				}
				if (a_arg == "clear-shader-cache") {
					effects11.ClearShaderCache();
					logger::info("[TEST] e11 clear-shader-cache");
					return true;
				}
				const auto dot = a_arg.find('.');
				const auto equals = a_arg.find('=');
				if (dot == std::string::npos || equals == std::string::npos || equals < dot) {
					a_error = fmt::format("'{}' is not <CATEGORY>.<Key>=<value> or clear-shader-cache", a_arg);
					return false;
				}
				const auto category = a_arg.substr(0, dot);
				const auto key = a_arg.substr(dot + 1, equals - dot - 1);
				const auto value = a_arg.substr(equals + 1);
				auto& settings = SettingManager::GetSingleton();
				const auto* info = settings.GetSettingInfo(key, category);
				if (!info) {
					a_error = fmt::format("Effects11 has no setting {}.{}", category, key);
					return false;
				}
				const auto id = info->id;
				if (info->type == SettingType::Bool) {
					if (value != "true" && value != "false" && value != "1" && value != "0") {
						a_error = fmt::format("'{}' is not true, false, 1 or 0", value);
						return false;
					}
					settings.SetValue<bool>(id, value == "true" || value == "1");
				} else if (info->type == SettingType::Float) {
					float number = 0.0f;
					const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
					if (error != std::errc{} || end != value.data() + value.size()) {
						a_error = fmt::format("'{}' is not a number", value);
						return false;
					}
					settings.SetValue<float>(id, number);
				} else {
					a_error = fmt::format("{}.{} is a time-of-day setting, which e11 does not write", category, key);
					return false;
				}
				logger::info("[TEST] e11 {}.{} = {}", category, key, value);
				return true;
			});
			// "hdr": 0|1, HDR Display's Enable HDR as its Advanced override sets it.
			RegisterCommand("hdr", [](const std::string& a_arg, std::string& a_error) {
				auto& hdr = globals::features::hdrDisplay;
				if (!hdr.loaded) {
					a_error = "HDR Display is not loaded";
					return false;
				}
				{
					std::lock_guard lock(hdr.settingsMutex);
					hdr.settings.enableHDR = a_arg == "1";
				}
				return true;
			});
			// "capture": [<seconds>], the menu's diagnostic capture. "telemetry": <label>|off, a BasicTelemetry session for this phase.
			RegisterCommand("capture", [](const std::string& a_arg, std::string&) {
				DCLF::Capture::Request(a_arg.empty() ? 10.0f : std::strtof(a_arg.c_str(), nullptr), true);
				return true;
			});
			// "graph-trace": every epoch's schedule at the next frame, into graph-trace-<frame>/ in the log folder.
			RegisterCommand("graph-trace", [](const std::string&, std::string&) {
				RenderGraph::Frame::Recipes::RequestGraphTrace();
				return true;
			});
			RegisterCommand("telemetry", [](const std::string& a_arg, std::string&) {
				DCLF::Diagnostics::TelemetryPhase(a_arg);
				return true;
			});

			// The VRAM spill lab's commands (Vram/SpillLab.h), which take their old "dclf:" lines.
			for (const char* name : { "spill", "demote", "frame-window", "ballast" })
				RegisterCommand(name, [name = std::string(name)](const std::string& a_arg, std::string& a_error) {
					if (Vram::SpillLab::Command(fmt::format("dclf:{}={}", name, a_arg)))
						return true;
					a_error = "the spill lab did not take it";
					return false;
				});

			// Waits: "shaders" (the compile queue is empty); "signal:<name>" is built in (WaitHolds).
			RegisterWait("shaders", [] { return !SIE::ShaderCache::Instance().IsCompiling(); });
		}
	}

	void RegisterCommand(std::string a_name, Command a_command)
	{
		auto& registry = Commands();
		std::scoped_lock lock(registry.mutex);
		registry.commands[std::move(a_name)] = std::move(a_command);
	}

	void RegisterCommandPrefix(std::string a_prefix, PrefixCommand a_command)
	{
		auto& registry = Commands();
		std::scoped_lock lock(registry.mutex);
		registry.prefixes.emplace_back(std::move(a_prefix), std::move(a_command));
	}

	bool HasCommand(std::string_view a_name)
	{
		RegisterGenericCommands();
		auto& registry = Commands();
		std::scoped_lock lock(registry.mutex);
		if (registry.commands.contains(a_name))
			return true;
		return std::ranges::any_of(registry.prefixes, [&](const auto& a_prefix) { return a_name.starts_with(a_prefix.first); });
	}

	void RegisterFeatureCommand(std::string a_owner, std::string a_name, Command a_command)
	{
		auto& registry = Commands();
		std::scoped_lock lock(registry.mutex);
		registry.featureCommands[a_owner + "." + a_name] = std::move(a_command);
	}

	bool RunFeatureCommand(std::string_view a_owner, std::string_view a_name, const std::string& a_arg, std::string& a_error)
	{
		auto& registry = Commands();
		const std::string key = fmt::format("{}.{}", a_owner, a_name);
		Command command;
		std::string known;
		{
			std::scoped_lock lock(registry.mutex);
			if (const auto found = registry.featureCommands.find(key); found != registry.featureCommands.end())
				command = found->second;
			else
				for (const auto& entry : registry.featureCommands)
					if (entry.first.starts_with(fmt::format("{}.", a_owner)))
						known += (known.empty() ? "" : ", ") + entry.first;
		}
		if (!command) {
			a_error = fmt::format("no test command '{}' is registered (known for {}: {})", key, a_owner, known.empty() ? "none" : known);
			return false;
		}
		return command(a_arg, a_error);
	}

	void RegisterWait(std::string a_name, std::function<bool()> a_holds)
	{
		auto& registry = Commands();
		std::scoped_lock lock(registry.mutex);
		registry.waits[std::move(a_name)] = std::move(a_holds);
	}

	void RegisterGenericCommands()
	{
		static std::once_flag once;
		std::call_once(once, RegisterGeneric);
	}

	bool RunCommand(const std::string& a_name, const std::string& a_arg, std::string& a_error)
	{
		RegisterGenericCommands();
		auto& registry = Commands();
		Command command;
		PrefixCommand prefixCommand;
		{
			std::scoped_lock lock(registry.mutex);
			if (const auto found = registry.commands.find(a_name); found != registry.commands.end())
				command = found->second;
			else
				for (const auto& [prefix, handler] : registry.prefixes)
					if (a_name.starts_with(prefix))
						prefixCommand = handler;
		}
		if (command)
			return command(a_arg, a_error);
		if (prefixCommand)
			return prefixCommand(a_name, a_arg, a_error);
		a_error = fmt::format("no command '{}' is registered{}", a_name, a_name.starts_with("dclf:") ? " (DCLF did not install?)" : "");
		return false;
	}

	bool WaitHolds(const std::string& a_condition, bool& a_known)
	{
		auto& registry = Commands();
		a_known = true;
		std::function<bool()> holds;
		{
			std::scoped_lock lock(registry.mutex);
			if (a_condition.starts_with("signal:"))
				return registry.signals.erase(a_condition.substr(7)) != 0;
			if (const auto found = registry.waits.find(a_condition); found != registry.waits.end())
				holds = found->second;
		}
		if (!holds) {
			a_known = false;
			return true;
		}
		return holds();
	}

	void TurnPlayer(float a_radians)
	{
		AddTask([a_radians] {
			if (auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton()))
				player->SetHeading(player->GetAngleZ() + a_radians);
		});
	}

	void MovePlayer(float a_units)
	{
		AddTask([a_units] {
			if (auto* player = CS_ENGINE_READ(Player, Other, RE::PlayerCharacter::GetSingleton())) {
				auto position = player->GetPosition();
				const float heading = player->GetAngleZ();
				position.x += std::sin(heading) * a_units;
				position.y += std::cos(heading) * a_units;
				player->SetPosition(position, true);
			}
		});
	}

	void TickCommands()
	{
		Cast::OnFrame();
		LoadScreenCycle::OnFrame();
		TickDrag();
		if (moving.frames) {
			--moving.frames;
			MovePlayer(moving.amount);
		}
		if (turning.frames) {
			--turning.frames;
			TurnPlayer(turning.amount * 0.017453292f);
		}
	}
}
