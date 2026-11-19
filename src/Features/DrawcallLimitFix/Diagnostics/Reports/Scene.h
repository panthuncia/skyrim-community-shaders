#pragma once

#include <cstdint>
#include <string>

namespace DCLF
{
	namespace Scene
	{
		class Coordinator;
	}
}

namespace DCLF::Reports
{

	/** @brief The coordinator's own lines when a_sceneFrame crossed into a new window. Coordinator, at a pass's end. */
	void ScenePassEnd(Scene::Coordinator& a_store, std::uint32_t a_sceneFrame);
	/** @brief The lines of what the render thread keeps, newline-terminated. Render thread. */
	std::string SceneFrameLines();
	/** @brief The GateReleases report point's lines when a_frame crossed into a new window. Render thread, frame start. */
	void GateReleases(std::uint32_t a_frame);
}
