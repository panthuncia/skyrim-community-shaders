#pragma once

/**
* @brief The frame revision coordinator (frame revision plan, steps 4c and 4d).
*
* Twins' size wishes (GraphResources::Texture2D/Texture3D::RequestSize) come here instead of a resize at the next frame boundary.
* A frame's Begin hands the wishes to the revision lane, which makes the next versions off the render thread (born cleared for
* histories) and finds the closure: the epochs whose graphs declare a changed slot. The CS stages among them record every frame
* slot against the pending versions. Once all of it is ready, one Begin adopts it whole: versions, exports and every member
* stage's schedule switch in the same frame. One revision is prepared at a time. Wishes that arrive meanwhile wait, latest wins.
* A wish no version can serve (another shape class) takes today's path: a resize at the next Begin.
*/
namespace RenderGraph::Frame::Revision
{
	/** @brief Takes the twins' size wishes. Frame::Driver::Install calls it. Does nothing without the render graph. */
	void Install();

	/**
	* @brief Render thread, first in Frame::Driver::OnBegin inside the adoption point. Applies wishes no version serves, advances
	* the revision in preparation, adopts it when it is ready, and hands newer wishes to the lane. Never waits.
	*/
	void AdoptLatest();

	/** @brief Logs the "[ORG] frame revision:" census line for the window and resets it. */
	void LogStats();
}
