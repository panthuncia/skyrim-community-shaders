#pragma once

// SetupGeometry's per-pipeline and per-frame PerGeometry values: PipelineFrame, sampled by the render thread at Prepass,
// and the pure functions that write a pipeline's block from it. Per-object values live in LightingConstants.

#include <array>
#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"

namespace RE
{
	class BSLight;
}

namespace DCLF::GeometryPort
{
	/**
	 * @brief The render flags the native main pass hands SetupGeometry: 0x41, or 0x45 for blended decals.
	 * SetupGeometry only reads bits 0x2, 0x8 and 0x10, which the main pass never sets, so 0x41 stands for both.
	 */
	inline constexpr std::uint32_t kMainPassRenderFlags = 0x41;

	/**
	 * @brief The per-frame inputs of a pipeline's PerGeometry block, as BSLightingShader::SetupGeometry (AE 0x1414dd040) would compute them now.
	 * Sampled once per frame at Prepass. After that nothing of the engine is read.
	 * Assumes Community Shaders' world-space patch at AE SetupGeometry+0x71, so the model-space branches never run.
	 */
	struct PipelineFrame
	{
		// ShadowSceneNode[0]->sunLight. Without it nothing below is meaningful.
		bool sun = false;
		// PS DirLightDirection (3): D3DXVec3Normalize(-NiDirectionalLight+0x140), through the engine's own import (0x1417502e0).
		float dirLightDirection[3]{};
		// PS DirLightColor (4): NiLight diffuse (+0x11c) x (NiLight fade +0x134 x ImageSpaceManager (*0x14332a6a0) +0xe0).
		float dirLightColor[3]{};
		// PS DirectionalAmbient (5): BSShaderManager::State+0xc8 (0x142033128), a 3x3 then a column. Stored as the 3x3's rows with the column in w.
		float directionalAmbient[12]{};
		// VS EyePosition (2), xyz: the current accumulator's eye (+0x16c) less posAdjust (shadow state +0x35c). eye is false without an accumulator.
		bool eye = false;
		float eyePosition[3]{};
		// PS SSRParams (16), xyz: the settings at 0x142034e80 and 0x142034e98, and the auxiliary-target switch 0x14338ca08.
		// The switch is 1 only inside the post-resolve-depth passes FUN_1414b3390 and FUN_1414b3f20.
		float ssrParams[3]{};
		// VS WorldMapOverlayParameters (8) and PS WorldMapOverlayParametersPS (17) for WorldMap passes: 0x14332a4f0 and 0x14332a500.
		float worldMapVS[4]{};
		float worldMapPS[4]{};
		// PS AmbientSpecularTintAndFresnelPower (6) for AmbientSpecular passes, which include every True PBR pass.
		// Vanilla SetupGeometry does not write it. SSE Engine Fixes patches it in around 0x1414ddb5c, copying BSShaderManager::State+0xfc.
		// ambientSpecularFix is set when that patch is present.
		bool ambientSpecularFix = false;
		float ambientSpecular[4]{};

		/** @brief Whether two frames give every pipeline the same block in what a bindless draw reads. If so, no pipeline's posted constants change. */
		bool SamePipelineInputs(const PipelineFrame& a_other) const;
	};

	/**
	 * @brief Render thread, in the engine-read window. Sampled at Prepass, not the frame's start, because only there are the accumulator's eye
	 * and posAdjust the main camera's. Earlier they still belong to the shadow-map camera.
	 */
	PipelineFrame SamplePipelineFrame();
	/**
	 * @brief The same for another scene's sun, such as a menu scene node's. With a_eyeZero, EyePosition is zero: the camera that set
	 * posAdjust is the scene's own, so its eye less posAdjust is zero. a_hdr, when given, is the HDR multiplier DirLightColor scales by
	 * in place of the ImageSpace manager's current one.
	 */
	PipelineFrame SamplePipelineFrame(const RE::BSLight* a_sun, bool a_eyeZero, const float* a_hdr = nullptr);

	/**
	 * @brief What SetupGeometry on a pipeline's template pass, the sun alone, writes into its PerGeometry block. kUnwrittenBits mark the rest.
	 * Per-object variables are left unwritten rather than taken from a template object. a_renderFlags is kept only for the call sites.
	 * Returns false without a sun, since the template pass would not exist.
	 */
	bool PipelineGeometryConstants(std::uint32_t a_passDescriptor, std::uint32_t a_renderFlags, const PipelineFrame& a_frame, GeometryConstants& a_out);

	/**
	 * @brief Writes in place the parts of a pipeline's block that move every frame: the sun lighting, AmbientSpecular where patched, and EyePosition.
	 * Everything else only moves with SamePipelineInputs.
	 */
	void RefreshFrameValues(std::uint32_t a_passDescriptor, const PipelineFrame& a_frame, GeometryConstants& a_inOut);

	/**
	 * @brief Whether SetupGeometry (0x1414dd040) writes VS EyePosition, by its rule on the raw technique with the pass rules applied.
	 * LightingConstants.h's WritesEyePosition reads the pass descriptor instead, so it misses TruePbr passes.
	 */
	bool PassWritesEyePosition(std::uint32_t a_passDescriptor);

	/** @brief MergeFrameLighting's contract, but taken from the frame instead of an evaluation. Fills only floats a_written lacks. */
	void MergeFrameLighting(const PipelineFrame& a_frame, std::array<float, 24>& a_out, std::uint32_t& a_written);

	// The PerGeometry variables left to the objects: World, PreviousWorld, LandBlendParams, TreeParams, WindTimers, TextureProj,
	// MaterialData, EmitColor and ProjectedUVParams 1-3. Of SSRParams only w is the object's.
	inline constexpr std::uint64_t kObjectVS = (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6);
	inline constexpr std::uint64_t kObjectPS = (1ull << 7) | (1ull << 8) | (1ull << 12) | (1ull << 13) | (1ull << 14);
}
