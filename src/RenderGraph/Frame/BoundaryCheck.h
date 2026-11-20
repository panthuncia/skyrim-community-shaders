#pragma once

#include <cstdint>
#include <string_view>

/**
* @brief The frame boundary census (frame revision plan, step 1). Rule R1: what a recording binds changes only at the adoption point.
*
* The adoption point is Frame::Driver::OnBegin, marked with a Scope. On the render thread, outside it, this counts:
* - late versions: every versioned resource adoption ORG reports (VersionedResource::SetAdoptionObserver), by resource name and caller tag;
* - late changes: client registrations and rebuild requests made inside the frame;
* - late attaches: graph builds attached or staged graphs switched in outside the adoption point;
* - refusals: builds ORG refused at their staging, attach or switch, by site and reason, on any thread (plan Q9).
* Late versions and late attaches are tap errors (TapError::OutsideAdoption), fatal in development builds, unless an Allow scope
* names an Exception. Exceptions are counted apart, by name. A new resource's first version is counted, not an error.
* Late changes and the counts below them stay counted, not fatal, until step 10c.
* One "[ORG] boundary:" line per statistics window and one at a test run's exit, in development builds only (not CS_RELEASE_PROFILE).
* The render thread is the one that last opened a Scope. Other threads are not counted.
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

	/** @brief The exceptions to rule R1 allowed for now. Each is deleted by the step that names it. */
	enum class Exception : std::uint8_t
	{
		Dclf,  // DCLF adopts its snapshot's versions and attaches its own epochs' builds at its build points (step 10a)
		Count
	};

	/** @brief Allows late versions and late attaches as a_exception on the calling thread while it lives. They are counted by name. */
	class Allow
	{
	public:
		explicit Allow(Exception a_exception);
		~Allow();
		Allow(const Allow&) = delete;
		Allow& operator=(const Allow&) = delete;

	private:
		Exception outer;
	};

	/** @brief Sets ORG's adoption observer. Development builds only. */
	void Install();

	/**
	* @brief A registration or rebuild request, counted once when made on the render thread outside the adoption point.
	* Build requests it makes while it lives are part of it and not counted again.
	*/
	class Change
	{
	public:
		Change(std::string_view a_what, std::string_view a_caller);
		~Change();
		Change(const Change&) = delete;
		Change& operator=(const Change&) = delete;
	};

	/** @brief A build request (RenderGraph::Builds). Counted like a Change unless one is open on the calling thread. */
	void NoteBuildRequest(std::string_view a_reason);

	/** @brief A graph attached for a_epoch. Counted when made on the render thread outside the adoption point, with the caller tag. */
	void NoteAttach(std::string_view a_epoch);

	/** @brief ORG refused a build at a_site (staging, attach, switch) for a_reason (GraphHost::AttachRefusalName). Counted on any thread. */
	void NoteRefusal(std::string_view a_site, std::string_view a_reason);

	/** @brief Logs the census line for the window and resets it. Called from Frame::Driver::LogStats. */
	void LogStats();
}
