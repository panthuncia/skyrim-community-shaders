#pragma once

#include "RenderGraph/GraphResources.h"

#include <d3d11.h>

#include <memory>
#include <string_view>

/**
 * @brief The frame's own textures: one texture per name for the whole frame, above the stages (docs/development/org-native-frame-graph.md,
 * "Aliasing survey (2026-11-17)"). A stage's graph imports it; no graph makes it.
 *
 * Why: a client that made its targets again at each registration left the old ones alive in the builds that still held them (a stage
 * that does not run in this frame kind, a previous snapshot), so each image existed once per registration, and a history started over at
 * each one. Here a name keeps its texture while its shape holds:
 * - the same description: the same texture, whatever registered it;
 * - a history or persistent texture of a new size (same format, mips and slices): a new version of the same slot. Passes declare the
 *   slot, so no graph is built again and every stage follows the version;
 * - anything else: a new texture under the name. The old one goes once the last build that holds it is gone.
 *
 * A transient keeps one object too. Each graph that uses it places it in its own alias memory; a stage's next build places it again,
 * and the old placement goes with the build it replaces.
 *
 * Render thread.
 */
namespace RenderGraph::Frame::Resources
{
	/** @brief What Texture2D returned: the texture, and whether its contents are new (made or resized now: a history needs its clear). */
	struct Made
	{
		std::shared_ptr<GraphResources::Texture2D> texture;
		bool fresh = false;
		explicit operator bool() const { return texture && *texture; }
	};

	/**
	 * @brief The frame's texture named a_name for a_desc (a DEFAULT 2D texture) and a_lifetime. A history or persistent texture is
	 * versioned (GraphResources::Options::versioned). Empty, logged, when the texture could not be made.
	 */
	Made Texture2D(std::string_view a_name, const D3D11_TEXTURE2D_DESC& a_desc, GraphResources::Lifetime a_lifetime);
}
