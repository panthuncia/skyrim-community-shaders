#include "RenderGraph/Frame/BoundaryCheck.h"

#include <cstdint>
#include <utility>

namespace RenderGraph::Frame::Boundary
{
	namespace
	{
		thread_local bool t_inBoundaryScope = false;
		thread_local std::string_view t_boundaryCaller;
		thread_local std::uint32_t t_boundaryChanges = 0;
	}

	void NoteLateChange(std::string_view a_what, std::string_view a_caller, const std::source_location& a_where);

	bool InAdoptionPoint()
	{
		return t_inBoundaryScope;
	}

	Scope::Scope() :
		outer(std::exchange(t_inBoundaryScope, true))
	{}

	Scope::~Scope()
	{
		t_inBoundaryScope = outer;
	}

	Caller::Caller(std::string_view a_tag) :
		outer(std::exchange(t_boundaryCaller, a_tag))
	{}

	Caller::~Caller()
	{
		t_boundaryCaller = outer;
	}

	Change::Change(std::string_view a_what, std::string_view a_caller, const std::source_location& a_where)
	{
		// Counted before the call, so a tap error that does not stop the game leaves the scope balanced.
		if (t_boundaryChanges++ == 0)
			NoteLateChange(a_what, a_caller, a_where);
	}

	Change::~Change()
	{
		--t_boundaryChanges;
	}

}

#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameClock.h"
#include "Testing/Testing.h"

