#pragma once

#include <cstdint>

#include "RenderGraph/Fatal.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RunRef.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/**
* @brief The one service that asks ORG's host thread for revision-driven epoch recordings (GraphHost::RequestEpochRecording).
* Every caller gets the same outcome rules: a drop is reported, any other error stops the game.
* Cache keeps a caller's recordings by what they were recorded for, with the same keep rules for every caller.
*/
namespace RenderGraph::Frame::EpochRecordings
{
	enum class Outcome : std::uint8_t
	{
		Ready,
		Dropped,
		Failed
	};

	struct Result
	{
		Outcome outcome = Outcome::Failed;
		std::shared_ptr<const org::GraphHost::EpochRecording> recording;  // Ready only
		std::string error;                                                // Dropped/Failed: why
	};

	// Called once, on the host's thread (or inline).
	using Done = std::function<void(Result)>;

	/**
	* @brief Any thread. Posts one recording of a_epoch for a_hostData on build a_buildGeneration (0: the attached build),
	* copy a_copy (org::RunRef::kNoCopy: every copy).
	* a_what names the request in logs and in the fatal message ("the Sky stage (slot 1)").
	* Outcome rules, the same for every caller:
	* - a recording whose buildGeneration is not a_buildGeneration (when a_buildGeneration != 0), or EpochRecordingDropped:
	*   Dropped. The caller asks again.
	* - any other error, or neither recording nor error: fatal. a_done is not called then.
	* Returns false when the host takes no requests (async epochs off). a_done is not called then.
	*/
	bool Request(org::GraphHost& a_host, std::uint32_t a_epoch, std::shared_ptr<const org::IHostExecutionData> a_hostData,
		std::uint64_t a_buildGeneration, std::uint32_t a_copy, std::string a_what, Done a_done);

	/** @brief An epoch's builds a cache keeps recordings of. 0: none. */
	struct Builds
	{
		std::uint64_t attached = 0;
		std::uint64_t staged = 0;
		/** @brief Whether a recording on build a_build is of one of them. */
		bool Has(std::uint64_t a_build) const { return a_build != 0 && (a_build == attached || a_build == staged); }
	};

	/** @brief What a cache does with an entry that a graph build dropped. */
	enum class AfterDrop : std::uint8_t
	{
		AskAgain,        // the next lookup of its key misses, so the caller asks again
		KeepUntilNewKey  // the lookup finds it dropped. The caller asks again only for a new key
	};

	/**
	* @brief One caller's kept epoch recordings, keyed by what they were recorded for. Key needs operator==; the caller hashes it.
	* An entry is one or more requests (variants) that settle together. The owner thread finds, adds and erases entries.
	* The host's thread completes them under the cache's lock. The rules, the same for every caller:
	* - a kept entry that settled with no recording and no drop stops the game;
	* - a dropped entry follows Rule;
	* - adding an entry drops the settled ones recorded on a build the caller no longer keeps (Builds). They hold that build's
	*   graph and alias pools;
	* - the least recently used settled entry goes when the cache holds Keep entries.
	* Payload is the caller's own data per entry, such as the fragment its recordings resolve.
	*/
	template <class Key, std::size_t Keep, AfterDrop Rule, class Payload = std::monostate>
	class Cache
	{
	public:
		using Recording = std::shared_ptr<const org::GraphHost::EpochRecording>;

		/** @brief An entry's state. */
		struct Status
		{
			bool settled = false;               // every request is recorded, dropped or refused
			bool dropped = false;               // a graph build dropped a request. Not a failure
			std::vector<Recording> recordings;  // by request. Kept only when every request recorded
			std::string error;                  // why it was dropped or refused
			/** @brief Whether every request recorded. */
			bool Ready() const { return settled && !dropped && error.empty(); }
		};

		struct Entry
		{
			Key key;
			std::uint64_t hash = 0;
			std::uint64_t lastUse = 0;  // owner thread: the caller's clock at the last lookup
			Payload payload{};          // owner thread: the caller's own data, set after Add
			// Under the cache's lock.
			Status status;
			std::size_t remaining = 0;
		};
		using EntryPtr = std::shared_ptr<Entry>;
		// Called once per entry, on the thread that settled it, outside the lock.
		using Settled = std::function<void(const Status&)>;

		/**
		* @brief Owner thread. The kept entry for a_key, or null. a_unusable(const Status&) drops a ready entry the caller cannot
		* use any more, so the lookup misses. a_describe() names the entry in the fatal message when a kept entry failed.
		*/
		template <class Unusable, class Describe>
		EntryPtr Find(std::uint64_t a_hash, const Key& a_key, std::uint64_t a_use, Unusable&& a_unusable, Describe&& a_describe)
		{
			const std::lock_guard lock(*m_mutex);
			for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
				auto& kept = **it;
				if (kept.hash != a_hash || !(kept.key == a_key))
					continue;
				if (kept.status.settled && !kept.status.Ready()) {
					if (!kept.status.dropped)
						RenderGraph::Fatal(fmt::format("the recording of {}", a_describe()), kept.status.error.empty() ? std::string_view("no recording") : std::string_view(kept.status.error));
					if constexpr (Rule == AfterDrop::AskAgain) {
						m_entries.erase(it);
						return nullptr;
					}
				} else if (kept.status.Ready() && a_unusable(std::as_const(kept.status))) {
					m_entries.erase(it);
					return nullptr;
				}
				kept.lastUse = a_use;
				return *it;
			}
			return nullptr;
		}

