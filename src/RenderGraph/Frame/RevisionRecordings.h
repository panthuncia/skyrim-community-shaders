#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace org
{
	struct IResourceVersions;
}

/**
* @brief The CS stages' recordings for one frame revision (frame revision plan, steps 4c and 4d). Implemented in FrameAnchors.cpp.
* A revision names pending texture and buffer versions, and staged builds. Each member stage records every frame slot that has a schedule against them, through
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
		std::uint32_t builds = 0;        // members recorded on their staged build, which adoption attaches
		std::uint32_t newBuilds = 0;     // staged builds this call found first: new inputs of the revision
		std::string waiting;             // what is not recorded yet
		std::string dropReasons;         // why this call asked again
	};

	/**
	* @brief Render owner. A revision whose member stages are the CS stages among a_epochs, recorded against a_versions (a_serial is
	* its serial, for the cache). The other epochs are added to a_others. Fragment sources' among them record against a_versions and
	* are adopted with the revision as its fragments (Frame::Revision::RegisterFragmentSource).
	*/
	std::shared_ptr<RevisionRecordings> MakeRevisionRecordings(std::span<const std::uint32_t> a_epochs, std::shared_ptr<const org::IResourceVersions> a_versions,
		std::uint64_t a_serial, std::vector<std::uint32_t>& a_others);

	/**
	* @brief Builds a revision must install: every declared segment in a_segments (a bit per SegmentId) takes a build made from
	* the anchors' snapshot a_serial or a newer one. A client registered or removed for a revision needs this, so the revision
	* adopts its values and the stage builds that hold the client in one frame.
	*/
	struct StageBuildNeed
	{
		std::uint64_t segments = 0;
		std::uint64_t serial = 0;
		std::string what;  // for the logs, as "client 'cs.exp-height-fog'"
	};

	/** @brief Render owner. The declared segments that hold client a_id now, a bit each. 0 when it is not registered. */
	std::uint64_t ClientSegments(std::string_view a_id);

	/** @brief Render owner. The serial of the anchors' newest snapshot. A build made from it holds every registration so far. */
	std::uint64_t ClientsSerial();

	/** @brief Render owner. The builds a_revision waits for, replacing the ones given before. */
	void RequireBuilds(RevisionRecordings& a_revision, std::vector<StageBuildNeed> a_needs);

	/** @brief Render owner. Whether a CS stage has a staged build, which only a frame revision attaches. */
	bool StagedStages();

	/** @brief Render owner. Adds the CS stages of a_epochs that are not members yet. Returns how many were added. */
	std::uint32_t AddRevisionMembers(RevisionRecordings& a_revision, std::span<const std::uint32_t> a_epochs, std::vector<std::uint32_t>& a_others);

	/**
	* @brief Render owner, at Begin. Requests what is missing and reports whether the revision can be adopted now. Never waits.
	* Every CS stage with a staged build joins the revision. Such a member records every frame slot on its staged build (the
	* revision's builds fragment), since its attached graph takes no new recording while a build is staged.
	*/
	RevisionProgress AdvanceRevisionRecordings(RevisionRecordings& a_revision, std::uint64_t a_frame);

	/** @brief The epochs of the members recorded on a staged build, for GraphHost::AttachStagedEpochGraphs at adoption. */
	std::vector<std::uint32_t> RevisionStagedEpochs(const RevisionRecordings& a_revision);

	/** @brief The member stages' labels. */
	std::vector<std::string> RevisionMembers(const RevisionRecordings& a_revision);

	/**
	* @brief Render owner, inside the adoption point, after the revision's versions were adopted and its staged builds attached, in
	* the Begin whose AdvanceRevisionRecordings reported ready: each member slot's schedule takes its revision recording. A member
	* on a staged build takes that build's description for every slot. Fatal on a mismatch.
	*/
	void InstallRevisionSchedules(const RevisionRecordings& a_revision, std::uint64_t a_frame);
}
