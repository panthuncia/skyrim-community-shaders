#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>

/**
 * @brief Image Based Lighting's SH projection (DiffuseIBLCS) as render-graph passes, behind CS_ORG_IBL=1.
 *
 * Two compute passes at the Prepass anchor project the environment cubemap (Dynamic Cubemaps' EnvTexture, while the DALC mode
 * uses it; on Dynamic Cubemaps' ORG path its published "DynamicCubemaps::Env", by name) and the engine's reflections cubemap
 * (RENDER_TARGETS_CUBEMAP::kREFLECTIONS) into the same 3x1 SH textures the D3D11
 * path writes. The textures are graph twins (GraphResources::Texture2D) exported as D3D11 SRVs: the deferred composite (t14,
 * t15) and the engine's pixel shaders (t76, t77) read those until their draws move to the graph. Exponential Height Fog's passes
 * read the twins by name. They are also published (PublishedResources), and Drawcall Limit Fix's colour passes read them so.
 * The SH is read again in the next frame (the reflection prepass binds the last frame's), so it is History.
 *
 * Fallback: when the program, sampler or textures cannot be made, Setup returns false and IBL keeps its D3D11 path. When the
 * graph fails later, the client is unregistered, the textures are withdrawn and a_onFailed (from Setup) brings the D3D11 path
 * back.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h). Everything here runs on the render thread.
 */
class ORGIBL
{
public:
	static ORGIBL& Get();

	/** @brief Whether CS_ORG_IBL=1 asks for the render-graph path. */
	static bool Requested();

	/** @brief Before the render graph initializes (IBL's PostPostLoad): reserves the Prepass anchor when Requested. */
	static void Reserve();

	/**
	 * @brief At SetupResources, with the runtime active: makes the program, sampler and SH textures and registers the anchor client.
	 * @param a_onFailed Called once if the graph fails later; IBL returns to D3D11.
	 * @return Whether the SH runs on the render graph; false keeps IBL on D3D11.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether the SH runs on the render graph. */
	bool IsActive() const;

	/**
	 * @brief IBL's Prepass, before the Prepass anchor runs: imports this frame's source cubemaps and rebuilds the passes when they
	 * or the environment's use (a_useEnvironment: the DALC mode reads it) changed.
	 * @return Whether the anchor computes the SH this frame; false means the D3D11 dispatches run.
	 */
	bool Prepass(bool a_useEnvironment);

	/** @brief The exported SH textures' D3D11 views; null when inactive. */
	ID3D11ShaderResourceView* EnvSRV() const;
	ID3D11ShaderResourceView* SkySRV() const;

	/**
	 * @brief Debug readback (CS_ORG_IBL_READBACK=1): every 250 frames, copies the SH textures into staging textures and logs the
	 * values a few frames later. Uses the exported textures while active, else a_env and a_sky (IBL's D3D11 textures). Call at the
	 * start of IBL's Prepass on both paths, so both read the same frame's result.
	 */
	void DebugReadback(ID3D11Texture2D* a_env, ID3D11Texture2D* a_sky);

	~ORGIBL();

private:
	ORGIBL() = default;

	/** @brief The exported SH texture (environment or sky); null when inactive. */
	ID3D11Texture2D* Exported(bool a_env) const;

	struct Impl;
	std::unique_ptr<Impl> impl;

	struct Readback;
	std::unique_ptr<Readback> readback;
};
