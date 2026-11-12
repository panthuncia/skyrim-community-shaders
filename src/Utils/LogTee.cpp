#include "LogTee.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/dist_sink.h>

#include <ankerl/unordered_dense.h>

namespace Util::LogTee
{
	namespace
	{
		std::shared_ptr<spdlog::sinks::dist_sink_mt> tee;

		/**
		 * @brief The guard in front of the log file's sinks. A lost device once logged one line per frame until the log reached 12 GB.
		 * - The same line more than kRepeatsPerSecond times in a second: the rest of that second is counted, then logged as one line.
		 * - Past kCapBytes only warnings and errors are written; past kHardCapBytes nothing is, after a final note.
		 */
		class GuardSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			explicit GuardSink(std::vector<std::shared_ptr<spdlog::sinks::sink>> a_sinks) :
				sinks(std::move(a_sinks)) {}

		protected:
			void sink_it_(const spdlog::details::log_msg& a_message) override
			{
				if (stopped)
					return;
				const std::string_view text(a_message.payload.data(), a_message.payload.size());
				const auto second = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
				if (second != windowSecond) {
					ReportRepeats(a_message);
					windowSecond = second;
				}
				// So many distinct lines that the map grows too big: the window ends early.
				if (repeats.size() > kMaxTracked)
					ReportRepeats(a_message);
				auto& entry = repeats[std::string(text)];
				if (++entry.count > kRepeatsPerSecond) {
					entry.level = a_message.level;
					return;
				}
				Write(a_message);
			}

			void flush_() override
			{
				for (const auto& sink : sinks)
					sink->flush();
			}

			void set_pattern_(const std::string& a_pattern) override
			{
				for (const auto& sink : sinks)
					sink->set_pattern(a_pattern);
			}

			void set_formatter_(std::unique_ptr<spdlog::formatter> a_formatter) override
			{
				for (const auto& sink : sinks)
					sink->set_formatter(a_formatter->clone());
			}

		private:
			static constexpr std::uint32_t kRepeatsPerSecond = 20;
			static constexpr std::size_t kMaxTracked = 4096;
			static constexpr std::uint64_t kCapBytes = 512ull << 20;
			static constexpr std::uint64_t kHardCapBytes = kCapBytes + (64ull << 20);
			// A line's prefix (time, thread, level) as the pattern writes it, about.
			static constexpr std::uint64_t kPrefixBytes = 48;

			struct Repeat
			{
				std::uint32_t count = 0;
				spdlog::level::level_enum level = spdlog::level::info;
			};

			/** @brief One line for each line held back in the last window, then the window's counts cleared. */
			void ReportRepeats(const spdlog::details::log_msg& a_like)
			{
				for (const auto& [text, repeat] : repeats)
					if (repeat.count > kRepeatsPerSecond)
						Note(a_like, repeat.level, fmt::format("... repeated {} times: {}", repeat.count - kRepeatsPerSecond, std::string_view(text).substr(0, 200)));
				repeats.clear();
			}

			void Note(const spdlog::details::log_msg& a_like, spdlog::level::level_enum a_level, const std::string& a_text)
			{
				const spdlog::details::log_msg note(a_like.logger_name, a_level, a_text);
				Write(note);
			}

			void Write(const spdlog::details::log_msg& a_message)
			{
				if (stopped || (capped && a_message.level < spdlog::level::warn))
					return;
				written += a_message.payload.size() + kPrefixBytes;
				for (const auto& sink : sinks)
					if (sink->should_log(a_message.level))
						sink->log(a_message);
				if (!capped && written > kCapBytes) {
					capped = true;
					Note(a_message, spdlog::level::warn, "[LogGuard] the log reached 512 MB; only warnings and errors are written from here");
				} else if (written > kHardCapBytes) {
					stopped = true;
					const spdlog::details::log_msg note(a_message.logger_name, spdlog::level::err, "[LogGuard] the log reached its 576 MB cap; nothing more is written");
					for (const auto& sink : sinks) {
						sink->log(note);
						sink->flush();
					}
				}
			}

			std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks;
			ankerl::unordered_dense::map<std::string, Repeat> repeats;
			long long windowSecond = 0;
			std::uint64_t written = 0;
			bool capped = false;
			bool stopped = false;
		};
	}

	void Install()
	{
		const auto current = spdlog::default_logger();
		if (!current || tee)
			return;
		auto fanOut = std::make_shared<spdlog::sinks::dist_sink_mt>();
		// The sinks keep their own formatters (the pattern set on the logger was applied to each), so the lines read as before. They sit
		// behind the guard; sinks added later (AddSink) see every line.
		auto guard = std::make_shared<GuardSink>(current->sinks());
		fanOut->add_sink(guard);
		auto log = std::make_shared<spdlog::logger>(current->name(), fanOut);
		log->set_level(current->level());
		log->flush_on(current->flush_level());
		tee = std::move(fanOut);
		spdlog::set_default_logger(std::move(log));
	}

	void AddSink(const std::shared_ptr<spdlog::sinks::sink>& a_sink)
	{
		if (tee && a_sink)
			tee->add_sink(a_sink);
	}

	void RemoveSink(const std::shared_ptr<spdlog::sinks::sink>& a_sink)
	{
		if (tee && a_sink)
			tee->remove_sink(a_sink);
	}
}
