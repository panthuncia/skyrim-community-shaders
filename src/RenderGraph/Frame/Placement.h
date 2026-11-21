#pragma once

#include <cstdint>

/**
* @brief Where a client's work goes now, and why it does not draw (FrameAnchors::Place, FrameAnchors::Stage,
* Frame::DeclaredClientOf::Held). A small header, so Frame/Declare.h carries it without FrameAnchors.h.
*/
namespace FrameAnchors
{
	/** @brief Why a client's work does not draw at a placement now (Placement::reason). */
	enum class Reason : std::uint8_t
	{
		None,          // it draws: a run of the anchor now draws the client's work, or the stage is open
		NoGraph,       // no render graph, or the caller does not own rendering
		Unregistered,  // the client has no registration (a declared client whose key is empty)
		NoStage,       // no stage of this frame holds the client at the anchor, or the stage is closed
		Passed,        // every stage holding the client at the anchor ended this frame
		Absent,        // the stage that owns the work has no schedule yet; it lags until the first
		Behind,        // the stage runs an older schedule or build, and one with the client is on its way
		Off,           // the frame's schedule holds the registration and has it off (its declared predicate)
		Missing,       // the stage runs a build without the client and none with it is coming: it would never draw
	};

	/** @brief A reason's name, for logs ("behind"). */
	const char* ReasonName(Reason a_reason);

	/**
	* @brief Where a client's work goes now, or a stage's state: Place's and Stage's answer, and what DeclaredClientOf::Held
	* carries. The one placement type.
	*/
	struct Placement
	{
		std::uint32_t stage = UINT32_MAX;  // the stage (a SegmentId value) that takes the work, UINT32_MAX for none
		std::uint64_t generation = 0;      // the scheduled registration's ClientDesc::generation
		bool draws = false;                // the frame's schedule of that stage holds the client's registration and has it on
		Reason reason = Reason::NoStage;
		bool built = false;                // that stage's built graph holds the client's registration

		/** @brief A run of the anchor taps the stage now, or the stage owns the work and has no schedule yet. */
		bool InStage() const { return stage != UINT32_MAX && reason != Reason::Passed; }

		/** @brief Whether the work may lag rather than never draw. False only for Missing and Unregistered. */
		bool MayLag() const { return reason != Reason::Missing && reason != Reason::Unregistered; }
	};
}
