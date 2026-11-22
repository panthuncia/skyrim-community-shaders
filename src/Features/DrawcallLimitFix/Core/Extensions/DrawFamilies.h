#pragma once

#include "RenderGraph/BufferRef.h"

// Core's side of Api/DrawFamilies.h: each family's entry, resolved once at install, and the contexts its callbacks receive.
// A family no feature takes has no entry, so its passes and requirements are left out.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org::services
{
	class NamedVersions;
}

namespace org
{
	class Buffer;
	class RenderPassBuilder;
	struct ResourceBindingToken;
}

namespace DCLF
{
	namespace Draws
	{
		struct SceneBuffers;
		struct ViewResources;
		struct UnitGroup;
	}
}

namespace DCLF::Api
{
	/** @brief The frame a family's frame callbacks run in. tables is null during serve. */
	class DrawFamilyFrame
	{
	public:
		std::uint32_t frame = 0;
		Draws::SceneBuffers* scene = nullptr;
		DrawFamily family = DrawFamily::Count;
		// Set by Pipelines when the family can draw something and is waiting for it.
		bool* waits = nullptr;

		/**
		 * @brief For decide. Posts the program source and requests the slot's pipelines.
		 * Returns true once they exist and a_drawable is set. A drawable family that is not ready sets waits, so its passes stay withheld.
		 */
		bool Pipelines(bool a_drawable) const;
	};

	/** @brief A pass's declarations at graph build, for the main pass or a trees view's faces. view is null for the main pass. */
	class DrawFamilyDeclare
	{
	public:
		org::RenderPassBuilder& builder;
		const Draws::SceneBuffers& scene;
		const Draws::ViewResources* view;
		std::span<org::ResourceBindingToken> arguments;
		RenderGraph::BufferRef row;  // out, main pass only. Fixed with the family's tables.
		// The main pass's depth shape, which draws only the depth programs. Without it, also declare what only the colour programs read.
		bool depth = false;
	};

	/**
	 * @brief The family's main draw for the CPU-driven pipeline, at the depth commit. Outputs the draw row and the arguments buffer.
	 * The caller holds the buffer with the frame.
	 */
	class DrawFamilyDirect
	{
	public:
		const Draws::SceneBuffers& scene;
		const org::services::NamedVersions& adopted;
		RenderGraph::BufferRef row;
		std::shared_ptr<org::Buffer> arguments;
	};

	/** @brief A main pass's revision. pipelines is null when the shape's own pipelines apply. */
	class DrawFamilyRevision
	{
	public:
		const Draws::SceneBuffers& scene;
		const std::shared_ptr<const void>& pipelines;
	};

	enum class DrawFamilyPass : std::uint8_t
	{
		Depth,          // the depth segment's phase 1 (z.depth)
		DepthPhaseTwo,  // its phase 2 (depth-phase2)
		Colour,         // the colour segment (main-opaque), under the Z-prepass layout
		Face,           // a trees view's face
	};

	/** @brief A family's draw, recorded inside the pass's render pass. */
	class DrawFamilyRecord
	{
	public:
		rhi::CommandList& commands;
		DrawFamilyPass pass;
		const std::shared_ptr<const void>& drawn;
		RenderGraph::BufferRef row;
		rhi::ResourceHandle arguments;
		rhi::PipelineHandle pipeline;
		rhi::CommandSignatureHandle signature;

		/** @brief The slot's pipelines as FamilyPipelines. The feature binds the colour pipeline for each draw in the colour pass. */
		template <class T>
		const T& Drawn() const
		{
			return *static_cast<const T*>(drawn.get());
		}
	};

	/**
	 * @brief A trees view's epoch commit. Writes the zeroed row the faces' latch takes and holds what the row binds.
	 * Without owners it only reports whether the last depth commit's row draws.
	 */
	class DrawFamilyFaceRow
	{
	public:
		const Draws::SceneBuffers& scene;
		bool viewDraws;
		std::span<std::byte> row;
		std::vector<std::shared_ptr<const void>>* owners;
	};

	/**
	 * @brief A trees view's faces' lists, whose units come from the family's tables.
	 * Outputs the tables, the units the revision needs, and what the current versions hold.
	 */
	class DrawFamilyLists
	{
	public:
		const Draws::SceneBuffers& scene;
		std::span<const std::uint32_t> inputs;
		Draws::UnitGroup* tables = nullptr;
		std::uint64_t required = 0;
		std::uint32_t current = 0;
	};

	/** @brief One face of a trees view's epoch commit. Writes the face's row and its list's header. */
	class DrawFamilyFaceWrite
	{
	public:
		std::span<const std::byte> commitRow;
		RenderGraph::BufferRef listRef;
		bool drawn = false;
		std::span<std::byte> row;
		std::span<std::byte> header;
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kDrawFamilies = static_cast<std::size_t>(Api::DrawFamily::Count);

	/** @brief Where a family's face values sit in the face latch, after the face block and earlier families in family order. */
	struct DrawFamilyFaceLayout
	{
		std::uint32_t rowInFace = 0, headerInFace = 0;
	};

	/** @brief A family's entry, with its strings owned. */
	struct DrawFamilyEntry
	{
		bool taken = false;
		Api::DrawFamilyContribution contribution;
		std::string colourRange;
		// The contribution's string views point into these.
		std::string name, label, keyName, sourcePath, nativeNote;
		std::string rowName, listName;
		std::uintptr_t engineShapeVtable = 0;
		DrawFamilyFaceLayout faceLayout;
	};
	using DrawFamilySet = std::array<DrawFamilyEntry, kDrawFamilies>;

	/** @brief Main thread, at the end of InstallFeatures. Builds the families' entries. */
	void ResolveDrawFamilies(const std::array<Api::DrawFamilyContribution, kDrawFamilies>& a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const DrawFamilySet& DrawFamilies();
	inline const DrawFamilyEntry& DrawFamilyOf(Api::DrawFamily a_family) { return DrawFamilies()[static_cast<std::size_t>(a_family)]; }
	/** @brief Any thread, after InstallFeatures. The family whose program uses a_shaderType, or Count. */
	Api::DrawFamily DrawFamilyOfShader(std::uint8_t a_shaderType);
	/** @brief Any thread, after InstallFeatures. Bytes of a trees view's face latch values, rounded to 256. */
	std::uint32_t DrawFamilyFaceBytes();
	/** @brief Whether a_family is taken and its object class is DCLF's. */
	bool DrawFamilyOn(Api::DrawFamily a_family);
	/** @brief Whether a_family draws in a trees view's faces. Grass has no faces. */
	inline bool DrawFamilyHasFaces(Api::DrawFamily a_family)
	{
		const auto& entry = DrawFamilyOf(a_family);
		return entry.taken && entry.contribution.faces.face;
	}

	/** @brief Render thread, at the frame requests. Every taken family's serve, in family order. */
	void ServeDrawFamilies(std::uint32_t a_frame);
}
