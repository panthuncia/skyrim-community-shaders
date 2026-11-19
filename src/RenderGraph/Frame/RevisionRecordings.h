#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org
{
	struct IResourceVersions;
}

/**
* @brief The CS stages' recordings for one frame revision (frame revision plan, steps 4c and 4d). Implemented in FrameAnchors.cpp.
* A revision names pending texture versions. Each member stage records every frame slot that has a schedule against them, through
* the stage's own recording cache (RequestDeclared), and adoption installs them all in one frame. Render owner, at a frame's Begin.
*/
namespace FrameAnchors
{
	/** @brief One revision's member stages and their recordings. Opaque outside FrameAnchors.cpp. */
	struct RevisionRecordings;

	/** @brief Where a revision's recordings stand after AdvanceRevisionRecordings. */
	struct RevisionProgress
	{
		bool ready = false;              // every member slot with a schedule has a current recording of that schedule's description
		std::uint32_t recordings = 0;    // the recordings adoption installs
		std::uint32_t askedAgain = 0;    // dropped, or recorded on a build replaced since, and asked again by this call
		std::uint32_t missing = 0;       // a member stage still missing a recording (a SegmentId value). Valid when !ready
		std::string waiting;             // what is not recorded yet
		std::string dropReasons;         // why this call asked again
		std::string deferred;            // members whose staged build must switch in first
	};

	/**
	* @brief Render owner. A revision whose member stages are the CS stages among a_epochs, recorded against a_versions (a_serial is
	* its serial, for the cache). The other epochs are added to a_others: they are not revision members (DCLF's until step 10b).
	*/
	std::shared_ptr<RevisionRecordings> MakeRevisionRecordings(std::span<const std::uint32_t> a_epochs, std::shared_ptr<const org::IResourceVersions> a_versions,
		std::uint64_t a_serial, std::vector<std::uint32_t>& a_others);

	/** @brief Render owner. Adds the CS stages of a_epochs that are not members yet. Returns how many were added. */
	std::uint32_t AddRevisionMembers(RevisionRecordings& a_revision, std::span<const std::uint32_t> a_epochs, std::vector<std::uint32_t>& a_others);

	/**
	* @brief Render owner, at Begin. Requests what is missing and reports whether the revision can be adopted now. Never waits.
	* A member with a staged build is deferred: its attached graph takes no recording until the staged build switches in.
	*/
	RevisionProgress AdvanceRevisionRecordings(RevisionRecordings& a_revision, std::uint64_t a_frame);

	/** @brief The member stages' labels. */
	std::vector<std::string> RevisionMembers(const RevisionRecordings& a_revision);

	/**
	* @brief Render owner, inside the adoption point, after the revision's versions were adopted and in the Begin whose
	* AdvanceRevisionRecordings reported ready: each member slot's schedule takes its revision recording. Fatal on a mismatch.
	*/
	void InstallRevisionSchedules(const RevisionRecordings& a_revision);
}
