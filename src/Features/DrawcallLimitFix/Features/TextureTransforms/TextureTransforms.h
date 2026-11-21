#pragma once

// Texture transforms (TexcoordOffset) and the ports' declared frame variables, patched each frame into the main material rows.
// Holds the list of rows that carry one, the pool that samples them, and the pass that writes them (TextureTransformsCS.hlsl).
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF::Api
{
	struct MaterialFrameComponent;
}

namespace DCLF::Features::TextureTransforms
{
	/**
	 * @brief One row of the pool's texture transform buffer. Must match TextureTransformsCS.hlsl's TextureTransformRow.
	 * Row 0 is the header with the entry count E and the animated row count A. Entries are rows 1..E, each stored at rowOffset
	 * into the frame copy where mask bits are set. Animated rows follow, and the pass copies them whole before any entry is stored.
	 */
	struct TextureTransformRow
	{
		std::uint32_t rowOffset = 0;
		std::uint32_t mask = 0;
		std::uint32_t padding[2]{};
		float values[4]{};
	};
	static_assert(sizeof(TextureTransformRow) == 32);

	/**
	 * @brief The rows of one main payload that carry TexcoordOffset or a frame variable. Immutable once built.
	 * Every frame patches all of them, so a backing holds this frame's value whatever its history.
	 */
	struct TextureTransformList
	{
		struct Entry
		{
			const RE::BSShaderMaterial* material = nullptr;
			std::uint32_t rowOffset = 0;  // bytes from the material rows' address to the first component
			std::uint32_t mask = 0;       // the components the record wrote (bit c)
			// A declared frame variable's read. Null means TexcoordOffset, from the frame's buffer.
			const Api::MaterialFrameComponent* components = nullptr;
			bool operator==(const Entry&) const = default;
		};
		std::vector<Entry> entries;
		std::size_t variables = 0;  // the entries of declared frame variables
		// Per row of the frame copy, the material slot copied into it (~0u when free).
		std::vector<std::uint32_t> animatedSlots;
		// The build's own state, so the next build rewrites only the slots that moved.
		std::vector<std::uint32_t> entrySlots;
		std::vector<std::uint8_t> slotVerdicts;
		std::array<std::uint64_t, 4> leftOut{};
	};

	/** @brief Per-stage counters on the list's way to the frame. Any thread, relaxed. */
	namespace TextureTransformStats
	{
		inline std::atomic<std::uint64_t> built{ 0 }, builtEntries{ 0 }, builtVariables{ 0 }, rowsUnpacked{ 0 }, rowsNoTexcoord{ 0 }, rowsFreeSlot{ 0 }, rowsStale{ 0 };
		inline std::atomic<std::uint64_t> published{ 0 }, publishedEntries{ 0 }, publishedNone{ 0 };
		inline std::atomic<std::uint64_t> adopted{ 0 }, adoptedEntries{ 0 }, adoptedNone{ 0 };
	}

	Api::Feature Feature();
	void RegisterPool(Api::InstallContext& a_context);
	void InstallList(std::shared_ptr<const void> a_list);
	/** @brief The pool's backing the current frame reads, 0 before the first frame. Render thread. */
	std::uint32_t RowsIndex();
	/** @brief The frame copy of the animated material rows, or none without a producer or the pass's program. Render thread. */
	Api::FrameRows AnimatedRowsAddress();
	/** @brief The animated-material-rows pool, which the pass declares written. Any thread after install. */
	Api::FrameValuePoolId CopyPool();
	/** @brief Records whether the pass's program was made. Without it nothing writes the copy and no draw points at it. */
	void SetCopyAvailable(bool a_available);
	void RegisterProduct(Api::InstallContext& a_context);
	void RegisterPass(Api::InstallContext& a_context);
}
