#pragma once

#include <d3d11.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * @brief One segment of a Scaleform display as the ORG path draws it. Renderer.cpp builds it on the render thread
 * and Passes.cpp commits and records it. Plain data, no ORG types.
 */
namespace RenderGraph::Engine::Scaleform
{
	/** @brief The renderer's depth-stencil states, CreateShaders' +0x468 to +0x488. */
	enum class Stencil : std::uint8_t
	{
		Off,        // no stencil (DisableMask, BeginDisplay)
		Clear,      // ALWAYS, REPLACE: BeginSubmitMask(kClear), reference 1
		Increment,  // EQUAL, INCR: BeginSubmitMask(kIncrement), reference the level before
		Decrement,  // EQUAL, DECR: BeginSubmitMask(kDecrement)
		Test,       // EQUAL, KEEP, no writes: EndSubmitMask
	};

	struct DrawState
	{
		std::uint8_t vertexKind = 1;  // FillVS.hlsl's VS_KIND, plus 6 for its VS_3D twin, 0 for a filter draw
		std::uint8_t pixelKind = 1;   // FillPS.hlsl's PS_KIND, 0 for a filter draw
		std::uint8_t blend = 1;       // 1 to 7, or 9 to 15 for the filters' array
		Stencil stencil = Stencil::Off;
		std::uint8_t filter = 0;  // FilterPS.hlsl's FILTER, 0 for a fill draw
		std::uint8_t lines = 0;   // 1 for a line strip, 0 for a triangle list

		auto operator<=>(const DrawState&) const = default;
	};

	inline constexpr std::int32_t kNoTexture = -1;

	/** @brief Command::textures value for scratch image a_scratch, counting down from -2. */
	constexpr std::int32_t ScratchTexture(std::int32_t a_scratch) { return -2 - a_scratch; }

	constexpr std::int32_t ScratchOf(std::int32_t a_texture) { return a_texture <= -2 ? -2 - a_texture : -1; }

	/** @brief A draw, or the stencil clear BeginSubmitMask(kClear) makes. */
	struct Command
	{
		enum class Kind : std::uint8_t
		{
			Draw,
			ClearStencil,
		};
		Kind kind = Kind::Draw;
		DrawState state;
		std::uint32_t stencilRef = 0;
		std::uint32_t record = 0;       // byte offset of its ScaleformFillDraw in the staging bytes
		std::uint32_t vertexCount = 0;  // triangles times three
		std::int32_t textures[2] = { kNoTexture, kNoTexture };  // indices into Display::textures, or ScratchTexture(k)
		std::uint8_t samplers[2] = { 0, 0 };    // wrap * 2 + sample
		bool noColour = false;  // a bitmap fill without a texture writes the stencil only
	};

	/** @brief ScaleformFillDraw in Scaleform/Fill.hlsli. */
	struct DrawRecord
	{
		float mvp[4][4];
		float texgen[4][4];
		float cxmul[4];
		float cxadd[4];
		std::uint32_t vertexOffset;
		std::uint32_t vertexStride;
		std::uint32_t indexOffset;
		std::uint32_t indexSize;
		std::int32_t baseVertex;
		std::uint32_t padding[3];
	};
	static_assert(sizeof(DrawRecord) == 192);

	/** @brief A filter draw's bytes, as Scaleform/Filter.hlsli reads them. */
	struct FilterRecord
	{
		float vertices[6][6];
		float mvp[4][4];
		float cxadd[4];
		float cxmul[4][4];
	};
	static_assert(sizeof(FilterRecord) == 288);
	inline constexpr std::uint32_t kFilterMatrixOffset = 144;     // offsetof(FilterRecord, mvp)
	inline constexpr std::uint32_t kFilterConstantsOffset = 208;  // offsetof(FilterRecord, cxadd)
	static_assert(offsetof(FilterRecord, mvp) == kFilterMatrixOffset && offsetof(FilterRecord, cxadd) == kFilterConstantsOffset);

	struct Display
	{
		std::vector<std::byte> staging;  // in the layout the staging buffer gets
		std::vector<Command> commands;
		std::vector<ID3D11ShaderResourceView*> textures;  // each holds a reference
		// The viewport BeginDisplay set, in pixels.
		float viewport[4] = {};  // x, y, width, height
		// The buffer size GFx laid the display out for (its viewport's buffer). The draw maps it onto the target's size, which
		// differs after a window resize until the movie is loaded again. Zero for a render texture display, never mapped.
		float layout[2] = {};                   // width, height
		ID3D11Texture2D* target = nullptr;      // referenced
		DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN;  // its RTV's format
		ID3D11Texture2D* depth = nullptr;       // referenced, null without one
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
		// For a target GFx pushed, the scratch image drawn in its place. -1 means `target`, and kLostScratch draws nothing.
		std::int32_t scratch = -1;
		static constexpr std::int32_t kLostScratch = -2;
		// The segment starts at a GFx push, which clears the target whole to zero.
		bool clearTarget = false;

		Display() = default;
		Display(const Display&) = delete;
		Display& operator=(const Display&) = delete;
		~Display() { Reset(); }

		/** @brief Empties the display and drops its references. */
		void Reset()
		{
			ResetCommands();
			if (target)
				target->Release();
			if (depth)
				depth->Release();
			target = depth = nullptr;
			targetFormat = depthFormat = DXGI_FORMAT_UNKNOWN;
			scratch = -1;
		}

		/** @brief Drops the commands but keeps the target and viewport for the next segment. */
		void ResetCommands()
		{
			for (auto* view : textures)
				view->Release();
			textures.clear();
			staging.clear();
			commands.clear();
			clearTarget = false;
		}

		/** @brief Appends a_bytes at a_alignment and returns their offset. */
		std::uint32_t Append(const void* a_bytes, std::size_t a_size, std::size_t a_alignment = 4)
		{
			const std::size_t offset = (staging.size() + a_alignment - 1) / a_alignment * a_alignment;
			staging.resize(offset + a_size);
			if (a_size)
				std::memcpy(staging.data() + offset, a_bytes, a_size);
			return static_cast<std::uint32_t>(offset);
		}
	};
}
