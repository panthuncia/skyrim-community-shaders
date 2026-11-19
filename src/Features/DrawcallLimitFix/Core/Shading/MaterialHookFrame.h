#pragma once

#include <array>
#include <cstdint>

struct ID3D11ShaderResourceView;

namespace RE
{
	class NiSourceTexture;
}

namespace DCLF::MaterialPort
{
	/** @brief Render target views HookFrame keeps. RE::RENDER_TARGETS::kTOTAL must fit, which the .cpp checks. */
	inline constexpr std::uint32_t kHookRenderTargets = 128;

	/**
	 * @brief The per-frame engine sources Community Shaders' SetupMaterial hooks read, sampled on the render thread at the frame's start.
	 * A contributor's own frame state is in its sample, not here.
	 * Core/Extensions/Materials.h forward-declares this by name, so it must stay a struct, never an alias.
	 */
	struct HookFrame
	{
		// SetupTechniqueDescriptor's bytes, which give the +0x94 technique TruePBR's hook branches on.
		std::uint8_t techniqueByte12 = 0;
		std::uint8_t techniqueByte7 = 0;

		// TruePBR's TexcoordOffset index, textureTransformCurrentBuffer, unmasked as the hook uses it.
		std::uint32_t textureTransformBuffer = 0;

		// TruePBR's character light: the image space texture's render target, -1 for none, and smState's params.
		std::int32_t characterLightTarget = -1;
		bool characterLightEnabled = false;
		std::array<float, 4> characterLightParams{};

		// Renderer render target views.
		std::array<ID3D11ShaderResourceView*, kHookRenderTargets> renderTargetViews{};

		// BSGraphics::State's default textures: their NiSourceTexture pointers, compared with material fields, and their views.
		const void* defaultBlack = nullptr;
		const void* defaultWhite = nullptr;
		const void* defaultNormalMap = nullptr;
		ID3D11ShaderResourceView* defaultBlackView = nullptr;
		ID3D11ShaderResourceView* defaultWhiteView = nullptr;
		ID3D11ShaderResourceView* defaultNormalMapView = nullptr;
	};

	/** @brief Render thread, at the frame's start. */
	HookFrame SampleHookFrame();

	/** @brief Render target a_index's view in a_frame, or null. */
	inline ID3D11ShaderResourceView* RenderTargetView(const HookFrame& a_frame, std::int32_t a_index)
	{
		// The engine indexes renderTargets unchecked. An index past the array has no defined view.
		return a_index >= 0 && static_cast<std::uint32_t>(a_index) < kHookRenderTargets ? a_frame.renderTargetViews[a_index] : nullptr;
	}

	/** @brief A texture's view as the hooks resolve it, or null. */
	ID3D11ShaderResourceView* ViewOfTexture(const RE::NiSourceTexture* a_texture);
}
