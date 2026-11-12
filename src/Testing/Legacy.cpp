// The old CS_DCLF_TEST_* switches, kept while tools/cs-ab still uses them. Frames count world frames only (no loading menu, no
// load hold), so a run with DCLF off counts the same frames as one with it on.
// - CS_DCLF_TEST_COMMANDS="<frame>:<command>;...": each command line (RunCommandLine) at that frame. "dclf:wait=<condition>[@<frames>]"
//   holds the list until the condition holds; the commands after it run as many frames later as it held.
// - CS_DCLF_TEST_TOGGLE="<off>:<on>[:<off>:<on>...]": DCLF's menu toggle flipped at those frames.
// - CS_DCLF_TEST_TURN / CS_DCLF_TEST_MOVE="<start>:<end>:<degrees or units per frame>;...": the player turned or carried forward.
// - CS_DCLF_TEST_EXIT=<frame>: the run ends there (ExitRun), later by the frames the waits held.
#include "Testing/Internal.h"

#include "Feature.h"
#include "Globals.h"
#include "State.h"
#include "Utils/SwitchFile.h"

namespace Testing
{
	namespace
	{
		// A wait's limit without an '@': 3000 frames, about a minute at 50 fps.
		constexpr std::uint32_t kWaitLimitFrames = 3000;

		struct Wait
		{
			std::string condition;
			std::uint32_t since = 0;  // the frame it started at
			std::uint32_t limit = 0;  // the frames it may hold
		};

		struct Range
		{
			std::uint32_t start = 0, end = 0;
			float amount = 0.0f;
		};

		std::vector<Range> ParseRanges(const std::string& a_value)
		{
			std::vector<Range> ranges;
			std::string_view text = a_value;
			while (!text.empty()) {
				const auto end = text.find(';');
				const std::string item(text.substr(0, end));
				Range range;
				if (std::sscanf(item.c_str(), "%u:%u:%f", &range.start, &range.end, &range.amount) == 3)
					ranges.push_back(range);
				text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			}
			return ranges;
		}

		Wait ParseWait(std::string_view a_argument, std::uint32_t a_frame)
		{
			Wait wait{ std::string(a_argument), a_frame, kWaitLimitFrames };
			if (const auto at = a_argument.rfind('@'); at != std::string_view::npos) {
				wait.condition = std::string(a_argument.substr(0, at));
				const std::string frames(a_argument.substr(at + 1));
				char* end = nullptr;
				const unsigned long limit = std::strtoul(frames.c_str(), &end, 10);
				if (frames.empty() || *end != '\0' || limit == 0)
					logger::warn("[TEST] test wait '{}': '{}' is no frame count; its limit is {} frames", wait.condition, frames, kWaitLimitFrames);
				else
					wait.limit = static_cast<std::uint32_t>((std::min)(limit, static_cast<unsigned long>(UINT32_MAX)));
			}
			return wait;
		}

		bool Holds(const Wait& a_wait)
		{
			bool known = true;
			const bool holds = WaitHolds(a_wait.condition, known);
			if (!known) {
				logger::warn("[TEST] test wait: no condition '{}'; not waiting", a_wait.condition);
				return true;
			}
			return holds;
		}

		struct Legacy
		{
			struct Command
			{
				std::uint32_t frame;
				std::string line;
			};
			std::vector<Command> commands;
			std::vector<std::uint32_t> toggles;
			std::vector<Range> turns, moves;
			std::uint32_t exitFrame = 0;
			std::size_t next = 0;
			std::uint32_t frame = 0;
			std::uint32_t shift = 0;
			std::optional<Wait> wait;

