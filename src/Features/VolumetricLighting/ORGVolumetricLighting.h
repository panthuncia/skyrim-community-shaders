#pragma once

#include <memory>
#include <string_view>

/**
 * @brief Volumetric Lighting's GPU work on the render graph (CS_ORG_VOLUMETRIC_LIGHTING=1, docs/development/p5/volumetric-lighting.md).
 *
 * The engine's CPU side runs as before: the VL render (SE 100306, AE 107023) builds the frame's constants and binds its textures,
 * and RenderWorldPostProcess calls the apply (SE 100475, AE 107193) and the blur (SE 100486, AE 107206). None of their GPU work
 * reaches D3D11 on this path:
 * - inside the VL render, every DispatchCSShader (the density generation, the raymarch, the noise on the first call) is withheld
 *   (Hooks.cpp's DispatchCSShader seam asks TakeEngineDispatch), and the engine's PerTechnique block is latched after it returns;
 * - the apply and the blur are detoured whole: their values are latched, and the blur's call runs the VolumetricLighting anchor.
 *
 * That run holds every pass: the noise volume (once a session), the generate and the raymarch into a graph volume, the apply into
 * the engine's VL target, the two blurs, and the main depth's copy into kMAIN_COPY that the apply makes on D3D11. Each pass but
 * the copy draws or dispatches through a run gate entry its commit opens. Inputs: the sun's cascades (kSHADOWMAPS_ESRAM and
 * kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM, whole-array views), Terrain and Cloud Shadows' textures (their published ids on their ORG
 * paths, else their D3D11 views imported), the engine's targets and depths, and the sky's noise gradient texture (imported).
 *
 * Nothing falls back to the engine at runtime: a frame the graph cannot run is skipped, logged once per reason. A failed setup or
 * a failed graph leaves the D3D11 path (the detours pass through).
 *
 * Render thread throughout. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGVolumetricLighting
{
public:
	/** @brief Whether CS_ORG_VOLUMETRIC_LIGHTING=1 is set. */
	static bool Requested();

	static ORGVolumetricLighting& Get();

	/** @brief The VolumetricLighting anchor, when requested. Before the render graph initializes (the feature's PostPostLoad). */
	static void Reserve();

	/** @brief The engine's VL render, apply and blur detoured, when requested (the feature's PostPostLoad). */
	static void Install();

	/**
	 * @brief At the feature's SetupResources, with the runtime active: the programs, the latch, the run gate and the noise volume,
	 * and the client registered with a volume for the exterior and interior sizes. An input missing now (the noise gradient
	 * texture before the sky loads it) registers it at the first VL render instead.
	 * @return Whether the GPU work runs on the render graph; false keeps the D3D11 path.
	 */
	bool Setup();

	/** @brief Whether the ORG path is set up and the runtime is up: the detours take the engine's work. */
	bool IsActive() const;

	/**
	 * @brief Hooks.cpp's DispatchCSShader seam: true when the dispatch is the VL render's on the ORG path, which the graph does
	 * instead (the caller skips it). a_computeShader is the BSComputeShader's name, for the log.
	 */
	bool TakeEngineDispatch(std::string_view a_computeShader);

	~ORGVolumetricLighting();

private:
	friend struct ORGVolumetricLightingDetours;
	struct Impl;

	ORGVolumetricLighting() = default;

	std::unique_ptr<Impl> impl;
};
