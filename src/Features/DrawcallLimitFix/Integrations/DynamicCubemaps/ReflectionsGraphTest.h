#pragma once

// The graph texture test (CS_DCLF_GRAPH_TEXTURE_TEST): a stand-in for Dynamic Cubemaps on its ORG path, which proves the frame graph
// textures (Api::FrameTexture::graph) end to end before any texture producer is ported. At the Prepass anchor a graph pass copies the
// reflections cubemap Dynamic Cubemaps bound at t30 (its D3D11 texture, imported) into an exported graph cubemap, published as
// kResourceId (RenderGraph/PublishedResources.h). The integration's t30 then names that cubemap:
// - "graph": through the graph (its published descriptor; the colour passes declare the read),
// - "d3d11": through its exported D3D11 view (imported as any CS view),
// so the two runs read the same memory by the two paths, and their screenshots must be identical. Unset or anything else: off. The live
// toggle "graph texture test" (seeded on by "graph") switches between the two paths inside one run.

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>

namespace DCLF::Integrations::DynamicCubemaps::ReflectionsGraphTest
{
	/** @brief The published id of the test's cubemap. */
	inline constexpr const char* kResourceId = "cs.dclf.test.reflections";

	enum class Mode : std::uint8_t
	{
		Off,
		Graph,
		D3D11,
	};

	/**
	 * @brief Main thread, at the integration's install: the switch's mode and the live toggle choosing the graph path; reserves the Prepass
	 * anchor when the test is on.
	 */
	void Install(Mode a_mode, Api::ToggleId a_graph);

	/** @brief The mode Install was given. */
	Mode CurrentMode();

	/**
	 * @brief Render thread: once, with the render graph active, makes the test's cubemap, publishes it and registers its anchor client.
	 * @return The cubemap's exported view, or null while it does not exist (the integration then names Dynamic Cubemaps' own).
	 */
	void* View();

	/** @brief Render thread: while the toggle is on, the published cubemap's descriptor while it is published. */
	bool GraphIndex(std::uint32_t& a_index);
}
