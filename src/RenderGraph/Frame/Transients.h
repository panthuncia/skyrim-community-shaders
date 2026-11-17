#pragma once

#include "RenderGraph/Frame/FrameDescription.h"
#include "RenderGraph/Frame/Recipe.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace org
{
	class GraphHost;
	class PixelBuffer;
}

/**
 * @brief The frame's transient memory: one arena for the whole frame (docs/development/org-native-frame-graph.md, "One arena for
 * the frame's transients (2026-11-17)").
 *
 * The recipe knows the stage order, so a transient's lifetime is a span of frame positions: the stages from its first use to its
 * last. ORG's frame alias arena (org::FrameAliasArena) gives each user a range of memory for its span. Ranges whose spans do not
 * overlap share bytes. So the frame's transient memory is the most that is live at one stage, not the sum of each stage's own.
 *
 * Two kinds of users:
 * - each CS stage's graph, for the transients only its passes use (Install): ORG's planner packs them in the stage's range, as
 *   it packed the stage's own pool before;
 * - the groups below: resources several stages use, placed once when they are made (Place). Their span is declared here.
 *
 * Hand-over: the runtime's alias ledger orders every use of shared bytes, whatever made the placement. A resource whose bytes
 * another one used since its own last use enters from Undefined, after that use on every queue. So the next span's first pass
 * must write what it reads (the recipe check finds a group whose first use is a read).
 *
 * A use outside a group's span is a recipe mismatch: the recipe check reports it (fatal in development builds, logged once in
 * release builds).
 *
 * Native mode only: guest mode runs no CS stage. CS_ORG_FRAME_ARENA=0 keeps each stage's transients in pools of its own and the
 * groups committed, for A/B runs.
 */
namespace RenderGraph::Frame::Transients
{
	/** @brief Frame positions, first to last, inclusive. */
	struct Span
	{
		std::uint32_t first = 0, last = 0;
	};

	/** @brief The frame position of a CS stage. */
	std::uint32_t PositionOf(SegmentId a_segment);

	/** @brief "pre-main to deferred+water". */
	std::string Describe(Span a_span);

	/** @brief The resource groups several stages use, each with its span (Transients.cpp). */
	enum class Group : std::uint8_t
	{
		GBuffer,           // Deferred's G-buffer: cleared in pre-main, read by the composite in deferred+water; DCLF's passes bind it from
		                   // the Z-prepass to first person
		ScaleformScratch,  // Scaleform's scratch images (GFx's render targets and filter passes): the UI stage only
		Count
	};

	const char* NameOf(Group a_group);
	Span SpanOf(Group a_group);

	/** @brief Whether the frame arena is on: native mode with CS stages, and CS_ORG_FRAME_ARENA not 0. Set by Install. */
	bool On();

	/**
	 * @brief Initialize, before the stages' graphs are built: the host gets each CS stage's span (org::GraphHost::SetFrameAliasSpans).
	 * a_epochs: each stage's epoch. False when the arena is off; the caller keeps its stages' pools private then.
	 */
	bool Install(org::GraphHost& a_host, std::span<const std::pair<SegmentId, std::uint32_t>> a_epochs);

	/**
	 * @brief Places unmaterialized ORG textures of a group in the arena, one after another, and materializes them. The placement lasts
	 * as long as the returned object; the textures' owner keeps it. Null when the arena is off: the caller materializes them as before.
	 * Throws when the memory cannot be allocated.
	 */
	std::shared_ptr<const void> Place(Group a_group, std::span<const std::shared_ptr<org::PixelBuffer>> a_textures);

	/** @brief The span of a placed group resource, by its graph name; empty for anything else. For the recipe check. Any thread. */
	std::optional<std::pair<Group, Span>> GroupOf(std::string_view a_name);

	/** @brief The census line for CS_ORG_EPOCH_STATS: the arena's blocks and what shares them. */
	void LogStats();
}
