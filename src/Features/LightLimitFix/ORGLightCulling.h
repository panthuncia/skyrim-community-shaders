#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string_view>

/**
* @brief Light Limit Fix's clustered light culling as OpenRenderGraph passes.
*
* Runs LLF's own shaders, built with LLF_ORG_BINDLESS, and produces the same light lists the D3D11 path does.
* The buffers are Persistent twins exported to D3D11 at t35-t37 and published by id. The ids are withdrawn when the graph fails.
* The culling runs in its own epoch through a host extension, not at an anchor, so Execute reports failure and LLF checks IsActive every frame.
* Free of ORG/Vulkan includes.
*/
class ORGLightCulling
{
public:
	/** @brief The published ids of the lights (t35), the light index list (t36) and the light grid (t37). */
	static constexpr std::string_view kLightsId = "LightLimitFix::Lights";
	static constexpr std::string_view kLightIndexListId = "LightLimitFix::LightIndexList";
	static constexpr std::string_view kLightGridId = "LightLimitFix::LightGrid";

	/** @brief Copied at Execute. Nothing is retained by reference. */
	struct FrameInputs
	{
		const void* lights = nullptr;
		uint32_t lightCount = 0;
		uint32_t clusterSize[3]{};  // the adopted revision's grid, at most the capacity given to Setup
		float lightsNear = 0.0f;
		float lightsFar = 0.0f;
		std::array<float, 16> cameraProjInverse{};  // row-major, as in FrameBuffer
		std::array<float, 16> cameraView{};         // row-major, as in FrameBuffer
	};

	static ORGLightCulling& Get();

	/**
	* @brief a_clusterCapacity is the largest cluster grid the buffers hold, the grid of the display size. The passes dispatch for it,
	* and each run culls the grid its FrameInputs carry. @return False keeps LLF on D3D11.
	*/
	bool Setup(const std::array<uint32_t, 3>& a_clusterCapacity, uint32_t a_maxLights, uint32_t a_lightStride, uint32_t a_maxLightsPerCluster);

	bool Execute(const FrameInputs& a_inputs);

	bool IsActive() const;

	/**
	* @brief Render thread. The cluster grid of the adopted revision: its display size times its dynamic ratios, 64 pixels a
	* cluster, at most the capacity given to Setup. The culling and every reader of the frame use it. @return False when inactive.
	*/
	bool AdoptedClusterGrid(std::array<uint32_t, 3>& a_grid) const;

	/** @brief Null when inactive. */
	ID3D11ShaderResourceView* GetLightsSRV() const;
	ID3D11ShaderResourceView* GetLightIndexListSRV() const;
	ID3D11ShaderResourceView* GetLightGridSRV() const;

	/** @brief Heap indices of t35, t36 and t37 for graph passes. False when inactive. */
	bool GetShaderResourceIndices(uint32_t& a_lights, uint32_t& a_lightIndexList, uint32_t& a_lightGrid) const;

	~ORGLightCulling();

private:
	ORGLightCulling() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
