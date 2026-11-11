#pragma once

#include <d3d11.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * @brief One segment of a Scaleform display (BeginDisplay to EndDisplay, or to and from the D3D11 work inside it) as the ORG path
 * draws it: built by the capture (Renderer.cpp) on the
 * render thread, then committed and recorded by Passes.cpp. Plain data, no ORG types.
 */
namespace RenderGraph::Engine::Scaleform
{
	/** @brief The renderer's depth-stencil states (CreateShaders' +0x468 to +0x488). */
	enum class Stencil : std::uint8_t
	{
		Off,        // no stencil (DisableMask, BeginDisplay)
		Clear,      // ALWAYS, REPLACE: BeginSubmitMask(kClear), reference 1
		Increment,  // EQUAL, INCR: BeginSubmitMask(kIncrement), reference the level before
		Decrement,  // EQUAL, DECR: BeginSubmitMask(kDecrement)
		Test,       // EQUAL, KEEP, no writes: EndSubmitMask
	};

	/** @brief A draw's pipeline, apart from the surface's formats. */
	struct DrawState
	{
		std::uint8_t vertexKind = 1;  // FillVS.hlsl's VS_KIND
		std::uint8_t pixelKind = 1;   // FillPS.hlsl's PS_KIND
		std::uint8_t blend = 1;       // the renderer's blend state index, 1 to 7
		Stencil stencil = Stencil::Off;

		auto operator<=>(const DrawState&) const = default;
	};

	/** @brief A display command: a draw, or the stencil clear BeginSubmitMask(kClear) makes. */
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
		std::uint32_t record = 0;       // bytes into the display's staging bytes: its ScaleformFillDraw
		std::uint32_t vertexCount = 0;  // vertices the draw makes (triangles times three)
		std::int32_t textures[2] = { -1, -1 };  // indices into Display::textures, stages 0 and 1
		std::uint8_t samplers[2] = { 0, 0 };    // the renderer's sampler index (wrap * 2 + sample), stages 0 and 1
	};

	/** @brief ScaleformFillDraw in Scaleform/Fill.hlsli: the D3D11 constants and where the draw's vertices are. */
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

	/** @brief What a display draws and onto what. */
	struct Display
	{
		std::vector<std::byte> staging;  // geometry and records, in the layout the staging buffer gets
		std::vector<Command> commands;
		std::vector<ID3D11ShaderResourceView*> textures;  // DXVK's views, each with a reference this display holds
		// The viewport BeginDisplay set and the target's rectangle it clips to (pixels).
		float viewport[4] = {};  // x, y, width, height
		ID3D11Texture2D* target = nullptr;      // render target 0's texture (DXVK's), referenced
		DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN;  // its RTV's format
		ID3D11Texture2D* depth = nullptr;       // the bound DSV's texture (DXVK's), referenced; null without one
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;

		Display() = default;
		Display(const Display&) = delete;
		Display& operator=(const Display&) = delete;
		~Display() { Reset(); }

		/** @brief Empty again: the references dropped. */
		void Reset()
		{
			ResetCommands();
			if (target)
				target->Release();
			if (depth)
				depth->Release();
			target = depth = nullptr;
			targetFormat = depthFormat = DXGI_FORMAT_UNKNOWN;
		}

		/** @brief The commands, their bytes and textures dropped; the target and viewport kept (the next segment of the display). */
		void ResetCommands()
		{
			for (auto* view : textures)
				view->Release();
			textures.clear();
			staging.clear();
			commands.clear();
		}

		/** @brief a_bytes appended at a_alignment; their offset. */
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
