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
 * the frame's transients (2026-11-17)" and "Derived spans (2026-11-18)").
 *
 * The recipe knows the stage order, so a transient's lifetime is a span of frame positions: the stages from its first use to its
 * last. ORG's frame alias arena (org::FrameAliasArena) gives each user a range of memory for its span. Ranges whose spans do not
 * overlap share bytes. So the frame's transient memory is the most that is live at one stage, not the sum of each stage's own.
 *
 * Two kinds of users:
 * - each CS stage's graph, for the transients only its passes use: ORG's planner packs them in a range for the stage's position;
 * - frame transients: resources several stages use (Place). Nothing here declares their span. ORG derives it from the graphs
 *   that use them (org::GraphHost::AddFrameTransients): every world recipe epoch has a position, Drawcall Limit Fix's too, so a
 *   resource's span runs from the first stage whose graph declares it to the last. It starts empty and grows in place as builds
 *   declare it, before they run.
 *
 * What CS still decides by hand: which resources are frame transients at all (their makers call Place), and which content
 * outlives the frame (Recipe.cpp's kPreviousFrame, and every texture not placed here).
 *
 * Hand-over: the runtime's alias ledger orders every use of shared bytes, whatever made the placement. A resource whose bytes
 * another one used since its own last use enters from Undefined, after that use on every queue. So the next span's first pass
 * must write what it reads (the recipe check finds a frame transient whose first use is a read).
 *
 * Native mode only: guest mode runs no CS stage. CS_ORG_FRAME_ARENA=0 keeps each stage's transients in pools of its own and the
 * frame transients committed, for A/B runs.
 */
namespace RenderGraph::Frame::Transients
{
	/** @brief Frame positions, first to last, inclusive. Empty (first after last) for a frame transient nothing uses yet. */
	struct Span
	{
		std::uint32_t first = 0, last = 0;
		bool Empty() const { return first > last; }
	};

	/** @brief The frame position of a CS stage. */
	std::uint32_t PositionOf(SegmentId a_segment);

	/** @brief "pre-main to deferred+water". */
	std::string Describe(Span a_span);

	/** @brief Whether the frame arena is on: native mode with CS stages, and CS_ORG_FRAME_ARENA not 0. Set by Install. */
	bool On();

	/**
	 * @brief Initialize, before the stages' graphs are built: the host gets every world recipe epoch's position, CS's stages with
	 * their own transients in the arena (org::GraphHost::SetFramePositions). False when the arena is off; the caller keeps its
	 * stages' pools private then.
	 */
	bool Install(org::GraphHost& a_host);

	/**
	 * @brief Places unmaterialized ORG textures that several stages use as frame transients, together, and materializes them.
	 * a_label names what made them (the log, and a resize's textures under the same label start from the span the last ones
	 * reached). The placement lasts as long as the returned object; the textures' owner keeps it. Null when the arena is off: the
	 * caller materializes them as before. Throws when the memory cannot be allocated.
	 */
	std::shared_ptr<const void> Place(std::string a_label, std::span<const std::shared_ptr<org::PixelBuffer>> a_textures);

	/** @brief A frame transient's label and its derived span now, by its graph name; empty for anything else. Any thread. */
	std::optional<std::pair<std::string, Span>> FrameTransientOf(std::string_view a_name);

	/** @brief The census line for CS_ORG_EPOCH_STATS: the arena's blocks, what shares them, and the frame transients' spans. */
	void LogStats();
}