		/**
		* @brief Owner thread. A new entry for a_key with a_requests requests to post. It replaces nothing, so call Find first.
		* First drops the settled entries recorded on builds not in a_builds, then the least recently used.
		*/
		EntryPtr Add(const Builds& a_builds, std::uint64_t a_hash, Key a_key, std::uint64_t a_use, std::size_t a_requests = 1)
		{
			auto entry = std::make_shared<Entry>();
			entry->key = std::move(a_key);
			entry->hash = a_hash;
			entry->lastUse = a_use;
			entry->status.recordings.resize(a_requests);
			entry->remaining = a_requests;
			const std::lock_guard lock(*m_mutex);
			std::erase_if(m_entries, [&](const EntryPtr& a_kept) {
				return a_kept->status.Ready() && std::ranges::any_of(a_kept->status.recordings, [&](const Recording& a_recording) {
					return a_recording && !a_builds.Has(a_recording->buildGeneration);
				});
			});
			while (m_entries.size() >= Keep) {
				auto oldest = m_entries.end();
				for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
					if ((*it)->status.settled && (oldest == m_entries.end() || (*it)->lastUse < (*oldest)->lastUse))
						oldest = it;
				if (oldest == m_entries.end())
					break;
				m_entries.erase(oldest);
			}
			m_entries.push_back(entry);
			return entry;
		}

		/**
		* @brief Any thread. Posts a_entry's requests through Request, one per host data, on build a_buildGeneration and copy a_copy.
		* a_what names them. With more than one request, each gets " (request i)". a_settled runs once, when the last one settles.
		* A refused post (async epochs off) settles the requests not posted with an error and returns false.
		*/
		bool Post(org::GraphHost& a_host, const EntryPtr& a_entry, std::uint32_t a_epoch, std::vector<std::shared_ptr<const org::IHostExecutionData>> a_hostData,
			std::uint64_t a_buildGeneration, std::uint32_t a_copy, std::string_view a_what, Settled a_settled = {})
		{
			const std::size_t count = a_hostData.size();
			auto settled = std::make_shared<const Settled>(std::move(a_settled));
			for (std::size_t i = 0; i < count; ++i) {
				auto what = count > 1 ? fmt::format("{} (request {})", a_what, i) : std::string(a_what);
				const bool posted = Request(a_host, a_epoch, std::move(a_hostData[i]), a_buildGeneration, a_copy, std::move(what),
					[mutex = m_mutex, entry = a_entry, settled, i](Result a_result) { Complete(*mutex, *entry, *settled, i, std::move(a_result), 1); });
				if (!posted) {
					Result refused;
					refused.outcome = Outcome::Failed;
					refused.error = "the host takes no recording requests (async epochs are off)";
					Complete(*m_mutex, *a_entry, *settled, i, std::move(refused), count - i);
					return false;
				}
			}
			return true;
		}

		/** @brief Any thread. A copy of a_entry's state. */
		Status Read(const Entry& a_entry) const
		{
			const std::lock_guard lock(*m_mutex);
			return a_entry.status;
		}

		/** @brief Any thread. Whether every request of a_entry settled. */
		bool IsSettled(const Entry& a_entry) const
		{
			const std::lock_guard lock(*m_mutex);
			return a_entry.status.settled;
		}

		/** @brief Any thread. a_entry's recording a_index once every request recorded, else null. */
		Recording RecordingOf(const Entry& a_entry, std::size_t a_index = 0) const
		{
			const std::lock_guard lock(*m_mutex);
			return a_entry.status.Ready() && a_index < a_entry.status.recordings.size() ? a_entry.status.recordings[a_index] : nullptr;
		}

		/** @brief Owner thread. Whether a kept entry matches a_pred(const Entry&). a_pred runs under the lock. */
		template <class Pred>
		bool AnyOf(Pred&& a_pred) const
		{
			const std::lock_guard lock(*m_mutex);
			return std::ranges::any_of(m_entries, [&](const EntryPtr& a_entry) { return a_pred(std::as_const(*a_entry)); });
		}

		/** @brief Owner thread. Erases the kept entries a_pred(const Entry&) matches. a_pred runs under the lock. Returns how many. */
		template <class Pred>
		std::size_t EraseIf(Pred&& a_pred)
		{
			const std::lock_guard lock(*m_mutex);
			return static_cast<std::size_t>(std::erase_if(m_entries, [&](const EntryPtr& a_entry) { return a_pred(std::as_const(*a_entry)); }));
		}

	private:
		/** @brief a_count requests from a_index settled with a_result. The entry settles with the last one. */
		static void Complete(std::mutex& a_mutex, Entry& a_entry, const Settled& a_settled, std::size_t a_index, Result a_result, std::size_t a_count)
		{
			Status done;
			{
				const std::lock_guard lock(a_mutex);
				auto& status = a_entry.status;
				if (a_result.outcome == Outcome::Ready) {
					status.recordings[a_index] = std::move(a_result.recording);
				} else {
					status.dropped = status.dropped || a_result.outcome == Outcome::Dropped;
					if (status.error.empty())
						status.error = std::move(a_result.error);
				}
				a_entry.remaining -= (std::min)(a_count, a_entry.remaining);
				if (a_entry.remaining != 0)
					return;
				status.settled = true;
				// A set of recordings is used whole or not at all.
				if (!status.Ready())
					status.recordings.clear();
				if (!a_settled)
					return;
				done = status;
			}
			a_settled(done);
		}

		std::shared_ptr<std::mutex> m_mutex = std::make_shared<std::mutex>();
		std::vector<EntryPtr> m_entries;
	};

	/** @brief Counters for the exit report. */
	struct Stats
	{
		std::uint64_t requested = 0;
		std::uint64_t ready = 0;
		std::uint64_t dropped = 0;
	};

	/** @brief Any thread. The counters so far. All zero without the render graph. */
	Stats Read();
}
