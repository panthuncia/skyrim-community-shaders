#pragma once

#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <memory>
#include <string_view>

/**
* @brief Dynamic Cubemaps' update pipeline on the render graph, in graph mode.
*
* The D3D11 path spreads its work over a three-frame state machine per cubemap. Here every frame's recording is the same.
* Each dispatch is indirect, and the commit writes this frame's group counts into the latch, with zero for the rest.
* It runs at DeferredPasses, not DeferredComposite. The ORG composite reads the cubemaps through descriptors the graph does not
* track, so writing them in its epoch would race the read.
* A BC6H copy cannot be a dispatch, since there is no UAV of a block-compressed format.
* The client is declared on the engine's reflection cubemap, so a change of it goes through a frame revision. The textures are made
* once for its size, and another size is fatal. In graph mode any failure is fatal. Render thread throughout. Free of ORG/Vulkan
* includes.
*/
class ORGDynamicCubemaps : public RenderGraph::Port<ORGDynamicCubemaps>
{
public:
	static constexpr std::string_view kName = "Dynamic Cubemaps";

	/** @brief The published ids, matching the D3D11 path's names. */
	static constexpr std::string_view kEnvId = "DynamicCubemaps::Env";
	static constexpr std::string_view kEnvReflectionsId = "DynamicCubemaps::EnvReflections";
	static constexpr std::string_view kEnvBC6HId = "DynamicCubemaps::EnvTextureBC6H";
	static constexpr std::string_view kEnvReflectionsBC6HId = "DynamicCubemaps::EnvReflectionsBC6H";

	/**
	* @brief Call at SetupResources with the default cubemap loaded.
	* @return Whether the graph path runs: false outside graph mode. In graph mode a failure is fatal.
	*/
	bool Setup();

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
	* @brief Call after the anchor ran. Notes a new capture colour source for the next revision.
	* @return False outside graph mode, where the D3D11 update runs.
	*/
	bool AfterUpdate();

	/** @brief No cubemap counts as encoded until its next publish. The programs reload with every live pipeline. */
	void ClearShaderCache();
	~ORGDynamicCubemaps();

private:
	friend class RenderGraph::Port<ORGDynamicCubemaps>;
	ORGDynamicCubemaps();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
