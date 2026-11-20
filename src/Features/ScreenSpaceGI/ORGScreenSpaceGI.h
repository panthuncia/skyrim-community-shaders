#pragma once

#include <d3d11.h>
#include <memory>
#include <string_view>

/**
* @brief Screen Space GI on the render graph, with CS_ORG_SCREEN_SPACE_GI=1.
*
* The D3D11 chain as compute passes at the DeferredPasses anchor. Each dispatch goes through its run gate entry, which the
* commit opens with this frame's group counts, so a kept recording follows dynamic resolution.
* Its textures, programs and client follow the frame revisions (frame revision plan 6.6): the textures the next frame reads
* are declared on the display size and the settings that make them exist, one texel while they do not; the programs on the
* define settings; the client on the shape settings, with the transients made at each registration. The composite's inputs
* are four published slots, exported to D3D11, which read zero while SSGI is off. A history reset is a gated clear the commit
* opens: on a loading screen, a new version, or a new configuration or size.
* Failures at setup keep D3D11; later ones are fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGScreenSpaceGI
{
public:
	static bool Requested();

	static ORGScreenSpaceGI& Get();

	/** @brief The composite's inputs (t10-t13), published once at setup. */
	static constexpr std::string_view kAoId = "SSGI::CompositeAO";
	static constexpr std::string_view kYId = "SSGI::CompositeIlY";
	static constexpr std::string_view kCoCgId = "SSGI::CompositeIlCoCg";
	static constexpr std::string_view kSpecularId = "SSGI::CompositeSpecular";

	/** @brief Call in the feature's PostPostLoad, before the graph initializes. */
	static void Reserve();

	/**
	* @brief Call at SetupResources with the runtime active. Declares the textures, programs and client.
	* @return False, logged, keeps SSGI on D3D11.
	*/
	bool Setup(const D3D11_TEXTURE2D_DESC& a_main);

	/** @brief Before the frame's DeferredPasses anchor. Notes new engine inputs, so the next revision makes the client for them. */
	void Prepass();

	/** @brief Set up and the runtime still up. The D3D11 textures are then not made. */
	bool IsActive() const;

	/** @brief The published outputs while the graph path runs, zero while the adopted revision has SSGI off. Nulls otherwise. */
	struct Outputs
	{
		ID3D11ShaderResourceView* ao = nullptr;
		ID3D11ShaderResourceView* y = nullptr;
		ID3D11ShaderResourceView* coCg = nullptr;
		ID3D11ShaderResourceView* specular = nullptr;
	};
	Outputs GetOutputs() const;

	/** @brief Render thread. The adopted revision's EnableExperimentalSpecularGI, which its client was made for. */
	bool AdoptedSpecularGI() const;

	~ORGScreenSpaceGI();

private:
	ORGScreenSpaceGI() = default;

	void Stop();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
