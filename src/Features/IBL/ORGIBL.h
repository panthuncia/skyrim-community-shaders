#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief Image Based Lighting's SH projection as compute passes at Prepass, with CS_ORG_IBL=1.
*
* Projects the environment cubemap and the engine's reflections cubemap into the same 3x1 SH textures the D3D11 path writes.
* The SH textures are History, since the reflection prepass reads last frame's. They are exported for D3D11 readers and published.
* A missing source clears its SH to zero, as a null view projects. The client is declared on the reflection cubemap and the DALC
* mode (Frame/Declare.h), so a change of either switches in with a frame revision. If the runtime stops later, IBL returns to D3D11.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGIBL
{
public:
	/** @brief The SH textures' published ids. */
	static constexpr std::string_view kEnvId = "IBL::EnvIBL";
	static constexpr std::string_view kSkyId = "IBL::SkyIBL";

	static ORGIBL& Get();

	static bool Requested();

	/** @brief IBL's PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/**
	* @brief At SetupResources with the runtime active.
	* @param a_onFailed Called once if the runtime stops later.
	* @return False keeps IBL on D3D11.
	*/
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Set up, and the runtime still up. */
	bool IsActive() const;

	/** @brief Before the Prepass anchor. The adopted revision's client projects the SH. @return False means the D3D11 dispatches run. */
	bool Prepass();

	/** @brief Null when inactive. */
	ID3D11ShaderResourceView* EnvSRV() const;
	ID3D11ShaderResourceView* SkySRV() const;

	~ORGIBL();

private:
	ORGIBL() = default;

	/** @brief The runtime went down: a_onFailed brings the D3D11 path back. */
	void Fail(const char* a_reason);

	struct Impl;
	std::unique_ptr<Impl> impl;
};
