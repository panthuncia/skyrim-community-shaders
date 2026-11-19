#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

/**
* @brief Dynamic Cubemaps' update pipeline on the render graph, with CS_ORG_DYNAMIC_CUBEMAPS=1.
*
* The D3D11 path spreads its work over a three-frame state machine per cubemap. Here every frame's recording is the same.
* Each dispatch is indirect, and the commit writes this frame's group counts into the latch, with zero for the rest.
* It runs at DeferredPasses, not DeferredComposite. The ORG composite reads the cubemaps through descriptors the graph does not
* track, so writing them in its epoch would race the read.
* A BC6H copy cannot be a dispatch, since there is no UAV of a block-compressed format.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGDynamicCubemaps
{
public:
	/** @brief The published ids, matching the D3D11 path's names. */
	static constexpr std::string_view kEnvId = "DynamicCubemaps::Env";
	static constexpr std::string_view kEnvReflectionsId = "DynamicCubemaps::EnvReflections";
	static constexpr std::string_view kEnvBC6HId = "DynamicCubemaps::EnvTextureBC6H";
	static constexpr std::string_view kEnvReflectionsBC6HId = "DynamicCubemaps::EnvReflectionsBC6H";

	static ORGDynamicCubemaps& Get();

	static bool Requested();

	/** @brief Call in the feature's PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/**
	* @brief Call at SetupResources with the runtime active and the default cubemap loaded.
	* @param a_onFailed Called once if the path stops later. The feature must drop its views of the outputs.
	* @return False keeps the D3D11 path.
	*/
	bool Setup(std::function<void()> a_onFailed);

	bool IsActive() const;

	/** @brief R11G11B10 with every mip, and their BC6H copies. Null when inactive. */
	struct Outputs
	{
		ID3D11Texture2D* env = nullptr;
		ID3D11ShaderResourceView* envSRV = nullptr;
		ID3D11Texture2D* envReflections = nullptr;
		ID3D11ShaderResourceView* envReflectionsSRV = nullptr;
		ID3D11Texture2D* bc6h = nullptr;
		ID3D11ShaderResourceView* bc6hSRV = nullptr;
		ID3D11Texture2D* bc6hReflections = nullptr;
		ID3D11ShaderResourceView* bc6hReflectionsSRV = nullptr;
	};
	Outputs Exported() const;

	/**
	* @brief Call after the anchor ran. Follows shader reloads, new engine targets and first encodes, each of which rebuilds the graph.
	* @return False means the D3D11 update runs.
	*/
	bool AfterUpdate();

	/** @brief Programs rebuild at the next AfterUpdate. No cubemap counts as encoded until its next publish. */
	void ClearShaderCache();

	~ORGDynamicCubemaps();

private:
	ORGDynamicCubemaps() = default;

	/** @brief For good. Unregisters, withdraws outputs, returns to D3D11, then releases the resources. */
	void Fail(const char* a_reason);

	/** @brief Graph readers return to their D3D11 views. */
	void Withdraw();

	/** @brief False, logged by FrameAnchors, when it cannot be. */
	bool RegisterClient();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
