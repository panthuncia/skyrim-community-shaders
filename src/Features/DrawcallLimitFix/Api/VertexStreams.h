#pragma once

// The second vertex stream: per-frame float4 positions for objects whose engine draws read BSDynamicTriShape::dynamicData.
// Core keeps one row per stream object in the scene tables and binds the feature's buffer as a vertex buffer.
// Objects of the entry class wait until they have a row. Core's draws have a single second-stream slot, so only one stream exists.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace DCLF::Api
{
	// Defined in Core/Extensions/VertexStreams.h.
	class VertexStreamSelect;
	class VertexStreamUpload;

	/** @brief One object's vertices in the stream and their range of the stream's buffer. */
	struct VertexStreamRow
	{
		static constexpr std::uint32_t kNoObject = ~0u;
		static constexpr std::uint32_t kNoFirst = ~0u;
		std::uint32_t object = kNoObject;  // kNoObject marks a free row
		std::uint32_t first = kNoFirst;
		std::uint32_t count = 0;
		std::uint64_t generation = 0;      // a new value triggers an upload
		const float* vertices = nullptr;   // float4 per vertex, valid while owner is held
		std::shared_ptr<const std::vector<float>> owner;
		// The feature's own data, carried with the row. Core never reads it.
		std::shared_ptr<const void> group;
		// The entry's BSGeometry, as a key for the row's release.
		const void* source = nullptr;
	};

	/**
	 * @brief The vertex stream. Callbacks run at scene setup and in commits, never per object.
	 *
	 * - resources: set up first, because every stream draw binds this buffer. Without state the scene's tables are retried next frame.
	 * - select: render thread, once after setup. Picks the versioned buffer holding the rows.
	 * - upload: render thread, first in each commit. Returns how many rows it sent.
	 */
	struct VertexStreamContribution
	{
		std::string_view name;
		EntryClass entryClass = EntryClass::Count;
		SceneResourcesId resources;
		void (*select)(VertexStreamSelect& a_select) = nullptr;
		std::uint32_t (*upload)(const VertexStreamUpload& a_upload) = nullptr;
	};
}
