#pragma once

#include "RenderGraph/BufferRef.h"

// Sky (CS_ORG_SKY): the render graph draws the engine's BSSkyShader passes in the main pass and the water reflection faces.
// Details are in docs/development/p5/sky.md.
// The engine's CPU code runs unchanged. The sky batch runs with DrawTriShape latched, so every pass still goes through the
// engine's setup, and the draw is recorded instead of issued (SkyCapture.cpp). An ORG pass at the Sky or SkyReflections anchor
// then draws the batch in engine order (SkyDraws.cpp). The cloud layers come right after in geometry group 13, and the capture
// spans them when that group holds only sky shapes.
// Cloud Shadows keeps per-layer occlusion in its D3D11 cubemaps, and layer L holds the occlusion of layers up to L. Graph cloud
// draws write the same layers, so both paths stay in step.
// With CS_ORG_OCCLUSION_QUERIES=1 the sun's occlusion query draw is the graph's too (docs/development/p5/occlusion-queries.md).

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
	namespace Address
	{
		// FinishAccumulating's RenderBatches of the sky techniques (SE 99938, AE 106583).
		inline constexpr Engine::Offset kSkyBatch{ { 106583, 0x237 }, { 99938, 0x247 } };
		// FinishAccumulating's RenderPersistentPassList of geometry group 13, right after the sky batch. SE 0x1412caf54, AE 0x1414b3004.
		inline constexpr Engine::Offset kCloudGroup{ { 106583, 0x274 }, { 99938, 0x284 } };
		// Sun::SubmitOcclusionQuery's draw (SE 25799 +0xAD, AE 26353 +0xAF). Read back for the sun glare by Sun::UpdateOcclusionQueries.
		// SE 99958 at 0x1412ccad0, AE 106604 at 0x1414b4c00.
		inline constexpr Engine::Offset kSunOcclusionQuery{ { 106604 }, { 99958 } };
	}

	/** @brief The scene depth the main view imports for t17: the Z-prepass copy. */
	inline constexpr auto kSceneDepth = RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY;

	/** @brief The geometry group the cloud layers are registered into (accumulation hint 0x11). */
	inline constexpr std::uint32_t kCloudGeometryGroup = 13;

	/** @brief Cloud Shadows' layer cubemaps (CloudShadows::kMaxCloudLayers). */
	inline constexpr std::uint32_t kCloudLayers = 32;

	enum class View : std::uint8_t
	{
		Main,  // the main pass's GBuffer (the Sky anchor)
		Face,  // a water reflection face (the SkyReflections anchor)
		Sun,   // the sun's occlusion query draw into kMAIN (the EngineQueries anchor, CS_ORG_OCCLUSION_QUERIES)
		Count
	};

	/** @brief SkyOrgDraw in Sky.hlsl, 352 bytes. */
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
		RenderGraph::BufferRef vertexRef;  // the vertex buffer view ref the vertex stage pulls through
		RenderGraph::BufferRef indexRef;   // the index buffer's
		std::uint32_t vertexLayout[2];
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t textures[4];  // t0-t2, then t26 (kSelfShadowFromOcclusion: the occlusion cubemap)
		std::uint32_t samplers[4];  // s0-s2
	};
	static_assert(sizeof(DrawConstants) == 352, "SkyOrgDraw's layout");
	static_assert(offsetof(DrawConstants, vertexRef) == 288 && offsetof(DrawConstants, textures) == 320, "SkyOrgDraw's rows");

	/** @brief The bytes of the vertex stage's PerGeometry the latch takes (c0 to c16.xy). */
	inline constexpr std::size_t kVertexConstantBytes = 264;

	/** @brief DrawConstants::textures[3] when self shadowing reads the occlusion cubemap. */
	inline constexpr std::uint32_t kSelfShadowFromOcclusion = ~0u;

	/** @brief ProgramKey::variant: SKY_ORG_FACES, the face's target alone, then Cloud Shadows' layer for a cloud draw. */
	inline constexpr std::uint32_t kVariantFaces = 1;

	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // the RasterProgram
		// A face cloud draw writing Cloud Shadows' layer. Its occlusion is also drawn into the face's later layers with occlusionProgram.
		bool occlusion = false;
		std::int32_t layer = -1;
		std::shared_ptr<const void> occlusionProgram;
		std::uint32_t indexCount = 0;
		std::uint32_t stencilRef = 0;  // the shadow state's stencil reference (command list state, not the pipeline's)
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	/**
	 * @brief The header texture registers the sky's pixel stage may read, in Sky.hlsl's EXTERN order.
	 * Exponential Height Fog's header pulls in t30 and t31.
	 */
	inline constexpr std::array<std::uint32_t, 4> kHeaderRegisters{ 76, 77, 30, 31 };
	inline constexpr std::array<GpuTextures::ViewKind, 4> kHeaderKinds{ GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Texture2D,
		GpuTextures::ViewKind::Cube, GpuTextures::ViewKind::Cube };

	/**
	 * @brief What a view's client is made for: the targets it imports once at setup.
	 * Headers and any t17 other than the prepass copy are read by heap index, resolved per batch.
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
		// View::Sun only. Whether the depth target is kPOST_ZPREPASS_COPY rather than kMAIN's.
		bool sunCopyDepth = false;
		std::int32_t query = -1;
	};

	Api::Feature Feature();

	/** @brief Whether CS_ORG_SKY=1 turned the family on (read at install). */
	bool Enabled();

	void InstallHooks();
	void PreflightHooks();

	namespace SkyDraws
	{
		/** @brief Whether the anchors and runtime are up. False for good once a client failed. */
		bool Available();
		/** @brief Reserves both anchors. Must run before the render graph initializes. */
		void Reserve();
		/** @brief Makes both views' clients for their predicted shapes and queues their programs. Main thread. */
		bool Setup();
		/** @brief Whether a_shape's batch is taken now. False is logged once per reason, and the engine draws the batch. */
		bool CanTake(const Shape& a_shape);
		/** @brief How a_view's client reads the published id a bound header or t17 exports. */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key, or null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases alive when it ran nothing, so the next batches find them. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's anchor. False means its draws were not drawn, and that is logged. */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief Whether the graph answers the sun's occlusion queries. */
		bool HasSunClient();
		/** @brief Whether a run of the sun's anchor now would draw. False is logged once. */
		bool CanTakeSun();
		std::string Status();
	}
}
