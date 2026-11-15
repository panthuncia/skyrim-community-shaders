#pragma once

#include "RenderGraph/Frame/PostChain.h"

#include <array>
#include <cstdint>
#include <span>

/**
 * @brief The world post chain's pass-state table: what each declared step of the chain draws, checked into the source
 * (docs/development/org-native-frame-graph.md, "The declared frame, stage 4b").
 *
 * The post description (RenderGraph/Frame/PostChain.h) selects rows: each draw it predicts names a step and its variant (a RowKey:
 * the step, its index, its parity, and the targets the end-of-frame ping-pong gives it). A row says what that draw's pass is: the
 * effect (its program), its outputs and inputs, the samplers' address and filter modes, the engine blend state and the viewport,
 * relative to its output or to the dynamic-resolution size so it survives a resolution change. Nothing in a row is learned at run
 * time.
 *
 * The rows were made from what the engine draws (CS_ORG_POST_TABLE_DUMP=1 logs a row for each declared step's draw, which a script
 * turns into PostTable.inl), then reviewed. A field whose value is not constant for a row is a variant the description lacks: the
 * fix is a field in the description, never a wider row. The check (on with the post description) compares each draw of the chain
 * with its row and counts a "post state mismatch" tap error on any difference.
 */
namespace RenderGraph::Engine::ImageSpace
{
	/** @brief How a row's viewport follows the resolution. */
	enum class ViewportRule : std::uint8_t
	{
		Full,     // the whole output (of its mip)
		Dynamic,  // the output's top-left part scaled by the engine's dynamic-resolution ratios (the render size)
	};

	/** @brief The most texture slots a chain step reads (ISTemporalAA: six). */
	inline constexpr std::uint32_t kPostSlots = 8;

	/** @brief One row of the table. Targets are named as the ImageSpace port names them (RE::RENDER_TARGET, with its kind bits). */
	struct PostRow
	{
		Frame::PostChain::RowKey key;
		std::uint16_t effect = 0;  // ImageSpaceManager::effects index (RE::ImageSpaceManager::ImageSpaceEffectEnum)
		std::int32_t output = -1;
		std::int32_t output1 = -1;
		std::array<std::int32_t, kPostSlots> inputs{ -1, -1, -1, -1, -1, -1, -1, -1 };
		std::array<std::uint8_t, kPostSlots> address{};
		std::array<std::uint8_t, kPostSlots> filter{};
		std::uint32_t blend = UINT32_MAX;  // the engine blend state's four table fields (PackBlend); UINT32_MAX for a dispatch
		bool compute = false;
		ViewportRule viewport = ViewportRule::Full;
	};

	/** @brief Every row, sorted by key. */
	std::span<const PostRow> PostRows();

	/** @brief The row for a key, or null. */
	const PostRow* FindPostRow(const Frame::PostChain::RowKey& a_key);
}
