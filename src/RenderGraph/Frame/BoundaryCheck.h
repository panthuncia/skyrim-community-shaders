#pragma once

#include <cstdint>
#include <source_location>
#include <string_view>

/**
* @brief The frame boundary census (frame revision plan, step 1). Rule R1: what a recording binds changes only at the adoption point.
*
* The adoption point is Frame::Driver::OnBegin, marked with a Scope. On the render thread, outside it, this counts:
* - late versions: every versioned resource adoption ORG reports (VersionedResource::SetAdoptionObserver), by resource name and caller tag;
* - late changes: client registrations and structural build requests made inside the frame, by what, caller tag and source site.
*   Other build requests are kept until the adoption point (Builds::Ask), so they are never late;
* - late attaches: graph builds attached or staged graphs switched in outside the adoption point;
* - refusals: builds ORG refused at their staging, attach or switch, by site and reason, on any thread (plan Q9).
* Late versions and late attaches are tap errors (TapError::OutsideAdoption), late changes are TapError::LateChange. All are
* fatal in development builds. In release builds (CS_RELEASE_PROFILE) the census still counts, and the first of each kind is
* logged and raises the DCLF menu's warning (Driver::NoteTapError). There are no exceptions. A new resource's first version is
* counted, not an error. One "[ORG] boundary:" line per statistics window and one at a test run's exit.
* The render thread is the render owner (FrameAnchors::IsRenderOwner), the thread that began the frame. Other threads are not counted.
*/
namespace RenderGraph::Frame::Boundary
{
	/** @brief Whether the calling thread is inside the adoption point (a Scope is open). Engine imports change only there (step 9b). */
	bool InAdoptionPoint();

	/** @brief Marks the adoption point on the calling thread. Frame::Driver::OnBegin opens it around its body. */
	class Scope
	{
	public:
		Scope();
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		bool outer;
	};

	/** @brief Names who adopts versions or attaches builds on the calling thread while it lives. a_tag must outlive it. */
	class Caller
	{
	public:
		explicit Caller(std::string_view a_tag);
		~Caller();
		Caller(const Caller&) = delete;
		Caller& operator=(const Caller&) = delete;

	private:
		std::string_view outer;
	};

	/** @brief Sets ORG's adoption observer. Every build with the render graph. */
	void Install();

	/**
	* @brief A registration or structural build request. Made on the render thread outside the adoption point, it is a late change:
	* counted once, by a_where, and a tap error. Changes it makes while it lives are part of it and not counted again.
	*/
	class Change
	{
	public:
		Change(std::string_view a_what, std::string_view a_caller, const std::source_location& a_where = std::source_location::current());
		~Change();
		Change(const Change&) = delete;
		Change& operator=(const Change&) = delete;
	};

	/** @brief A graph attached for a_epoch. Counted when made on the render thread outside the adoption point, with the caller tag. */
	void NoteAttach(std::string_view a_epoch);

	/** @brief ORG refused a build at a_site (staging, attach, switch) for a_reason (GraphHost::AttachRefusalName). Counted on any thread. */
	void NoteRefusal(std::string_view a_site, std::string_view a_reason);

	/** @brief Logs the census line for the window and resets it. Called by the test run's exit report. */
	void LogStats();
}
