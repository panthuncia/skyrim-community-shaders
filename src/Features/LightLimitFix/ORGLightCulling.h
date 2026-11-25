#pragma once

#include "RenderGraph/Port.h"

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string_view>

/**
* @brief Light Limit Fix's clustered light culling as OpenRenderGraph passes, in graph mode.
*
* Runs LLF's own shaders, built with LLF_ORG_BINDLESS, and produces the same light lists the D3D11 path does.
* The buffers are Persistent twins exported to D3D11 at t35-t37 and published by id.
* The cluster buffers are declared on the display size (Frame::DeclareBuffer), so a new display size is a new version of each.
* The culling is a declared client at the Prepass anchor (the pre-main stage's Prepass step), so it is a member of the frame
* revisions: the first frame of a revision with a new display size binds the new versions. A failed setup or tap is fatal.
* Free of ORG/Vulkan includes.
*/
class ORGLightCulling : public RenderGraph::Port<ORGLightCulling>
{
public:
	static constexpr std::string_view kName = "Light culling";

	/** @brief The published ids of the lights (t35), the light index list (t36) and the light grid (t37). */
	static constexpr std::string_view kLightsId = "LightLimitFix::Lights";
	static constexpr std::string_view kLightIndexListId = "LightLimitFix::LightIndexList";
	static constexpr std::string_view kLightGridId = "LightLimitFix::LightGrid";

	/** @brief Copied at Execute. Nothing is retained by reference. */
	struct FrameInputs
	{
		const void* lights = nullptr;
		uint32_t lightCount = 0;
		uint32_t clusterSize[3]{};  // the adopted revision's grid, at most the bound buffers' grid
		float lightsNear = 0.0f;
		float lightsFar = 0.0f;
		std::array<float, 16> cameraProjInverse{};  // row-major, as in FrameBuffer
		std::array<float, 16> cameraView{};         // row-major, as in FrameBuffer
	};

	/**
	* @brief a_clusterCapacity is the display size's grid at setup, used while the display size variable is zero. The buffers hold
	* the adopted display size's grid. The passes dispatch for it, and each run culls the grid its FrameInputs carry.
	* In graph mode a failure is fatal. @return True when the culling runs on the graph. False outside graph mode, where LLF uses D3D11.
	*/
	bool Setup(const std::array<uint32_t, 3>& a_clusterCapacity, uint32_t a_maxLights, uint32_t a_lightStride, uint32_t a_maxLightsPerCluster);

	/**
	* @brief Render thread, in Light Limit Fix's Prepass, before the Prepass anchor runs. Copies the frame's lights and camera.
	* The anchor's commit writes them into its run, so the culling runs at that tap. @return False when not set up.
	*/
	bool SetFrame(const FrameInputs& a_inputs);

	/**
	* @brief Render thread. The cluster grid of the adopted revision: its display size times its dynamic ratios, 64 pixels a
	* cluster, at most the bound buffers' grid. The culling and every reader of the frame use it. @return False when inactive.
	*/
	bool AdoptedClusterGrid(std::array<uint32_t, 3>& a_grid) const;

	/** @brief Null when inactive. */
	ID3D11ShaderResourceView* GetLightsSRV() const;
	ID3D11ShaderResourceView* GetLightIndexListSRV() const;
	ID3D11ShaderResourceView* GetLightGridSRV() const;

	~ORGLightCulling();

private:
	friend class RenderGraph::Port<ORGLightCulling>;
	ORGLightCulling();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
