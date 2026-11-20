#pragma once

// Per-frame resources that every DCLF draw reads at a fixed register, named by the frame record because they change each frame.
// Frame bindings are a feature's versioned buffers. Frame textures are render targets or CS features' per-frame textures.
// Frame graph buffers are other CS features' render graph buffers. Per-draw buffers are other features' constant buffers bound per draw.
// Registers are compile-time constants. Features/Manifest.cpp and the install both check them for clashes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/FrameBindings.h.
	class FrameBindingSelect;

	/**
	 * @brief A feature buffer every DCLF draw reads at a texture register, with the version chosen per frame.
	 * select may alternate between frames, so the address parity accepts either of two consecutive frames.
	 * It must read only handles made at scene setup. A null answer leaves the register as the record set it.
	 */
	struct FrameBinding
	{
		std::string_view name;  // name used by the named-address parity
		std::uint32_t textureRegister = ~0u;
		void (*select)(FrameBindingSelect& a_select) = nullptr;
	};

	/** @brief Most published graph ids one frame texture may list. */
	inline constexpr std::size_t kFrameTextureGraphIds = 2;

	/**
	 * @brief A view every DCLF colour draw reads at a texture register, such as a render target or a CS feature's per-frame texture.
	 *
	 * - view: render thread, per colour commit and per epoch of a view with its own frame constants. Null means the null texture.
	 *   With no view callback at all, the registration only lets the graph path take the register.
	 * - capturedWhenNull: a null view keeps whatever the colour commit captured at that register.
	 *   Used when the texture is only DCLF's to name while the feature binds it itself.
	 * - graphResources: ids a producer ported to ORG publishes the texture under. See dclf-architecture.md, "Frame graph textures".
	 *   A commit reads the graph resource when the resolved view is that id's export, and imports the view otherwise.
	 * - facesGraphResources: per-epoch views. Ids the reflection faces' epochs read at this register in place of the main view's
	 *   texture, such as a reflection medium. Bound: the faces' graphs declare them and the faces read the first one published by
	 *   id, with no view callback, so the faces' view changes only with a frame revision. Until one is on the graph the faces read
	 *   the null texture. Empty: the faces read what the main view reads.
	 */
	struct FrameTexture
	{
		std::string_view name;
		std::uint32_t textureRegister = ~0u;
		void* (*view)() = nullptr;
		bool capturedWhenNull = false;
		std::array<std::string_view, kFrameTextureGraphIds> graphResources{};
		std::array<std::string_view, kFrameTextureGraphIds> facesGraphResources{};
	};

	/**
	 * @brief Another CS feature's render graph buffers, read at consecutive texture registers from firstRegister.
	 *
	 * - resources: one id per register. Only the leading non-empty ones count.
	 * - indices: render thread. Writes each resource's bindless index and returns whether they are all on the graph.
	 *   On false the registers keep what the commit captured.
	 */
	struct FrameGraphBuffers
	{
		std::string_view name;
		std::uint32_t firstRegister = ~0u;
		std::array<std::string_view, 4> resources{};
		bool (*indices)(std::span<std::uint32_t> a_out) = nullptr;
	};

	/** @brief A shader stage a per-draw buffer is bound to. */
	enum class ShaderStage : std::uint8_t
	{
		Vertex,
		Pixel,
	};

	/**
	 * @brief A constant buffer another CS feature binds per draw. What the main pass has bound there belongs to the last drawn object.
	 * So frame blocks never mirror it and capture checks skip it. Declaration only, nothing is called.
	 */
	struct PerDrawBuffer
	{
		std::string_view name;
		ShaderStage stage = ShaderStage::Pixel;
		std::uint32_t constantRegister = ~0u;  // below D3D11's 14 constant buffer slots
	};
}
