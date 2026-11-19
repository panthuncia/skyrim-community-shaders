#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief Screen Space GI on the render graph, with CS_ORG_SCREEN_SPACE_GI=1.
*
* The D3D11 chain as compute passes at the DeferredPasses anchor. Each dispatch goes through its run gate entry, which the
* commit opens with this frame's group counts, so a kept recording follows dynamic resolution.
* Each D3D11 ping-pong pair is split by lifetime: the texture the next frame reads is History, the scratch is Transient.
* The composite's inputs are exported to D3D11, so they are not aliased while D3D11 reads them.
* A history reset clears the History textures through one-shot passes of a second client.
* On any failure the feature returns to D3D11. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGScreenSpaceGI
{
public:
	static bool Requested();

	static ORGScreenSpaceGI& Get();

	/** @brief The composite's inputs (t10-t13), published while the graph path runs and SSGI is on. The specular id only with specular textures. */
	static constexpr std::string_view kAoId = "SSGI::CompositeAO";
	static constexpr std::string_view kYId = "SSGI::CompositeIlY";
	static constexpr std::string_view kCoCgId = "SSGI::CompositeIlCoCg";
	static constexpr std::string_view kSpecularId = "SSGI::CompositeSpecular";

	/** @brief Call in the feature's PostPostLoad, before the graph initializes. */
	static void Reserve();

	/**
	* @brief Call at SetupResources with the runtime active. The anchor client registers at the first Prepass.
	* @param a_onFailed Called once if the graph path stops later.
	* @return False, logged, keeps SSGI on D3D11.
	*/
	bool Setup(const D3D11_TEXTURE2D_DESC& a_main, std::function<void()> a_onFailed);

	/** @brief Before the frame's DeferredPasses anchor. Rebuilds and re-registers after changes. Any failure returns to D3D11. */
	void Prepass();

	/** @brief Set up, not failed, and the runtime still up. The D3D11 textures are then not made. */
	bool IsActive() const;

	/** @brief Nulls unless the graph path runs and SSGI is on. */
	struct Outputs
	{
		ID3D11ShaderResourceView* ao = nullptr;
		ID3D11ShaderResourceView* y = nullptr;
		ID3D11ShaderResourceView* coCg = nullptr;
		ID3D11ShaderResourceView* specular = nullptr;
	};
	Outputs GetOutputs() const;

	~ORGScreenSpaceGI();

private:
	ORGScreenSpaceGI() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