#include <Resources/Resource.h>
#include <Resources/VersionedResource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RenderGraph::Frame::Boundary
{
	namespace
	{
		// Distinct keys kept per window. The rest are counted under one key.
		constexpr std::size_t kBoundaryKeys = 128;
		constexpr std::size_t kBoundaryTop = 6;

		struct BoundaryCounts
		{
			std::map<std::string, std::uint64_t, std::less<>> byKey;
			std::uint64_t total = 0;

			void Add(std::string_view a_key)
			{
				++total;
				if (const auto it = byKey.find(a_key); it != byKey.end()) {
					++it->second;
					return;
				}
				if (byKey.size() < kBoundaryKeys)
					byKey.emplace(std::string(a_key), 1);
				else
					++byKey["(other)"];
			}

			/** @brief Every key with its count, most first. */
			std::string All() const
			{
				std::vector<std::pair<std::string_view, std::uint64_t>> sorted(byKey.begin(), byKey.end());
				std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
				std::string line;
				for (const auto& [key, count] : sorted)
					line += fmt::format("{}{} x {}", line.empty() ? "" : ", ", key, count);
				return line.empty() ? std::string("none") : line;
			}

			std::string Top() const
			{
				std::vector<std::pair<std::string_view, std::uint64_t>> sorted(byKey.begin(), byKey.end());
				std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
				std::string line;
				for (std::size_t i = 0; i < sorted.size() && i < kBoundaryTop; ++i)
					line += fmt::format("{}{} x {}", line.empty() ? "" : ", ", sorted[i].first, sorted[i].second);
				if (sorted.size() > kBoundaryTop)
					line += fmt::format(", {} more", sorted.size() - kBoundaryTop);
				return line;
			}
		};

		struct BoundaryState
		{
			std::mutex mutex;
			BoundaryCounts versions;  // by resource name
			BoundaryCounts firstVersions;  // a new resource's first version, by name
			BoundaryCounts versionCallers;
			BoundaryCounts changes;
			BoundaryCounts attaches;
			BoundaryCounts refusals;  // by site and reason, any thread
			BoundaryCounts sessionChangeSites;  // late changes in the session, by site: the sources to move before they can be fatal
			std::uint64_t atBegin = 0;
			std::uint64_t sessionVersions = 0, sessionFirst = 0, sessionChanges = 0, sessionAttaches = 0, sessionRefusals = 0;
		};

		BoundaryState& GetBoundary()
		{
			static BoundaryState state;
			return state;
		}

		/**
		* @brief The census counts the render owner, the thread that began the frame (FrameAnchors::IsRenderOwner), once a frame has
		* begun. Setup registers before the first Begin, which is no frame.
		*/
		bool OnRenderThread(const BoundaryState&)
		{
			return FrameClock::BeginFrame() > 0 && FrameAnchors::IsRenderOwner();
		}

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

		std::string_view CallerTag()
		{
			return t_boundaryCaller.empty() ? std::string_view("untagged") : t_boundaryCaller;
		}

		/** @brief Rule R1 broken. A tap error, fatal in development builds. Called with no census lock held. */
		void ReportOutsideAdoption(std::string_view a_what) noexcept
		{
			try {
				Driver::NoteTapError(Driver::TapError::OutsideAdoption, SegmentId::Count, a_what);
			} catch (...) {
				// The census line still counts it.
			}
		}

		void OnAdoption(const org::VersionedResource&, const org::ResourceVersion& a_version) noexcept
		{
			auto& state = GetBoundary();
			if (!OnRenderThread(state))
				return;
			std::string error;
			try {
				const std::lock_guard lock(state.mutex);
				if (t_inBoundaryScope) {
					++state.atBegin;
					return;
				}
				const auto& name = a_version.resource ? a_version.resource->GetName() : std::string();
				const auto key = name.empty() ? std::string_view("(unnamed)") : std::string_view(name);
				// A new resource made inside the frame replaces nothing a recording bound, so it is counted apart.
				if (a_version.number == 1) {
					state.firstVersions.Add(key);
					++state.sessionFirst;
					return;
				}
				const auto caller = CallerTag();
				state.versions.Add(key);
				state.versionCallers.Add(caller);
				++state.sessionVersions;
				error = fmt::format("{} took version {} outside the frame's adoption point (caller {})", key, a_version.number, caller);
			} catch (...) {
				// A census that cannot allocate counts nothing.
				return;
			}
			ReportOutsideAdoption(error);
		}
	}

	void Install()
	{
		org::VersionedResource::SetAdoptionObserver(&OnAdoption);
		// The test run's exit logs the census.
		Testing::RegisterExitReport([] { LogStats(); });
		logger::info("[ORG] boundary: versions adopted, builds attached, clients registered and builds requested inside the frame, outside its "
					 "adoption point, are tap errors (rule R1)");
	}

	void NoteLateChange(std::string_view a_what, std::string_view a_caller, const std::source_location& a_where)
	{
		auto& state = GetBoundary();
		if (t_inBoundaryScope || !OnRenderThread(state))
			return;
		std::string key;
		try {
			const std::lock_guard lock(state.mutex);
			key = fmt::format("{} {} at {}", a_caller, a_what, SiteOf(a_where));
			state.changes.Add(key);
			state.sessionChangeSites.Add(key);
			++state.sessionChanges;
		} catch (...) {
			// A census that cannot allocate counts nothing.
			return;
		}
		// Rule R1: registrations and structural build requests are made only inside the adoption point (plan step 10c's follow-up).
		try {
			Driver::NoteTapError(Driver::TapError::LateChange, SegmentId::Count,
				fmt::format("{} came inside the frame, outside its adoption point; registrations and structural build requests are made only there", key), a_where);
		} catch (...) {
			// The census line still counts it.
		}
	}

	void NoteAttach(std::string_view a_epoch)
	{
		auto& state = GetBoundary();
		if (t_inBoundaryScope || !OnRenderThread(state))
			return;
		const auto what = fmt::format("{} at {}", a_epoch, CallerTag());
		{
			const std::lock_guard lock(state.mutex);
			state.attaches.Add(what);
			++state.sessionAttaches;
		}
		ReportOutsideAdoption(fmt::format("a build of {} attached outside the frame's adoption point", what));
	}

	void NoteRefusal(std::string_view a_site, std::string_view a_reason)
	{
		auto& state = GetBoundary();
		const std::lock_guard lock(state.mutex);
		state.refusals.Add(fmt::format("{}: {}", a_site, a_reason));
		++state.sessionRefusals;
	}

	void LogStats()
	{
		auto& state = GetBoundary();
		const std::lock_guard lock(state.mutex);
		const auto part = [](const BoundaryCounts& a_counts) {
			return a_counts.total ? fmt::format("{} (top: {})", a_counts.total, a_counts.Top()) : std::string("0");
		};
		logger::info("[ORG] boundary: late versions {}{}, late changes {}, late attaches {}; new resources made late {}; "
					 "adoptions at Begin {}; build refusals {}; session: late versions {}, late changes {}, late attaches {}, "
					 "new resources made late {}; build refusals {}",
			part(state.versions), state.versionCallers.total ? fmt::format(" (callers: {})", state.versionCallers.Top()) : std::string(), part(state.changes),
			part(state.attaches), part(state.firstVersions), state.atBegin, part(state.refusals),
			state.sessionVersions, state.sessionChanges, state.sessionAttaches, state.sessionFirst, state.sessionRefusals);
		// Every site, so a run's last line lists every late change with its source line.
		if (state.sessionChangeSites.total)
			logger::info("[ORG] boundary: session late changes by site: {}", state.sessionChangeSites.All());
		state.versions = {};
		state.firstVersions = {};
		state.versionCallers = {};
		state.changes = {};
		state.attaches = {};
		state.refusals = {};
		state.atBegin = 0;
	}
}