			Legacy()
			{
				const auto moveValue = Util::SwitchFile::Value("CS_DCLF_TEST_MOVE");
				// Moving carries the player faster than it can survive (falls, collisions): god mode first.
				if (!moveValue.empty())
					commands.push_back({ 1, "tgm" });
				std::string_view text;
				const auto list = Util::SwitchFile::Value("CS_DCLF_TEST_COMMANDS");
				text = list;
				while (!text.empty()) {
					const auto end = text.find(';');
					const auto item = text.substr(0, end);
					if (const auto colon = item.find(':'); colon != std::string_view::npos)
						commands.push_back({ static_cast<std::uint32_t>(std::strtoul(std::string(item.substr(0, colon)).c_str(), nullptr, 10)), std::string(item.substr(colon + 1)) });
					text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
				}
				const auto toggle = Util::SwitchFile::Value("CS_DCLF_TEST_TOGGLE");
				for (std::size_t start = 0; start < toggle.size();) {
					const auto colon = toggle.find(':', start);
					if (const auto f = static_cast<std::uint32_t>(std::strtoul(toggle.substr(start, colon - start).c_str(), nullptr, 10)))
						toggles.push_back(f);
					if (colon == std::string::npos)
						break;
					start = colon + 1;
				}
				turns = ParseRanges(Util::SwitchFile::Value("CS_DCLF_TEST_TURN"));
				moves = ParseRanges(moveValue);
				exitFrame = static_cast<std::uint32_t>(std::strtoul(Util::SwitchFile::Value("CS_DCLF_TEST_EXIT").c_str(), nullptr, 10));
			}

			bool Empty() const { return commands.empty() && toggles.empty() && turns.empty() && moves.empty() && exitFrame == 0; }

			std::uint32_t HeldFrames() const { return shift + (wait ? frame - wait->since : 0u); }

			void Run(const std::string& a_line)
			{
				logger::info("[TEST] test command at frame {}: {}", frame, a_line);
				std::string error;
				if (!RunCommandLine(a_line, error))
					logger::warn("[TEST] test command '{}': {}", a_line, error);
			}

			void OnFrame()
			{
				if (!WorldFrame())
					return;
				++frame;
				if (exitFrame && frame >= exitFrame + HeldFrames())
					ExitRun();
				if (wait) {
					const bool held = Holds(*wait);
					const bool timedOut = !held && frame - wait->since >= wait->limit;
					if (!held && !timedOut)
						return;
					shift += frame - wait->since;
					if (timedOut)
						logger::warn("[TEST] test wait '{}' timed out: held frames {}-{} ({} frames, its limit); the commands after it run {} frames later", wait->condition,
							wait->since, frame, frame - wait->since, shift);
					else
						logger::info("[TEST] test wait '{}' held frames {}-{} ({} frames); the commands after it run {} frames later", wait->condition, wait->since, frame,
							frame - wait->since, shift);
					wait.reset();
				}
				while (!wait && next < commands.size() && frame >= commands[next].frame + shift) {
					const std::string& line = commands[next++].line;
					if (!line.starts_with("dclf:wait=")) {
						Run(line);
						continue;
					}
					logger::info("[TEST] test command at frame {}: {}", frame, line);
					wait = ParseWait(std::string_view(line).substr(10), frame);
					if (Holds(*wait)) {
						logger::info("[TEST] test wait '{}' held at once", wait->condition);
						wait.reset();
					}
				}
				for (std::size_t i = 0; i < toggles.size(); ++i)
					if (frame == toggles[i]) {
						const bool off = (i % 2) == 0;
						logger::info("[TEST] test toggle at frame {}: {}", frame, off ? "off" : "on");
						globals::state->SetFeatureDisabled("DrawcallLimitFix", off);
					}
				for (const auto& move : moves)
					if (frame >= move.start && frame < move.end)
						MovePlayer(move.amount);
				for (const auto& turn : turns)
					if (frame >= turn.start && frame < turn.end)
						TurnPlayer(turn.amount * 0.017453292f);
			}
		};

		std::mutex progressMutex;
		LegacyProgress progress;
	}

	void RunLegacy(std::uint32_t)
	{
		static Legacy legacy;
		if (legacy.Empty())
			return;
		legacy.OnFrame();
		std::scoped_lock lock(progressMutex);
		progress.frame = legacy.frame;
		progress.shift = legacy.shift;
		progress.next = static_cast<std::uint32_t>(legacy.next);
		progress.commands = static_cast<std::uint32_t>(legacy.commands.size());
		progress.nextFrame = legacy.next < legacy.commands.size() ? legacy.commands[legacy.next].frame + legacy.shift : 0;
		progress.nextCommand = legacy.next < legacy.commands.size() ? legacy.commands[legacy.next].line : std::string{};
		progress.waiting = legacy.wait ? legacy.wait->condition : std::string{};
	}

	LegacyProgress GetLegacyProgress()
	{
		std::scoped_lock lock(progressMutex);
		return progress;
	}
}
