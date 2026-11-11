#pragma once

// Sky (CS_ORG_SKY, docs/development/p5/sky.md): the engine's BSSkyShader draws (the sky dome, the stars and moon masks, the sun and
// moons, the cloud layers) drawn by the render graph in the engine's place, in the main pass and in the water reflection's faces.
//
// The engine's CPU code runs unchanged. At the engine's sky batch (BSShaderAccumulator::FinishAccumulating's RenderBatches of the sky
// techniques, SkyCapture.cpp) the family lets the batch run with its draw choke point latched: every sky pass goes through the
// engine's own BeginPass, SetupTechnique, SetupGeometry and render state, and its DrawTriShape records the draw instead of drawing it
// (the shadow state, the constants SetupGeometry wrote, read through ConstantMirror, the textures, the geometry). The batch's draws are
// then drawn by an ORG pass at the Sky anchor (the main pass) or the SkyReflections anchor (a reflection face), in the engine's order,
// with CS's Sky.hlsl built for ORG and the pipeline states the engine's state objects describe (SkyDraws.cpp).
//
// The cloud layers are not in that batch: they are geometry group 13's pass list, which FinishAccumulating draws right after it. When
// that list holds only sky TriShapes, the capture spans it too, and the anchor runs after it (SkyCapture.cpp, CloudGroup).
//
// On the faces Cloud Shadows' fourth render target becomes an output of the faces' cloud draws: Cloud Shadows' graph faces
// (ORGCloudShadows::GraphFaces), which its ORG copy reads for every face whose clouds the graph drew last (ORGCloudShadows::NoteFace).
//
// Anything the family cannot draw stays the engine's: a draw it cannot latch is drawn by the engine where it was, and a batch whose
// targets the anchor's client was not built for runs on D3D11 while the client is made again.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11Buffer;
struct ID3D11ShaderResourceView;

namespace DCLF::Features::Sky
{
	// Engine::Offsets (Address Library ids) of the engine code the family hooks (docs/development/p5/sky.md, "RE").
	namespace Address
	{
		// FinishAccumulating's RenderBatches(accumulator, 0x5C00005D, 0x5C000064, flags, -1): the sky techniques' batch (SE 99938, AE 106583).
		inline constexpr Engine::Offset kSkyBatch{ { 106583, 0x237 }, { 99938, 0x247 } };
		// FinishAccumulating's RenderPersistentPassList(geometry group 13, flags), right after the sky batch: the cloud layers (BSSkyShader
		// passes of accumulation hint 0x11) and the sky-object effects (hint 0x12). SE 0x1412caf54, AE 0x1414b3004.
		inline constexpr Engine::Offset kCloudGroup{ { 106583, 0x274 }, { 99938, 0x284 } };
		// BSGraphics::Renderer::DrawTriShape(renderer, TriShape*, startIndex, triangleCount): a static TriShape's draw (R1).
		inline constexpr Engine::Offset kDrawTriShape{ { 77263 }, { 75477 } };
	}

	/** @brief The sky techniques' ids (BSSkyShader's pass enumeration): the batch's range. */
	inline constexpr std::uint32_t kFirstSkyTechnique = 0x5C00005D;
	inline constexpr std::uint32_t kLastSkyTechnique = 0x5C000064;

	/** @brief The geometry group the cloud layers are registered into (accumulation hint 0x11; BSBatchRenderer::geometryGroups). */
	inline constexpr std::uint32_t kCloudGeometryGroup = 13;

	/** @brief Where a batch draws. */
	enum class View : std::uint8_t
	{
		Main,  // the main pass's GBuffer (the Sky anchor)
		Face,  // a water reflection face (the SkyReflections anchor)
		Count
	};

	/** @brief SkyOrgDraw in Sky.hlsl: a draw's latched constants, 288 bytes. */
	struct DrawConstants
	{
		float worldViewProj[16];
		float world[16];
		float previousWorld[16];
		float eyePosition[3];
		float vParams;
		float blendColor[12];
		float texCoordOff[2];
		float pParams[2];
		float alphaTestRef;
		std::uint32_t extraShaderDescriptor;
		std::uint32_t padding[2];
	};
	static_assert(sizeof(DrawConstants) == 288, "SkyOrgDraw's layout");

	/** @brief The bytes of the vertex stage's PerGeometry the latch takes (c0 to c16.xy). */
	inline constexpr std::size_t kVertexConstantBytes = 264;

	/** @brief What a draw's pipeline is made of: CS's Sky shaders and the engine's state objects for the draw. */
	struct PipelineKey
	{
		View view = View::Main;
		bool occlusion = false;  // a face's cloud draw that also writes Cloud Shadows' occlusion (no depth target)
		std::uint32_t vertexDescriptor = 0, pixelDescriptor = 0;
		const void* blend = nullptr;  // the engine's ID3D11BlendState for the draw (Cloud Shadows' own for an occlusion draw)
		const void* depthStencil = nullptr;
		const void* raster = nullptr;
		std::uint32_t alphaToCoverage = 0;
		std::array<std::uint32_t, 3> colourFormats{};  // rhi::Format values, 0 past the targets
		std::uint32_t depthFormat = 0;
		bool operator==(const PipelineKey&) const = default;
	};

