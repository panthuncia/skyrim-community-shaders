#pragma once

// Dense readback ranges by run. Features include this header directly, so it must stay self-contained.

#include <Render/RunRef.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace DCLF::Draws
{
	/** @brief Dense ranges of a host-side readback, one per stable run's lease index. A commit reads what its range's last run left,
	 * which is done because the lease retired. Leased runs get no range, since their lease index changes every run. Any thread. */
	class RunRanges
	{
	public:
		static constexpr std::uint32_t kNone = UINT32_MAX;
		explicit RunRanges(std::uint32_t a_ranges) :
			ranges(a_ranges) {}
		std::uint32_t Count() const { return ranges; }
		/** @brief a_run's range, assigned on first use. kNone for a leased run or when every range is taken. */
		std::uint32_t Of(const org::RunRef& a_run) const
		{
			if (!a_run.Stable())
				return kNone;
			std::scoped_lock lock(mutex);
			if (of.size() <= a_run.lease)
				of.resize(std::size_t(a_run.lease) + 1, kNone);
			auto& range = of[a_run.lease];
			if (range == kNone && given < ranges)
				range = given++;
			return range;
		}

	private:
		std::uint32_t ranges = 0;
		mutable std::mutex mutex;
		mutable std::vector<std::uint32_t> of;  // by lease index
		mutable std::uint32_t given = 0;
	};

	/** @brief Enough ranges for every published copy of the epochs that record a DCLF readback. */
	inline constexpr std::uint32_t kReadbackRanges = 16;

}
