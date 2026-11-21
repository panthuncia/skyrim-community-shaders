#include "RenderGraph/Frame/EpochRecordings.h"

#include <atomic>
#include <exception>
#include <stdexcept>
#include <utility>

namespace RenderGraph::Frame::EpochRecordings
{
	namespace
	{
		std::atomic<std::uint64_t> epochRecordingsRequested{ 0 };
		std::atomic<std::uint64_t> epochRecordingsReady{ 0 };
		std::atomic<std::uint64_t> epochRecordingsDropped{ 0 };
	}

	bool Request(org::GraphHost& a_host, std::uint32_t a_epoch, std::shared_ptr<const org::IHostExecutionData> a_hostData,
		std::uint64_t a_buildGeneration, std::uint32_t a_copy, std::string a_what, Done a_done)
	{
		const bool posted = a_host.RequestEpochRecording(
			a_epoch, std::move(a_hostData),
			[what = std::move(a_what), done = std::move(a_done), a_buildGeneration](std::shared_ptr<const org::GraphHost::EpochRecording> a_recording,
				std::exception_ptr a_error) {
				Result result;
				if (a_error) {
					try {
						std::rethrow_exception(a_error);
					} catch (const org::GraphHost::EpochRecordingDropped& e) {
						result.outcome = Outcome::Dropped;
						result.error = e.what();
					} catch (const std::exception& e) {
						RenderGraph::Fatal(fmt::format("the recording of {}", what), e.what());
					} catch (...) {
						RenderGraph::Fatal(fmt::format("the recording of {}", what), "an unknown error");
					}
				} else if (!a_recording) {
					RenderGraph::Fatal(fmt::format("the recording of {}", what), "the host's thread completed the request with neither a recording nor an error");
				} else if (a_buildGeneration != 0 && a_recording->buildGeneration != a_buildGeneration) {
					result.outcome = Outcome::Dropped;
					result.error = fmt::format("recorded on graph build {}, not build {}", a_recording->buildGeneration, a_buildGeneration);
				} else {
					result.outcome = Outcome::Ready;
					result.recording = std::move(a_recording);
				}
				(result.outcome == Outcome::Ready ? epochRecordingsReady : epochRecordingsDropped).fetch_add(1, std::memory_order_relaxed);
				done(std::move(result));
			},
			a_copy, a_buildGeneration);
		if (posted)
			epochRecordingsRequested.fetch_add(1, std::memory_order_relaxed);
		return posted;
	}

	Stats Read()
	{
		return { epochRecordingsRequested.load(std::memory_order_relaxed), epochRecordingsReady.load(std::memory_order_relaxed),
			epochRecordingsDropped.load(std::memory_order_relaxed) };
	}
}

