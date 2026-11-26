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
 * @brief One memory arena for the whole frame's transients.
 *
 * A transient lives from the first stage that uses it to the last. Transients whose spans do not overlap share bytes, so the
 * frame needs only the most memory live at one stage. Each stage's graph places its own transients. Resources several stages
 * use are placed with Place, and ORG derives their span from the graphs that declare them.
 * A resource whose bytes were reused since its last use starts from Undefined, so the first pass of its span must write it.
 */
namespace RenderGraph::Frame::Transients
{
	/** @brief Frame positions, first to last, inclusive. Empty for a frame transient nothing uses yet. */
	struct Span
	{
		std::uint32_t first = 0, last = 0;
		bool Empty() const { return first > last; }
	};

	std::uint32_t PositionOf(SegmentId a_segment);

	/** @brief Such as "pre-main to deferred+water". */
	std::string Describe(Span a_span);

	/** @brief Native mode with CS stages. Set by Install. */
	bool On();

	/** @brief Gives the host every stage's frame position before the graphs are built. */
	bool Install(org::GraphHost& a_host);

	/**
	 * @brief Places textures several stages use as frame transients and materializes them. The owner keeps the returned object.
	 * Null in guest mode, so the caller materializes them itself. Throws when the memory cannot be allocated.
	 */
	std::shared_ptr<const void> Place(std::string a_label, std::span<const std::shared_ptr<org::PixelBuffer>> a_textures);

	/** @brief A frame transient's label and current span, by graph name. Empty for anything else. Any thread. */
	std::optional<std::pair<std::string, Span>> FrameTransientOf(std::string_view a_name);
}
