#pragma once

#include "Utils/Hash.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Util
{
	/** @brief Thread-safe first-use flag that can be re-armed. */
	class OnceFlag
	{
	public:
		/** @brief True the first time after construction or Reset(). */
		bool First() noexcept
		{
			return !fired.exchange(true, std::memory_order_relaxed);
		}

		/** @brief Re-arms the flag. */
		void Reset() noexcept
		{
			fired.store(false, std::memory_order_relaxed);
		}

	private:
		std::atomic<bool> fired{ false };
	};

	/** @brief Thread-safe first-use tracking for hashed key combinations. */
	class OnceSet
	{
	public:
		/** @brief True the first time a key combination is seen. */
		template <class... Keys>
		bool First(const Keys&... keys)
		{
			std::size_t signature = 0;
			(CombineHash(signature, std::hash<Keys>{}(keys)), ...);
			std::scoped_lock lock(mutex);
			return seen.insert(signature).second;
		}

	private:
		std::mutex mutex;
		std::unordered_set<std::size_t> seen;
	};

	/** @brief Thread-safe gate: first call, then once per interval frames. */
	class FrameThrottle
	{
	public:
		explicit constexpr FrameThrottle(std::uint32_t intervalFrames) noexcept :
			interval(intervalFrames) {}

		/** @brief True on the first call or past the interval since last true. */
		bool Ready(std::uint32_t frame) noexcept
		{
			auto state = last.load(std::memory_order_relaxed);
			if ((state & kFired) && frame - static_cast<std::uint32_t>(state) <= interval)
				return false;
			return last.compare_exchange_strong(state, kFired | frame, std::memory_order_relaxed);
		}

	private:
		static constexpr std::uint64_t kFired = 1ull << 32;

		std::uint32_t interval;
		std::atomic<std::uint64_t> last{ 0 };
	};

	/**
	 * @brief Logs each distinct reason once under one prefix, and counts every report of it.
	 *
	 * For a fallback that can happen every frame ("the engine draws this batch: <reason>"): the log gets the first time of each
	 * reason, Count has the rest. Keep reasons a small fixed set of texts; put changing values (sizes, ids) in a_detail, which only
	 * the first line shows. Thread-safe.
	 *
	 * Usage: `static Util::ReasonLog s_fallbacks("[ORG] Bink: the engine draws");` then `s_fallbacks.Report("no program yet");`.
	 */
	class ReasonLog
	{
	public:
		explicit ReasonLog(std::string a_prefix, spdlog::level::level_enum a_level = spdlog::level::info) :
			prefix(std::move(a_prefix)), level(a_level) {}

		/** @brief Counts a_reason. The first time, logs "<prefix>: <reason>" (and ", <detail>" when given). True when it logged. */
		bool Report(std::string_view a_reason, std::string_view a_detail = {})
		{
			{
				std::scoped_lock lock(mutex);
				auto found = counts.find(a_reason);
				if (found != counts.end()) {
					++found->second;
					return false;
				}
				counts.emplace(std::string(a_reason), 1);
			}
			if (a_detail.empty())
				spdlog::log(level, "{}: {}", prefix, a_reason);
			else
				spdlog::log(level, "{}: {}, {}", prefix, a_reason, a_detail);
			return true;
		}

		/** @brief How many times a_reason was reported. */
		std::uint64_t Count(std::string_view a_reason) const
		{
			std::scoped_lock lock(mutex);
			const auto found = counts.find(a_reason);
			return found != counts.end() ? found->second : 0;
		}

	private:
		std::string prefix;
		spdlog::level::level_enum level;
		mutable std::mutex mutex;
		std::map<std::string, std::uint64_t, std::less<>> counts;
	};
}
