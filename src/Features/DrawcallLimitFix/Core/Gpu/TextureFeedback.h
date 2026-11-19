#pragma once

#include "RenderGraph/BufferRef.h"

#include <cstdint>

// Texture streaming's feedback buffer. One word per material slot in host-visible memory that the colour pass writes and the
// streaming worker reads directly, so it costs no pass, copy or wait. The sides never sync, and a value lost to a race returns later.
namespace DCLF::TextureFeedback
{
	// Word 0 turns the writes on. A slot's word is at slot + 1. Slots are 16 bits.
	inline constexpr std::uint32_t kSlots = 1u << 16;
	inline constexpr std::uint32_t kWords = kSlots + 1;
	inline constexpr std::uint32_t kEmpty = ~0u;

	// Values store the footprint, log2 of the UV one pixel covers, as Lighting.hlsl encodes it.
	inline constexpr float kFootprintOffset = 32.0f, kFootprintScale = 256.0f;

	/** @brief Render thread. Makes the buffer once, when the graph's host is up. */
	void Ensure();

	/** @brief Any thread. The view ref the pixel stages write through, or null. */
	RenderGraph::BufferRef Ref();

	/** @brief Any thread. The mapped words, kWords of them, or null. */
	volatile std::uint32_t* Words();
}
