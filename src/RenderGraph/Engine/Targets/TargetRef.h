#pragma once

#include <d3d11.h>

#include <array>
#include <cstdint>
#include <optional>

/**
* @brief Names for the engine's render targets and their views (cleanup plan B4, phase 3 design 4.1).
*
* A TargetRef says which engine target a field handle is, and which of its views. Targets::Identify answers it for a field handle,
* and Targets::Slot (EngineTargetSet.h) gives the declared texture for it. This header includes nothing of ORG or OMS, so ports
* include it cheaply.
*/
namespace RenderGraph::Engine::Targets
{
	// Room above the engine's counts: render targets SE 114 / AE 116, depth-stencils 12, one cubemap, three 3D.
	inline constexpr std::uint32_t kColourSlots = 128;
	inline constexpr std::uint32_t kDepthSlots = 32;
	inline constexpr std::uint32_t kCubeSlots = 4;
	inline constexpr std::uint32_t kVolumeSlots = 8;
	// DepthStencilData has eight DSVs and eight read-only DSVs, one per slice.
	inline constexpr std::uint32_t kMaxDepthSlices = 8;

	/** @brief Which table of the engine's targets. ColourCopy is a colour target's textureCopy and SRVCopy. */
	enum class Kind : std::uint8_t
	{
		Colour,      // RE::RENDER_TARGET
		ColourCopy,  // the same index, its copy texture
		Depth,       // RE::RENDER_TARGETS_DEPTHSTENCIL
		Cubemap,     // RE::RENDER_TARGETS_CUBEMAP
		Volume,      // RE::RENDER_TARGETS_3D
	};

	/** @brief Which field of the target. */
	enum class View : std::uint8_t
	{
		Texture,
		Rtv,
		Srv,
		Uav,
		Dsv,          // a depth target's views[slice]
		ReadOnlyDsv,  // a depth target's readOnlyViews[slice]
		StencilSrv,   // a depth target's stencilSRV
		CubeFace,     // a cubemap's cubeSideRTV[slice]
	};

	/**
	* @brief One engine target, or one of its views. A mip view target (one the engine makes as views of another target's mip) is
	* named by its source's index, with mip set to the mip it views.
	*/
	struct TargetRef
	{
		Kind kind = Kind::Colour;
		std::uint16_t index = 0;
		View view = View::Texture;
		std::uint8_t slice = 0;  // a depth DSV's slice or a cubemap face
		std::uint8_t mip = 0;
		bool operator==(const TargetRef&) const = default;
	};

	constexpr TargetRef Colour(std::uint32_t a_index, View a_view = View::Texture)
	{
		return { Kind::Colour, static_cast<std::uint16_t>(a_index), a_view };
	}

	constexpr TargetRef ColourCopy(std::uint32_t a_index, View a_view = View::Texture)
	{
		return { Kind::ColourCopy, static_cast<std::uint16_t>(a_index), a_view };
	}

	constexpr TargetRef Depth(std::uint32_t a_index, View a_view = View::Texture, std::uint32_t a_slice = 0)
	{
		return { Kind::Depth, static_cast<std::uint16_t>(a_index), a_view, static_cast<std::uint8_t>(a_slice) };
	}

	constexpr TargetRef Cubemap(std::uint32_t a_index, View a_view = View::Texture, std::uint32_t a_face = 0)
	{
		return { Kind::Cubemap, static_cast<std::uint16_t>(a_index), a_view, static_cast<std::uint8_t>(a_face) };
	}

	constexpr TargetRef Volume(std::uint32_t a_index, View a_view = View::Texture)
	{
		return { Kind::Volume, static_cast<std::uint16_t>(a_index), a_view };
	}

	/** @brief One target's properties, as its create function got them. A width of 0 means the engine never made it. */
	struct Shape
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t depthOrSlices = 1;  // a 3D target's depth, a depth target's slices, 6 for a cubemap
		std::uint32_t mips = 1;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		std::uint32_t bindFlags = 0;  // D3D11_BIND_*
		bool copy = false;            // a colour target with a copy texture
		bool cube = false;
		bool volume = false;
		bool operator==(const Shape&) const = default;
	};

	/** @brief Every target's recorded shape, the value of Vars::EngineTargetShapes(). Compared by content. */
	struct ShapeTable
	{
		std::array<Shape, kColourSlots> colours{};
		std::array<Shape, kDepthSlots> depths{};
		std::array<Shape, kCubeSlots> cubes{};
		std::array<Shape, kVolumeSlots> volumes{};
		bool operator==(const ShapeTable&) const = default;
	};

	/**
	* @brief Which engine target a field handle is. Any thread: the map is filled once when the handles are installed and never
	* changes after. Empty for anything that is not a field handle of a target the set made: an engine-owned target, a loaded
	* texture, a DXVK object (pass the handle, never an unwrapped one), or before the handles exist.
	*/
	std::optional<TargetRef> Identify(const ID3D11View* a_view);
	std::optional<TargetRef> Identify(const ID3D11Resource* a_resource);
}
