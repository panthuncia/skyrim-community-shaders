#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief Screen Space Shadows on the render graph, with CS_ORG_SCREEN_SPACE_SHADOWS=1.
*
* At the Prepass anchor, a clear to white then one raymarch pass. Bend's dispatch list changes every frame but the recording
* is kept, so the pass records the list's full capacity. Each dispatch reads constants from the latch and counts from the gate,
* and unused entries stay closed. The output is exported for the engine's t45 and published for DCLF.
* On any failure the feature returns to D3D11. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGScreenSpaceShadows
{
public:
	/** @brief The shadow texture's published id. */
	static constexpr std::string_view kShadowTextureId = "ScreenSpaceShadows::ShadowTexture";

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGScreenSpaceShadows& Get();

	/** @brief At SetupResources with the runtime active. @return False keeps the D3D11 path. */
	bool Setup(const D3D11_TEXTURE2D_DESC& a_desc);

	/** @brief The client is registered and the runtime is still up. */
	bool IsActive() const;

	/** @brief At the feature's Prepass. @return The SRV for the engine's t45, or null on the D3D11 path. */
	ID3D11ShaderResourceView* Prepass();

	/** @brief Null on the D3D11 path. */
	ID3D11ShaderResourceView* ShadowSRV() const;

	~ORGScreenSpaceShadows();

private:
	ORGScreenSpaceShadows() = default;

	void Stop();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
