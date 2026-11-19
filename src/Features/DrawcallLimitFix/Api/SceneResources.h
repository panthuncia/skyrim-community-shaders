#pragma once

// A feature's GPU state held with the scene's tables: its programs and buffers.
// Versioned buffers join the scene's buffer set, which grows them to each revision's needs.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/SceneResources.h.
	class SceneResourcesSetup;
	class SceneResourcesGraph;
	class SceneResourcesRequirements;
	class SceneResourcesFit;
	class SceneResourcesLatch;
	class SceneResourcesSealed;

	/** @brief Registered scene resources. Extensions::SceneResourcesOf finds their state by this id. */
	struct SceneResourcesId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A feature's scene resources. Callbacks run in registration order, with the vertex stream's resources first.
	 *
	 * - setup: render thread, once, before the passes' setups. A null result means none, and no other callback runs.
	 * - require: snapshot builder, per revision. A set seals only once the buffers hold what this requires.
	 *   It may read only table families, the vertex stream rows and the posted input words, because a revision whose witnesses
	 *   of those did not change is kept without calling it. Anything else must go through postInputs.
	 * - fit: what the buffers hold, so an object past them waits.
	 * - postInputs: render thread. Writes inputWords zeroed words. A change makes a new revision.
	 * - latched: buffers the depth commit latches, each from offset 0.
	 * - sealed: snapshot builder, once a revision's set seals, before any of its shapes.
	 * The state belongs to the render thread. Other threads may read only the handles setup made, which are never replaced.
	 */
	struct SceneResources
	{
		std::string_view name;
		std::shared_ptr<void> (*setup)(const SceneResourcesSetup& a_setup) = nullptr;
		void (*registerGraph)(const SceneResourcesGraph& a_graph) = nullptr;
		void (*require)(const SceneResourcesRequirements& a_requirements) = nullptr;
		void (*fit)(SceneResourcesFit& a_fit) = nullptr;
		void (*postInputs)(std::span<std::uint32_t> a_words) = nullptr;
		std::uint32_t inputWords = 0;
		void (*latched)(SceneResourcesLatch& a_latch) = nullptr;
		void (*sealed)(const SceneResourcesSealed& a_sealed) = nullptr;
	};
}
