#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief TruePBR's glints noise on the render graph, with CS_ORG_GLINTS=1.
*
* The 128x128 noise never changes, so a one-shot pass at EarlyPrepass fills it once after Setup. That anchor runs before every reader.
* The texture is persistent, exported for the engine at t20 and published for DCLF. TruePBR binds nothing at t20 until the fill ran.
* On any failure it stops for good. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGGlintsNoise
{
public:
	/** @brief The noise texture's published id. */
	static constexpr std::string_view kNoiseId = "TruePBR::GlintsNoise";

	static bool Requested();

	/** @brief Call in PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGGlintsNoise& Get();

	/** @brief At TruePBR::PostSetupResources when requested. @return False keeps the D3D11 path. */
	bool Setup();

	/** @brief The client is registered and the runtime is still up. */
	bool Active() const;

	/** @brief A run with the fill was submitted, so later work sees it filled. */
	bool Filled() const;

	/** @brief t20. Null when inactive or not filled yet. */
	ID3D11ShaderResourceView* NoiseSRV() const;

	/** @brief Rebuilds the fill program and fills again. After a failed compile, sets the path up again. */
	void ClearShaderCache();

	~ORGGlintsNoise();

	struct Impl;

private:
	ORGGlintsNoise() = default;

	/** @brief Replacing the client makes the fill pending again. False on failure. */
	bool RegisterClient(const std::shared_ptr<Impl>& a_impl);

	/** @brief For good. */
	void Fail();

	std::shared_ptr<Impl> impl;
	bool failed = false;
	bool programFailed = false;  // a shader reload may fix a failed compile
};
