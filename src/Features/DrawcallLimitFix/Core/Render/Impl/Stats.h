#pragma once

// IndirectDraws::Impl's report counters. Included by Impl.h after Impl/SnapshotBuilder.h, which defines SnapshotTiming.

namespace DCLF
{
	// Report counters, each since the last report unless noted.
	struct ImplStats
	{
		std::uint64_t shadowUnbuilt = 0;  // covered frames whose publication had no shadow payload
		// Frames whose views were left to the engine because there was no shape or payload yet.
		std::uint64_t shadowUnrecorded = 0;
		std::uint64_t occlusionUnrecorded = 0;  // maps left to the engine for lack of the revision's shape
		// Atomic because the report exchanges them.
		std::atomic<std::uint64_t> snapshotsBuilt{ 0 }, snapshotsCoalesced{ 0 }, snapshotsWithoutDraws{ 0 };
		std::atomic<std::uint64_t> payloadsInstalled{ 0 };
		EventQueue<ImplSnapshotBuilder::SnapshotTiming, 256> snapshotTimings;
		std::uint64_t snapshotsAdopted = 0, snapshotFramesKept = 0;
		std::vector<std::uint32_t> adoptionLatency;
		// Frames from a growth's adoption to draws that embed its versions. ~0u for none.
		std::uint32_t growthAdoptedFrame = ~0u;
		std::vector<std::uint32_t> growthToDraws;
		// From the oldest event a publication carried to its adoption, in ms and frames.
		std::vector<double> eventAdoptionMs;
		std::vector<std::uint32_t> eventAdoptionFrames;
		std::array<SharedEpochStats, kMaxEpochs> sharedEpochs{};
		// Per main epoch, covered frames with no payload for the main resources.
		std::array<std::uint64_t, 2> unbuilt{};
		std::uint64_t frameSlotsCarried = 0;
	};
}
