// The command and wait registries, and the generic commands, which work with every feature off. A step names a command by "do" and
// passes "arg". Features register their own (DCLF's under "dclf:").
#include "Testing/Internal.h"

#include "CSEditor/EditorWindow.h"
#include "Feature.h"
#include "Features/HDRDisplay.h"
#include "Features/PostProcessing.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Menu.h"
#include "RenderGraph/Engine/Bink/Bink.h"
#include "ShaderCache.h"
#include "State.h"
#include "Vram/MemProbe.h"
#include "Vram/SpillLab.h"
#include "Vram/VramLedger.h"

#if defined(CS_HAS_DCLF)
#	include "Features/DrawcallLimitFix/Diagnostics/Capture/Capture.h"
#	include "Features/DrawcallLimitFix/Diagnostics/TelemetryProfile.h"
#endif

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

		/** @brief The a_index-th nearest actor (by distance to the player) that a_filter accepts, or null. Main thread. */
		template <class Filter>
		std::pair<RE::Actor*, float> NearestActor(std::size_t a_index, Filter&& a_filter)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* lists = RE::ProcessLists::GetSingleton();
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

		void AddTask(std::function<void()> a_task)
		{
			if (auto* tasks = SKSE::GetTaskInterface())
				tasks->AddTask(std::move(a_task));
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
						auto* player = RE::PlayerCharacter::GetSingleton();
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

		bool Shot(const std::string& a_arg, std::string& a_error)
		{
			if (a_arg.empty()) {
				a_error = "a shot needs a name";
				return false;
			}
			auto directory = RunDirectory();
			directory = directory.empty() ? logger::log_directory().value_or(std::filesystem::path{}) / "dclf-shots" : directory / "shots";
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			globals::features::screenshotFeature.RequestCapture(directory / (a_arg + ".png"));
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
			unsigned width = 0, height = 0;
			bool done = false;
			if (a_arg == "minimize" || a_arg == "restore") {
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
				a_error = fmt::format("'{}' is not minimize, restore, background, foreground or <W>x<H>", a_arg);
				return false;
			}
			logger::info("[TEST] window {}: {}", a_arg, done ? "sent" : "failed");
			return true;
		}

		bool Weather(const std::string& a_arg, std::string& a_error)
		{
			auto* editor = EditorWindow::GetSingleton();
			if (a_arg == "off") {
				editor->UnlockWeather();
				return true;
			}
			// By editor id first, then by hex form id.
			RE::TESWeather* weather = nullptr;
			std::string known;
			if (auto* data = RE::TESDataHandler::GetSingleton()) {
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
			if (!weather) {
				a_error = fmt::format("no weather '{}'. Loaded weathers: {}", a_arg, known);
				return false;
			}
			logger::info("[TEST] weather {:08X} {}", weather->GetFormID(), weather->GetFormEditorID() ? weather->GetFormEditorID() : "?");
			// The CS editor's lock holds it against scripts and climate timers; the console's fw depends on frame timing.
			editor->LockWeatherForUser(weather);
			return true;
		}

		bool SetFeature(const std::string& a_arg, std::string& a_error)
		{
			// "<ShortName>=0|1"; the old "dclf:feature=0|1" names DCLF.
			const auto equals = a_arg.rfind('=');
			const std::string name = equals == std::string::npos ? std::string("DrawcallLimitFix") : a_arg.substr(0, equals);
			const std::string value = equals == std::string::npos ? a_arg : a_arg.substr(equals + 1);
			const auto& list = Feature::GetFeatureList();
			if (std::ranges::none_of(list, [&](Feature* a_feature) { return a_feature && a_feature->GetShortName() == name; })) {
				a_error = fmt::format("no feature '{}'", name);
				return false;
			}
			globals::state->SetFeatureDisabled(name, value == "0");
			return true;
		}

		void RegisterGeneric()
		{
			RegisterCommand("console", ConsoleCommand);
			RegisterCommand("exit", [](const std::string&, std::string&) {
				ExitRun();
				return true;
			});
			RegisterCommand("shot", Shot);
			RegisterCommand("window", Window);
			RegisterCommand("weather", Weather);
			RegisterCommand("feature", SetFeature);
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
					if (auto* camera = RE::PlayerCamera::GetSingleton())
						first ? camera->ForceFirstPerson() : camera->ForceThirdPerson();
				});
				return true;
			});
			// "draw": 1|0, the player's weapons or magic hands drawn or sheathed.
			RegisterCommand("draw", [](const std::string& a_arg, std::string&) {
				AddTask([draw = a_arg != "0"] {
					if (auto* player = RE::PlayerCharacter::GetSingleton())
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
					auto* player = RE::PlayerCharacter::GetSingleton();
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
					auto* player = RE::PlayerCharacter::GetSingleton();
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
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto* actor = NearestActor(index, [&](RE::Actor& a_actor) { return player && a_actor.GetParentCell() == player->GetParentCell(); }).first;
					if (actor) {
						actor->SetAlpha(alpha);
						logger::info("[TEST] npc-alpha: {} ({:08X}) set to {}", actor->GetDisplayFullName(), actor->GetFormID(), alpha);
					}
				});
				return true;
			});
			// "cs-menu": open|close, Community Shaders' own menu.
			RegisterCommand("cs-menu", [](const std::string& a_arg, std::string&) {
				if (a_arg == "open")
					Menu::GetSingleton()->Open();
				else
					Menu::GetSingleton()->Close();
				return true;
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
			// "hdr": 0|1, HDR Display's Enable HDR as its Advanced override sets it.
			RegisterCommand("hdr", [](const std::string& a_arg, std::string& a_error) {
				auto& hdr = globals::features::hdrDisplay;
				if (!hdr.loaded) {
					a_error = "HDR Display is not loaded";
					return false;
				}
				std::lock_guard lock(hdr.settingsMutex);
				hdr.settings.enableHDR = a_arg == "1";
				return true;
			});
#if defined(CS_HAS_DCLF)
			// "capture": [<seconds>], the menu's diagnostic capture. "telemetry": <label>|off, a BasicTelemetry session for this phase.
			RegisterCommand("capture", [](const std::string& a_arg, std::string&) {
				DCLF::Capture::Request(a_arg.empty() ? 10.0f : std::strtof(a_arg.c_str(), nullptr), true);
				return true;
			});
			RegisterCommand("telemetry", [](const std::string& a_arg, std::string&) {
				DCLF::Diagnostics::TelemetryPhase(a_arg);
				return true;
			});
#endif

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
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				player->SetHeading(player->GetAngleZ() + a_radians);
		});
	}

	void MovePlayer(float a_units)
	{
		AddTask([a_units] {
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
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
