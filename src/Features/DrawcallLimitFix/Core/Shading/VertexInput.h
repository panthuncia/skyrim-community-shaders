#pragma once

#include <array>
#include <cstdint>
#include <dxgiformat.h>

namespace DCLF
{
	/** @brief The part of a BSGraphics::VertexDesc a pipeline depends on: attribute flags and offsets, without the stride in bits 0-3. */
	constexpr std::uint64_t VertexLayoutOf(std::uint64_t a_vertexDesc) { return a_vertexDesc & ~0xFull; }

	/**
	 * @brief A vertex shader's input mask, built as ShaderCache builds vertexDesc: each consumed attribute's flag in both streams,
	 * bits 44 + a and 54 + a, plus its offset nibble. The engine picks its input layout by mask & geometry VertexDesc.
	 */
	void AddVertexAttribute(std::uint64_t& a_mask, std::uint32_t a_attribute);

	/** @brief The attributes a vertex layout has in either stream, which a pulled vertex stage can load. Bit a is attribute a. */
	constexpr std::uint32_t VertexLayoutAttributes(std::uint64_t a_layout) { return static_cast<std::uint32_t>(((a_layout >> 44) | (a_layout >> 54)) & 0x3FFu); }

	/** @brief One D3D11_INPUT_ELEMENT_DESC of an engine input layout. */
	struct VertexElement
	{
		const char* semantic;
		std::uint32_t semanticIndex;
		DXGI_FORMAT format;
		std::uint32_t slot;  // 0 is the geometry's vertex buffer, 1 the second stream of dynamic geometry
		std::uint32_t offset;
		bool perInstance;
	};

	struct VertexElements
	{
		std::array<VertexElement, 16> elements{};
		std::uint32_t count = 0;
	};

	/** @brief The input layout the engine creates for a layout key, ported from AE 0x140e4bf20. Positions are always four floats. */
	VertexElements BuildVertexElements(std::uint64_t a_key);
}