	/** @brief One latched draw. */
	struct Draw
	{
		PipelineKey key;
		std::shared_ptr<const void> program;  // the RasterProgram (SkyDraws.cpp)
		std::uint64_t vertexAddress = 0, indexAddress = 0, vertexLayout = 0;
		std::uint32_t vertexStride = 0, firstIndex = 0, indexCount = 0;
		std::array<std::uint32_t, 3> textures{};  // heap indices (t0-t2)
		std::array<std::uint32_t, 3> samplers{};  // sampler heap indices (s0-s2)
		std::uint32_t viewport[4]{};
		float depthRange[2]{ 0.0f, 1.0f };
		DrawConstants constants{};
	};

	/** @brief What a batch's client is built for: its targets, which a change makes again. */
	struct Shape
	{
		View view = View::Main;
		std::array<std::int32_t, 3> colours{ -1, -1, -1 };  // RE::RENDER_TARGET, the main pass's
		std::int32_t depth = -1;                            // RE::RENDER_TARGETS_DEPTHSTENCIL of the depth target
		std::int32_t sceneDepth = -1;                       // the main pass's t17, an engine depth target (-1: none)
		std::string sceneDepthId;                           // or a published graph texture's id
		const void* sceneDepthView = nullptr;               // or the D3D11 view CS binds (Terrain Blending's D3D11 depth), imported
		bool cloudShadows = false;                          // Cloud Shadows' published occlusion is read (t25, t26)
		bool clouds = false;                                // the cloud layers (geometry group 13) are the graph's when the group allows
		// The headers' textures the sky's includes read (kHeaderRegisters): what CS binds there for the pass, as a published graph
		// texture's id, or the D3D11 view (imported) when it is not one's export. Neither: nothing is bound there.
		std::array<std::string, 4> headerIds;
		std::array<const void*, 4> headerViews{};
		bool operator==(const Shape&) const = default;
	};

	/**
	 * @brief The headers' texture registers the sky's pixel stage may read, in Sky.hlsl's EXTERN order: IBL's t76 and t77 and Dynamic
	 * Cubemaps' t30 and t31, which Exponential Height Fog's header includes (its reflection fog reads t31).
	 */
	inline constexpr std::array<std::uint32_t, 4> kHeaderRegisters{ 76, 77, 30, 31 };

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		View view = View::Main;
		std::uint32_t face = 0;
		std::vector<Draw> draws;
		std::uint64_t revision = 0;  // what the recording depends on (SkyDraws::RevisionOf)
		std::vector<std::shared_ptr<const void>> owners;  // the textures' imports and the buffers' leases
	};

	// SkyFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief Whether CS_ORG_SKY=1 turned the family on (read at install). */
	bool Enabled();

	/** @brief Whether the graph takes a_view's batches (the CS_ORG_SKY_VIEWS diagnostic: main, faces, or both when unset). */
	bool TakesView(View a_view);

	/**
	 * @brief Whether the graph takes a_view's cloud layers too (CS_ORG_SKY_CLOUDS: 0 leaves them to the engine, main or faces takes
	 * only that view's; unset: both).
	 */
	bool TakesClouds(View a_view);

	// SkyCapture.cpp: the engine hooks, from the feature's install.
	void InstallHooks();
	void PreflightHooks();

	// SkyDraws.cpp: the render graph's side (render thread).
	namespace SkyDraws
	{
		/** @brief Whether the anchors exist and the runtime is up; false for good once a client failed. */
		bool Available();
		/** @brief Reserves both anchors (the feature's install, before the render graph initializes). */
		void Reserve();
		/**
		 * @brief The client of a_shape's view built for a_shape and prepared once: its draws may be withheld. Otherwise it is
		 * (re)registered for a_shape and false: the batch stays the engine's.
		 */
		bool Ready(const Shape& a_shape);
		/**
		 * @brief The program for a key (a RasterProgram), compiled the first time (it blocks then, as CS's D3D11 shaders compile on
		 * first use); null when it cannot be built. a_shader: the engine's BSSkyShader, whose defines ShaderCache composes.
		 */
		std::shared_ptr<const void> Program(const PipelineKey& a_key, const void* a_shader);
		/** @brief Keeps a probe's imports and leases as long as a run's, so the next batches find them instead of importing again. */
		void Keep(std::vector<std::shared_ptr<const void>> a_owners);
		/** @brief Runs a_run's anchor with its draws (an empty run builds and prepares the client only). */
		void Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
