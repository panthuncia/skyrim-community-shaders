#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>

/**
 * @brief Dynamic Cubemaps' update pipeline on the render graph (CS_ORG_DYNAMIC_CUBEMAPS=1).
 *
 * The D3D11 path spreads its work over a state machine of three frames per cubemap (DynamicCubemaps::NextUpdateStep): capture,
 * inference and the first irradiance mip; the middle irradiance mips; the last mip, BC6H encoding and publishing. Here the same work
 * is compute and copy passes at the DeferredPasses anchor, after Screen Space GI's and Subsurface Scattering's passes. Every frame's
 * recording is the same: each dispatch is indirect, and the client's commit runs the state machine and writes the group counts of
 * the work this frame does (zero for the rest) into the frame's latch region, with the capture constants. Mips are ORG's GenerateMips
 * (GraphPasses::MakeGenerateMips), gated the same way.
 *
 * Not DeferredComposite, where the D3D11 path runs: the ORG composite (CS_ORG_COMPOSITE) reads the environment cubemaps through
 * descriptors the graph does not track, so writing them in the composite's own epoch would race its read. One epoch earlier, the
 * only D3D11 work skipped over is Screen Space GI (which writes no colour) and Subsurface Scattering's skin blur of kMAIN.
 *
 * Outputs, exported to D3D11 while D3D11 readers remain, and published under their names (PublishedResources) so graph readers can
 * name them: the environment cubemaps ("DynamicCubemaps::Env", "DynamicCubemaps::EnvReflections": the composite, IBL) and their
 * BC6H copies ("DynamicCubemaps::EnvTextureBC6H", "DynamicCubemaps::EnvReflectionsBC6H": the engine's pixel shaders at t30 and t31,
 * Drawcall Limit Fix). The BC6H copy cannot be a dispatch (no UAV of a block-compressed format), so it runs every frame from each
 * cubemap's persistent encoder output, from the frame after that cubemap's first encode on.
 *
 * Fallback: when anything cannot be made, Setup returns false and the feature keeps its D3D11 path. When the graph fails later, the
 * client is unregistered and a_onFailed (from Setup) brings the D3D11 path back, before the exported textures are released.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h). Everything here runs on the render thread.
 */
class ORGDynamicCubemaps
{
public:
	static ORGDynamicCubemaps& Get();

	/** @brief Whether CS_ORG_DYNAMIC_CUBEMAPS=1 asks for the render-graph path. */
	static bool Requested();

	/** @brief Before the render graph initializes (the feature's PostPostLoad): reserves the DeferredPasses anchor when Requested. */
	static void Reserve();

	/**
	 * @brief At SetupResources, with the runtime active and the feature's default cubemap loaded: makes the programs, the graph
	 * textures and the latch, and publishes the outputs. The anchor client registers at the first AfterUpdate with the engine
	 * targets and the shared constants there.
	 * @param a_onFailed Called once if the graph fails later: the feature returns to D3D11 (it must drop its views of the outputs).
	 * @return Whether the update runs on the render graph; false keeps the D3D11 path.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether the update runs on the render graph. */
	bool IsActive() const;

	/** @brief The exported outputs: the environment cubemaps (R11G11B10, every mip) and their BC6H copies. Null when inactive. */
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
	 * @brief After the anchor ran (the feature's UpdateCubemap): registers the client once its inputs are there (and gives up after
	 * a while), rebuilds the programs after ClearShaderCache, follows a new engine target or reflections cubemap, and adds a cubemap's
	 * BC6H copy once it was first encoded. Each rebuilds the anchor's graph at its next execution.
	 */
	void AfterUpdate();

	/** @brief The programs are built again at the next AfterUpdate, and no cubemap counts as encoded until its next publish. */
	void ClearShaderCache();

	~ORGDynamicCubemaps();

private:
	ORGDynamicCubemaps() = default;

	/**
	 * @brief Leaves the graph path for good: the client unregistered (a_unregister; the graph's failure did that already), the outputs
	 * withdrawn, the feature back on D3D11, then the resources released.
	 */
	void Fail(const char* a_reason, bool a_unregister);

	/** @brief Withdraws the published outputs: graph readers return to their D3D11 views. */
	void Withdraw();

	/** @brief Registers the anchor client: its passes, resources and commit. False, logged by FrameAnchors, when it cannot be. */
	bool RegisterClient();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
