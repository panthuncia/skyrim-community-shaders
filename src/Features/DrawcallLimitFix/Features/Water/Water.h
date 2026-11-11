#pragma once

// Water (CS_ORG_WATER, docs/development/p5/water.md): the engine's BSWaterShader draws of the main camera drawn by the render graph in
// the engine's place: the water mask (the Stencil technique, which writes the water's normal mask, its motion vectors and its per-type
// stencil bits) and the water itself (the surface, underwater, LOD and specular light techniques).
//
// The engine's CPU code runs unchanged. At the engine's two water batches (FinishAccumulating's RenderBatches of the Stencil technique,
// FinishAccumulatingPostResolveDepth's RenderBatches of the water techniques: WaterCapture.cpp) the family lets the batch run with its
// draw choke point latched: every water pass goes through the engine's own BeginPass, SetupTechnique, SetupMaterial, SetupGeometry and
// render state (Unified Water's, Light Limit Fix's and the cubemap fix's hooks with them), and DrawTriShape records the draw instead of
// drawing it (the shadow state with its stencil mode and reference, the six constant groups the Setup functions wrote, read through
// ConstantMirror, Light Limit Fix's strict lights, the twelve textures and samplers, the geometry). The batch's draws are then drawn by
// an ORG pass at the EngineDraws anchor, in the engine's order, with CS's Water.hlsl built for ORG and the pipeline states the engine's
// state objects describe (WaterDraws.cpp).
//
// The water displacement simulation (ImageSpaceEffectWaterDisplacement) stays the engine's: it runs earlier in the frame, and the
// water's draws read its output (kWATER_DISPLACEMENT, t2) as the engine binds it.
//
// Anything the family cannot draw stays the engine's: a draw it cannot latch makes the rest of its batch the engine's, and a batch whose
// targets or published inputs the view's client was not made for is drawn by the engine. A client is made once per view; nothing the
// family does registers one again during play.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Features::Water
{
	// Engine::Offsets (Address Library ids) of the engine code the family hooks (docs/development/p5/water.md, "RE").
	namespace Address
	{
		// FinishAccumulating's RenderBatches(accumulator, 0x5C00006D, 0x5C000070, flags, -1): the water mask (SE 99938, AE 106583).
		inline constexpr Engine::Offset kMaskBatch{ { 106583, 0x492 }, { 99938, 0x4A6 } };
		// FinishAccumulatingPostResolveDepth's RenderBatches(accumulator, 0x5C000071, 0x5C006071, flags, -1): the water (SE 99939, AE 106584).
		inline constexpr Engine::Offset kSurfaceBatch{ { 106584, 0x161 }, { 99939, 0x164 } };
		// BSGraphics::Renderer::DrawTriShape(renderer, TriShape*, startIndex, triangleCount): a static TriShape's draw (R1).
		inline constexpr Engine::Offset kDrawTriShape{ { 77263 }, { 75477 } };
	}

	/** @brief Where a batch draws. */
	enum class View : std::uint8_t
	{
		Mask,     // the Stencil technique: the water's normal mask (kNORMAL_TAAMASK_SSRMASK or its swap) and motion vectors
		Surface,  // the water techniques: kMAIN, or kRAW_WATER when ISWaterBlend composites the water after
		Count
	};

	/**
	 * @brief The engine targets a view's draws may name, imported once by its client: the colour targets (RE::RENDER_TARGET) a draw's
	 * shadow state names are looked up here, and a draw naming another stays the engine's.
	 */
	inline constexpr std::array<std::int32_t, 3> kMaskTargets{ 4, 5, 7 };       // kNORMAL_TAAMASK_SSRMASK, its swap, kMOTION_VECTOR
	inline constexpr std::array<std::int32_t, 3> kSurfaceTargets{ 1, 88, -1 };  // kMAIN, kRAW_WATER

	/** @brief The colour targets a view's pixel stage writes (Water.hlsl's PS_OUTPUT): the mask's two, the surface's one. */
	inline constexpr std::uint32_t TargetsOf(View a_view) { return a_view == View::Mask ? 2u : 1u; }

	/**
	 * @brief The headers' resource registers Water.hlsl's pixel stage reads by handle, in its EXTERN order (WATER_ORG_RESOURCES): the
	 * scene depth (17), Volumetric Shadows' shadow map (18), the fog's volumes (19, 22), Cloud Shadows (25, 26), Dynamic Cubemaps (30,
	 * 31), Light Limit Fix's lights (35-37), Skylighting's probes (50), Terrain Shadows (60), Wetness Effects' occlusion (70), IBL
	 * (76, 77) and Light Limit Fix's local shadows (102, 103). The directional shadow record (t98) is a latch region instead
	 * (kDirectionalShadowRegister), between 77 and 102 in the push block.
	 */
	inline constexpr std::array<std::uint32_t, 18> kHeaderRegisters{ 17, 18, 19, 22, 25, 26, 30, 31, 35, 36, 37, 50, 60, 70, 76, 77, 102, 103 };
	/** @brief Which of kHeaderRegisters are buffers: the graph reads only a published one (a D3D11 buffer cannot be imported). */
	inline constexpr std::array<bool, 18> kHeaderIsBuffer{ false, false, false, false, false, false, false, false, true, true, true, false, false, false, false,
		false, true, false };
	/** @brief The headers before the directional shadow record in the push block. */
	inline constexpr std::size_t kHeadersBeforeShadow = 16;
	/** @brief Deferred's directional shadow record (ShadowSampling.hlsli), which the CPU writes: copied into each draw's latch. */
	inline constexpr std::uint32_t kDirectionalShadowRegister = 98;

	/** @brief The rows of each engine constant group the latch takes (Water.hlsl's packoffsets). */
	inline constexpr std::array<std::uint32_t, 6> kGroupRows{ 1, 6, 14, 5, 14, 22 };  // VS b0, b1, b2, PS b0, b1, b2

	/** @brief WaterOrgDraw in Water.hlsl: a draw's latched values, 1152 bytes. */
	struct DrawConstants
	{
		float vsTechnique[1][4];
		float vsMaterial[6][4];
		float vsGeometry[14][4];
		float psTechnique[5][4];
		float psMaterial[14][4];
		float psGeometry[22][4];
		std::uint32_t descriptors[4];  // CS's permutation block: vertex, pixel, extra, extra feature
		float effectRadius;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t padding;
		std::uint32_t textures[12];
		std::uint32_t samplers[12];
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(DrawConstants) == 1152, "WaterOrgDraw's layout");

	/** @brief StrictLightDataValues in LightLimitFix.hlsli (LightLimitFix::StrictLightDataCB): Light Limit Fix's PS b3, 1232 bytes. */
	inline constexpr std::size_t kStrictLightBytes = 1232;

	/** @brief DirectionalShadowLightData (ShadowSampling.hlsli, Deferred::DirectionalShadowLightData): t98's record, 272 bytes. */
	inline constexpr std::size_t kDirectionalShadowBytes = 272;

	/** @brief One draw's latch region: its values, its strict lights, then the directional shadow record. */
	struct DrawLatch
	{
		DrawConstants constants;
		std::byte strictLights[kStrictLightBytes];
		std::byte directionalShadow[kDirectionalShadowBytes];
	};
	static_assert(sizeof(DrawLatch) == 2656 && sizeof(DrawLatch) % 16 == 0, "a draw's latch region keeps 16-byte rows");

	/** @brief What a draw's pipeline is made of: CS's Water shaders and the engine's state objects for the draw. */
	struct PipelineKey
	{
		View view = View::Surface;
		std::uint32_t vertexDescriptor = 0, pixelDescriptor = 0;
		const void* blend = nullptr;  // the engine's ID3D11BlendState for the draw
		const void* depthStencil = nullptr;
		const void* raster = nullptr;
		std::uint32_t alphaToCoverage = 0;
		std::array<std::uint32_t, 2> colourFormats{};  // rhi::Format values, 0 past the targets
		std::uint32_t depthFormat = 0;
		bool operator==(const PipelineKey&) const = default;
	};

	/** @brief One latched draw. */
	struct Draw
	{
		PipelineKey key;
		std::shared_ptr<const void> program;    // the RasterProgram (WaterDraws.cpp)
		std::array<std::uint8_t, 2> colours{};  // indices into the view's targets (kMaskTargets, kSurfaceTargets)
		std::uint32_t stencilRef = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t viewport[4]{};
		float depthRange[2]{ 0.0f, 1.0f };
		DrawLatch latch{};
	};

	/**
	 * @brief What a view's client is made for: the headers' inputs that are published graph textures (declared by its pass, so the
	 * graph orders their writes before the water's reads). A header bound to a D3D11 view or nothing is read by heap index instead,
	 * resolved per batch, and is no part of the shape.
	 */
	struct Shape
	{
		View view = View::Surface;
		std::array<std::string, kHeaderRegisters.size()> headerIds;
		bool operator==(const Shape&) const = default;
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		View view = View::Surface;
		std::vector<Draw> draws;
		std::array<std::uint32_t, kHeaderRegisters.size()> headerIndices{};  // the unpublished headers' heap indices
		std::uint64_t revision = 0;                                          // what the recording depends on (WaterDraws::RevisionOf)
		std::vector<std::shared_ptr<const void>> owners;                     // the textures' imports and the buffers' leases
	};

	// WaterFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief Whether CS_ORG_WATER=1 turned the family on (read at install). */
	bool Enabled();

	// WaterCapture.cpp: the engine hooks, from the feature's install.
	void InstallHooks();
	void PreflightHooks();

	// WaterDraws.cpp: the render graph's side (render thread).
	namespace WaterDraws
	{
		/** @brief Whether the anchor exists and the runtime is up; false for good once a client failed. */
		bool Available();
		/** @brief Reserves the EngineDraws anchor (the feature's install, before the render graph initializes). */
		void Reserve();
		/**
		 * @brief The view's client, made for a_shape and prepared once: its draws may be withheld. A view without a client gets one made
		 * for a_shape (false: the batch stays the engine's while it joins the graph). A client made for another shape is kept: the batch
		 * stays the engine's (logged once), and no client is registered again during play.
		 */
		bool Ready(const Shape& a_shape);
		/**
		 * @brief The program for a key (a RasterProgram), compiled the first time (it blocks then, as CS's D3D11 shaders compile on
		 * first use); null when it cannot be built. a_shader: the engine's BSWaterShader, whose defines ShaderCache composes.
		 */
		std::shared_ptr<const void> Program(const PipelineKey& a_key, const void* a_shader);
		/** @brief Runs a_run's view at the anchor with its draws (an empty run builds and prepares the client only). */
		void Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
