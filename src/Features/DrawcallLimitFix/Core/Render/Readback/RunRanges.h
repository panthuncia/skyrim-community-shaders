#pragma once

// Dense readback ranges by run (RunRanges): what a host-side readback indexes its buffers by. Self-contained (features include it).

#include <Render/RunRef.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief Dense ranges of a host-side readback, one per run lease index that a stable run uses (org::RunRef::Stable: a
	 * revision-driven epoch's published copies, which keep their lease indices). A readback pass records into its run's range and the
	 * run's commit reads what the range's last run left there: the lease retired before it was opened again, so that run is done. A
	 * leased run (a live epoch's, an external run's) gets no range: its lease index changes from run to run, so no later commit would
	 * read what it wrote. Past Count() ranges, a run gets none either. Any thread.
	 */
	class RunRanges
	{
	public:
		static constexpr std::uint32_t kNone = UINT32_MAX;
		explicit RunRanges(std::uint32_t a_ranges) :
			ranges(a_ranges) {}
		std::uint32_t Count() const { return ranges; }
		/** @brief a_run's range, given on its lease index's first use; kNone for a leased run, or when every range is given. */
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

	/** @brief The ranges a DCLF readback keeps: room for every published copy of the epochs that record it. */
	inline constexpr std::uint32_t kReadbackRanges = 16;

}
