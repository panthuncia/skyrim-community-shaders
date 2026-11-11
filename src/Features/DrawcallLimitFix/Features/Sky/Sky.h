#pragma once

// Sky (CS_ORG_SKY, docs/development/p5/sky.md): the engine's BSSkyShader draws (the sky dome, the stars and moon masks, the sun and
// moons, the cloud layers) drawn by the render graph in the engine's place, in the main pass and in the water reflection's faces.
//
// The engine's CPU code runs unchanged. At the engine's sky batch (BSShaderAccumulator::FinishAccumulating's RenderBatches of the sky
// techniques, SkyCapture.cpp) the family lets the batch run with its draw choke point latched: every sky pass goes through the
// engine's own BeginPass, SetupTechnique, SetupGeometry and render state, and its DrawTriShape (the engine draw families' one detour,
// Core/Engine/Family) records the draw instead of drawing it (the shadow state, the constants SetupGeometry wrote, read from the engine
// constants capture, the textures, the geometry). The batch's draws are then drawn by an ORG pass at the Sky anchor (the main pass) or the
// SkyReflections anchor (a reflection face), in the engine's order, with CS's Sky.hlsl built for ORG and the pipeline states the
// engine's state objects describe (SkyDraws.cpp).
//
// The cloud layers are not in that batch: they are geometry group 13's pass list, which FinishAccumulating draws right after it. When
// that list holds only sky TriShapes and CS_ORG_SKY_CLOUDS takes the view's clouds, the capture spans it too, and the anchor runs after
// it (SkyCapture.cpp, CloudGroup).
//
// Cloud Shadows keeps its per-layer occlusion in its D3D11 layer cubemaps: layer L of a face holds the occlusion of the face's layers
// up to L. The main pass's clouds read their own layer for their self shadowing, and its occlusion copy reads the last layer. A face
// cloud draw on the graph writes its layer through an import of it, and draws its occlusion into the face's later layers too
// (SkyDraws.cpp, FaceClouds), so both paths keep and read the same layers.
//
// The family is the primary path for the sky (docs/development/p5/primary-path.md): its stages compile when CS loads BSSkyShader's
// pixel descriptors, its programs are prebuilt from the manifest, and both views' clients are made at setup for the targets the views
// draw into (SkyDraws::Setup), with every published input declared. A draw it cannot take (its inherent refusals,
// docs/development/p5/sky.md "What stays the engine's") ends the batch's latched part: the latched draws run at that point and the
// engine draws the rest, the cloud group's included. A batch whose targets differ from its client's is the engine's.
//
// With CS_ORG_OCCLUSION_QUERIES=1 the sun's occlusion query draw is the graph's too (docs/development/p5/occlusion-queries.md): at
// Sun::SubmitOcclusionQuery's draw (SkyCapture.cpp, SunOcclusionQuery) the engine's query is bracketed
// (RenderGraph/Engine/Queries), its SunOcclude draw is latched as a batch's draws are, and the EngineQueries anchor's pass draws it
// with an ORG occlusion query whose count answers the engine's GetQueryData. A draw it cannot take is skipped, never the engine's: the
// query answers its last count.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
		// The accumulator's occlusion query draw (Sun::SubmitOcclusionQuery's, SE 25799 +0xAD, AE 26353 +0xAF): the sun's SunOcclude pass
		// between BeginQuery and EndQuery, read back for the sun glare by Sun::UpdateOcclusionQueries (SE 99967, AE 106605). The graph's
		// with CS_ORG_OCCLUSION_QUERIES=1 (docs/development/p5/occlusion-queries.md). SE 99958 at 0x1412ccad0, AE 106604 at 0x1414b4c00.
		inline constexpr Engine::Offset kSunOcclusionQuery{ { 106604 }, { 99958 } };
	}

	/** @brief The scene depth the main view's client imports for t17: the Z-prepass copy (Util::GetCurrentSceneDepthSRV's own). */
	inline constexpr auto kSceneDepth = RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY;

	/** @brief The geometry group the cloud layers are registered into (accumulation hint 0x11; BSBatchRenderer::geometryGroups). */
	inline constexpr std::uint32_t kCloudGeometryGroup = 13;

	/** @brief Cloud Shadows' layer cubemaps (CloudShadows::kMaxCloudLayers). */
	inline constexpr std::uint32_t kCloudLayers = 32;

	/** @brief Where a batch draws. */
	enum class View : std::uint8_t
	{
		Main,  // the main pass's GBuffer (the Sky anchor)
		Face,  // a water reflection face (the SkyReflections anchor)
		Sun,   // the sun's occlusion query draw into kMAIN (the EngineQueries anchor, CS_ORG_OCCLUSION_QUERIES)
		Count
	};

	/** @brief SkyOrgDraw in Sky.hlsl: a draw's latched values, 352 bytes. */
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
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t textures[4];  // t0-t2, then t26 (kSelfShadowFromOcclusion: the occlusion cubemap)
		std::uint32_t samplers[4];  // s0-s2
	};
	static_assert(sizeof(DrawConstants) == 352, "SkyOrgDraw's layout");
	static_assert(offsetof(DrawConstants, vertexAddress) == 288 && offsetof(DrawConstants, textures) == 320, "SkyOrgDraw's rows");

	/** @brief The bytes of the vertex stage's PerGeometry the latch takes (c0 to c16.xy). */
	inline constexpr std::size_t kVertexConstantBytes = 264;

	/** @brief DrawConstants::textures[3] for a draw whose self shadowing reads the occlusion cubemap (SKY_ORG_OCCLUSION). */
	inline constexpr std::uint32_t kSelfShadowFromOcclusion = ~0u;

	/** @brief ProgramKey::variant: SKY_ORG_FACES, the face's target alone, then Cloud Shadows' layer for a cloud draw. */
	inline constexpr std::uint32_t kVariantFaces = 1;

	/** @brief One latched draw. */
	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // the RasterProgram
		// A face's cloud draw that writes Cloud Shadows' layer `layer` (no depth target, as Cloud Shadows binds it). Its occlusion is
		// also drawn into the face's later layers with occlusionProgram (the same draw with the face's target write-masked).
		bool occlusion = false;
		std::int32_t layer = -1;
		std::shared_ptr<const void> occlusionProgram;
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	/**
	 * @brief The headers' texture registers the sky's pixel stage may read, in Sky.hlsl's EXTERN order: IBL's t76 and t77 and Dynamic
	 * Cubemaps' t30 and t31, which Exponential Height Fog's header includes (its reflection fog reads t31). Their declared kinds follow.
	 */
	inline constexpr std::array<std::uint32_t, 4> kHeaderRegisters{ 76, 77, 30, 31 };
	inline constexpr std::array<GpuTextures::ViewKind, 4> kHeaderKinds{ GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Texture2D,
		GpuTextures::ViewKind::Cube, GpuTextures::ViewKind::Cube };

	/**
	 * @brief What a view's client is made for: its targets, which it imports once at setup (SkyDraws::Setup predicts them: the deferred
	 * pass's first three targets and kMAIN's depth, the reflection cubemap with its cube depth). The published inputs its passes declare
	 * are fixed when the client is made (SkyDraws.cpp, ListInputs), so they are no part of it. The headers and a t17 other than the
	 * Z-prepass copy are read by heap index, resolved per batch (Run): a D3D11 view's import, a null view, or a published input's
	 * index (SkyDraws::ReadPublished).
	 */
	struct Shape
	{
		View view = View::Main;
		std::array<std::int32_t, 3> colours{ -1, -1, -1 };  // RE::RENDER_TARGET, the main pass's
		std::int32_t depth = -1;                            // RE::RENDER_TARGETS_DEPTHSTENCIL of the depth target
		bool cloudShadows = false;                          // Cloud Shadows' published occlusion is read (t25)
		bool clouds = false;                                // the cloud layers (geometry group 13) are the graph's when the group allows
		bool operator==(const Shape&) const = default;
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		View view = View::Main;
		std::uint32_t face = 0;
		std::vector<Draw> draws;
		std::uint32_t sceneDepthIndex = GpuTextures::kInvalid;  // t17 when not kSceneDepth: its import, or a published id's index
		bool sceneDepthNull = false;                            // t17 is the depth target itself, or nothing: a null view
		std::array<std::uint32_t, 4> headerIndices{};           // kHeaderRegisters: imports, published ids' indices or null views
		std::uint64_t revision = 0;                             // what the recording depends on (SkyDraws.cpp, RevisionOf)
		Family::Owners owners;                                  // the textures' imports and the buffers' leases
		// View::Sun: the depth target is kPOST_ZPREPASS_COPY (else kMAIN's), and the engine ring slot its one draw counts.
		bool sunCopyDepth = false;
		std::int32_t query = -1;
	};

	// SkyFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief Whether CS_ORG_SKY=1 turned the family on (read at install). */
	bool Enabled();

	/**
	 * @brief Whether the graph takes a_view's cloud layers too (CS_ORG_SKY_CLOUDS: 1 both views', main or faces only that view's;
	 * unset or 0: the engine draws them).
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
		 * @brief The family's setup (Family::AddSetup, main thread): the stage precompile started, the manifest's programs queued, both
		 * views' clients made for their predicted shapes and their build asked for. True once done (or refused for good, logged); false
		 * while the graph has no host.
		 */
		bool Setup();
		/**
		 * @brief Whether a_shape's batch is taken: its view's client was made for a_shape's targets, and a run of its anchor now would
		 * draw (FrameAnchors::CanRun). False is logged once per reason; the engine draws the batch. No client is made again during play.
		 */
		bool CanTake(const Shape& a_shape);
		/**
		 * @brief How a_view's client reads a_id, the published id a bound header or t17 exports (Family::PublishedInputs::Resolve).
		 * NotListed when the view has no client.
		 */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key (Family::EngineDrawPrograms::Get): null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases as long as a run's when it ran nothing, so the next batches find them. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's anchor with its draws. True when the run was submitted; otherwise its draws were not drawn (logged). */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief Whether the sun's occlusion client was made at setup and the family has not failed: the graph answers its queries. */
		bool HasSunClient();
		/** @brief Whether a run of the sun's anchor now would draw (FrameAnchors::CanRun). False is logged once. */
		bool CanTakeSun();
		/** @brief One status line. */
		std::string Status();
	}
}
