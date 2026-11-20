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
		thread_local Exception t_boundaryException = Exception::Count;
	}

	// The thread that opens the adoption point is the render thread the census counts.
	void NoteScopeThread();
	void NoteLateChange(std::string_view a_what, std::string_view a_caller);

	bool InAdoptionPoint()
	{
		return t_inBoundaryScope;
	}

	Scope::Scope() :
		outer(std::exchange(t_inBoundaryScope, true))
	{
		if (!outer)
			NoteScopeThread();
	}

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

	Allow::Allow(Exception a_exception) :
		outer(std::exchange(t_boundaryException, a_exception))
	{}

	Allow::~Allow()
	{
		t_boundaryException = outer;
	}

	Change::Change(std::string_view a_what, std::string_view a_caller)
	{
		if (t_boundaryChanges++ == 0)
			NoteLateChange(a_what, a_caller);
	}

	Change::~Change()
	{
		--t_boundaryChanges;
	}

	void NoteBuildRequest(std::string_view a_reason)
	{
		if (t_boundaryChanges == 0)
			NoteLateChange(a_reason, "build request");
	}
}

#if defined(CS_HAS_RENDER_GRAPH) && !defined(CS_RELEASE_PROFILE)

#	include "RenderGraph/Frame/FrameDriver.h"
#	include "Testing/Testing.h"

#	include <Resources/Resource.h>
#	include <Resources/VersionedResource.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <cstdint>
#	include <map>
#	include <mutex>
#	include <string>
#	include <thread>
#	include <vector>

namespace RenderGraph::Frame::Boundary
{
	namespace
	{
		struct BoundaryExceptionInfo
		{
			std::string_view name;
			std::string_view until;
		};

		// The one list of allowed exceptions to rule R1. Each row goes with the step that names it.
		constexpr std::array<BoundaryExceptionInfo, static_cast<std::size_t>(Exception::Count)> kBoundaryExceptions{ {
			{ "dclf", "step 10a" },
		} };

		struct BoundaryExceptionCounts
		{
			std::uint64_t versions = 0;
			std::uint64_t attaches = 0;
		};
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
			std::atomic<std::thread::id> renderThread{};
			std::mutex mutex;
			BoundaryCounts versions;  // by resource name
			BoundaryCounts firstVersions;  // a new resource's first version, by name
			BoundaryCounts versionCallers;
			BoundaryCounts changes;
			BoundaryCounts attaches;
			BoundaryCounts refusals;  // by site and reason, any thread
			std::array<BoundaryExceptionCounts, static_cast<std::size_t>(Exception::Count)> allowed{};
			std::array<BoundaryExceptionCounts, static_cast<std::size_t>(Exception::Count)> sessionAllowed{};
			std::uint64_t atBegin = 0;
			std::uint64_t sessionVersions = 0, sessionFirst = 0, sessionChanges = 0, sessionAttaches = 0, sessionRefusals = 0;
		};

		BoundaryState& GetBoundary()
		{
			static BoundaryState state;
			return state;
		}

		bool OnRenderThread(const BoundaryState& a_state)
		{
			return a_state.renderThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
		}

		std::string_view CallerTag()
		{
			return t_boundaryCaller.empty() ? std::string_view("untagged") : t_boundaryCaller;
		}

		/** @brief The open exception's window or session counts, or null outside an Allow scope. Under the state's mutex. */
		BoundaryExceptionCounts* AllowedCounts(BoundaryState& a_state, bool a_session)
		{
			const auto index = static_cast<std::size_t>(t_boundaryException);
			if (index >= kBoundaryExceptions.size())
				return nullptr;
			return a_session ? &a_state.sessionAllowed[index] : &a_state.allowed[index];
		}

		/** @brief Rule R1 broken with no exception. A tap error, fatal in development builds. Called with no census lock held. */
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
				if (auto* allowed = AllowedCounts(state, false)) {
					++allowed->versions;
					++AllowedCounts(state, true)->versions;
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
		// The statistics window may never close in a short test run, so the run's exit logs the census too.
		Testing::RegisterExitReport([] { LogStats(); });
		logger::info("[ORG] boundary: versions adopted and builds attached outside the frame's adoption point are tap errors unless a named exception "
					 "allows them; rebuild requests there are counted (rule R1)");
	}

	void NoteLateChange(std::string_view a_what, std::string_view a_caller)
	{
		auto& state = GetBoundary();
		if (t_inBoundaryScope || !OnRenderThread(state))
			return;
		const std::lock_guard lock(state.mutex);
		state.changes.Add(fmt::format("{} {}", a_caller, a_what));
		++state.sessionChanges;
	}

	void NoteAttach(std::string_view a_epoch)
	{
		auto& state = GetBoundary();
		if (t_inBoundaryScope || !OnRenderThread(state))
			return;
		const auto what = fmt::format("{} at {}", a_epoch, CallerTag());
		{
			const std::lock_guard lock(state.mutex);
			if (auto* allowed = AllowedCounts(state, false)) {
				++allowed->attaches;
				++AllowedCounts(state, true)->attaches;
				return;
			}
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
		const auto exceptions = [](const auto& a_counts) {
			std::string line;
			for (std::size_t e = 0; e < kBoundaryExceptions.size(); ++e)
				line += fmt::format("{}{} (until {}) versions {} attaches {}", line.empty() ? "" : ", ", kBoundaryExceptions[e].name, kBoundaryExceptions[e].until,
					a_counts[e].versions, a_counts[e].attaches);
			return line;
		};
		logger::info("[ORG] boundary: late versions {}{}, late changes {}, late attaches {}; new resources made late {}; allowed exceptions: {}; "
					 "adoptions at Begin {}; build refusals {}; session: late versions {}, late changes {}, late attaches {}, "
					 "new resources made late {}, allowed exceptions: {}; build refusals {}",
			part(state.versions), state.versionCallers.total ? fmt::format(" (callers: {})", state.versionCallers.Top()) : std::string(), part(state.changes),
			part(state.attaches), part(state.firstVersions), exceptions(state.allowed), state.atBegin, part(state.refusals),
			state.sessionVersions, state.sessionChanges, state.sessionAttaches, state.sessionFirst, exceptions(state.sessionAllowed), state.sessionRefusals);
		state.versions = {};
		state.firstVersions = {};
		state.versionCallers = {};
		state.changes = {};
		state.attaches = {};
		state.refusals = {};
		state.allowed = {};
		state.atBegin = 0;
	}

	void NoteScopeThread()
	{
		GetBoundary().renderThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
	}
}

#else

namespace RenderGraph::Frame::Boundary
{
	void Install() {}
	void NoteLateChange(std::string_view, std::string_view) {}
	void NoteAttach(std::string_view) {}
	void NoteRefusal(std::string_view, std::string_view) {}
	void LogStats() {}
	void NoteScopeThread() {}
}

#endif
