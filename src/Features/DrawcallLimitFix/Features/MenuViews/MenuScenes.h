#pragma once

#include "RenderGraph/BufferRef.h"

// Menu scenes as an engine draw family (docs/development/engine/menus.md). Batches are the local map (LocalMapRenderer::Render,
// SE 16094, AE 16335) with its fog of war, and a menu model's shadow, depth and colour passes (UI3DSceneManager::Render, SE 51855,
// AE 52727). An ORG pass at the EngineDraws anchor draws each batch in engine order (MenuScenesDraws.cpp). Programs are ORG builds of
// Utility.hlsl, Lighting.hlsl, Effect.hlsl and Water.hlsl, with the world's CS features off.
// Native mode computes and draws the scenes itself (MenuScenesNative.cpp): each draw comes from the scene graph, its lights and camera,
// no engine render function runs, and a draw CS cannot compute is fatal.
// Guest mode keeps the engine's render (MenuScenesCapture.cpp): the engine culls and sets up every pass, and DrawTriShape latches each
// draw. In local map mode a Lighting property makes a BSUtilityShader RENDER_BASE_TEXTURE pass instead (SE 0x1412c7cd0), while
// landscape keeps its Lighting passes. A draw with no ORG build, such as tree LOD, or a refused draw ends its batch's latched part,
// and the engine draws the rest.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPortFrame.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"
#include "Snapshot/FrameCamera.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Features::MenuViews::Scenes
{
	enum class View : std::uint8_t
	{
		LocalMap,     // the local map's land and objects (LocalMapRenderer::Render's first SubmitAccumulator)
		LocalMapFog,  // its fog of war (the second, render mode 0x13)
		MenuDepth,    // a menu model's depth pass (UI3DSceneManager::Render's StartAccumulating, with keepDepth)
		MenuColour,   // its colour pass (UI3DSceneManager::Render's SubmitAccumulator)
		MenuShadow,   // the loading screen model's shadow maps (UI3DSceneManager::Render's call that renders its shadow lights)
		Count
	};

	/** @brief The view's name for logs and client ids. */
	const char* NameOf(View a_view);

	/** @brief RE::RENDER_TARGET values the batches draw into. */
	inline constexpr std::int32_t kFramebufferTarget = 0;        // kFRAMEBUFFER
	inline constexpr std::int32_t kMainTarget = 1;               // kMAIN
	inline constexpr std::int32_t kNormalTaaMaskTarget = 4;      // kNORMAL_TAAMASK_SSRMASK
	inline constexpr std::int32_t kNormalTaaMaskSwapTarget = 5;  // kNORMAL_TAAMASK_SSRMASK_SWAP
	inline constexpr std::int32_t kMotionVectorTarget = 7;       // kMOTION_VECTOR
	inline constexpr std::int32_t kLocalMapSwapTarget = 17;      // kLOCAL_MAP_SWAP

	/** @brief The colour targets a view's client imports. Empty for none. */
	std::span<const std::int32_t> TargetsOf(View a_view);

	/** @brief The most colour targets a draw writes: RT0, motion vectors and the TAA mask. */
	inline constexpr std::uint32_t kMaxColours = 3;

	inline constexpr std::uint32_t kUtilitySlots = 8;

	using Kind = GpuTextures::ViewKind;

	/** @brief Each Utility slot's declared dimension. The shadow maps (t3, t4, t6) are arrays. */
	constexpr Kind UtilitySlotKind(std::uint32_t a_slot)
	{
		return a_slot == 3 || a_slot == 4 || a_slot == 6 ? Kind::Texture2DArray : Kind::Texture2D;
	}

	/** @brief Whether UTILITY_ORG_SCENE reads slot a_slot. Every other slot is latched as a null view. */
	constexpr bool UtilitySlotRead(std::uint32_t a_slot) { return a_slot == 0 || a_slot == 7; }

	inline constexpr std::uint32_t kVSTechniqueRows = 2;
	inline constexpr std::uint32_t kVSMaterialRows = 1;
	inline constexpr std::uint32_t kVSGeometryRows = 8;
	inline constexpr std::uint32_t kPSMaterialRows = 2;
	inline constexpr std::uint32_t kPSGeometryRows = 4;

	/** @brief UtilityOrgSceneDraw in Utility.hlsl, 384 bytes. */
	struct UtilitySceneConstants
	{
		float vsTechnique[kVSTechniqueRows][4];
		float vsMaterial[kVSMaterialRows][4];
		float vsGeometry[kVSGeometryRows][4];
		float psMaterial[kPSMaterialRows][4];
		float psGeometry[kPSGeometryRows][4];
		float alphaTestRef;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t padding;
		std::uint32_t textures[kUtilitySlots];
		std::uint32_t samplers[kUtilitySlots];
		RenderGraph::BufferRef vertexRef;  // the vertex buffer view ref the vertex stage pulls through
		RenderGraph::BufferRef indexRef;   // the index buffer's
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(UtilitySceneConstants) == 384 && sizeof(UtilitySceneConstants) % 16 == 0, "UtilityOrgSceneDraw's layout");

	inline constexpr std::uint32_t kLightingVSTechniqueRows = 4;
	inline constexpr std::uint32_t kLightingVSMaterialRows = 3;
	inline constexpr std::uint32_t kLightingVSGeometryRows = 15;
	inline constexpr std::uint32_t kLightingPSTechniqueRows = 3;
	inline constexpr std::uint32_t kLightingPSMaterialRows = 18;
	inline constexpr std::uint32_t kLightingPSGeometryRows = 30;
	inline constexpr std::uint32_t kLightingSlots = 16;

	/**
	 * @brief Whether Lighting.hlsl declares slot a_slot for a_descriptor.
	 * The engine leaves stale views in unset slots, such as a depth plane after a world frame, so undeclared slots are latched null.
	 */
	constexpr bool LightingSlotRead(std::uint32_t a_descriptor, std::uint32_t a_slot)
	{
		enum : std::uint32_t
		{
			kEnvmap = 1,
			kParallax = 3,
			kFacegen = 4,
			kParallaxOcc = 7,
			kMultiTexLand = 8,
			kLodLand = 9,
			kMultilayerParallax = 11,
			kEye = 16,
			kLodLandNoise = 18,
			kMultiTexLandLodBlend = 19,
		};
		const std::uint32_t technique = (a_descriptor >> 24) & 0x3F;
		const auto flag = [&](std::uint32_t a_bit) { return (a_descriptor & (1u << a_bit)) != 0; };
		// TRUE_PBR (bit 3) and the land techniques declare most slots: all are latched.
		if (flag(3) || technique == kMultiTexLand || technique == kLodLand || technique == kLodLandNoise || technique == kMultiTexLandLodBlend)
			return true;
		const bool projected = flag(15);
		const bool envmap = technique == kEnvmap || technique == kMultilayerParallax || technique == kEye || flag(19) || flag(21);
		switch (a_slot) {
		case 2:
			return flag(2);
		case 3:
			return technique == kFacegen || technique == kParallax || technique == kParallaxOcc || projected;
		case 4:
		case 5:
			return envmap || technique == kFacegen;
		case 7:
		case 15:
			return false;
		case 8:
			return technique == kMultilayerParallax || projected;
		case 10:
			return projected;
		case 13:
			return flag(18);
		default:
			return true;
		}
	}

	/**
	 * @brief The Lighting program variant that keeps Linear Lighting's pipeline (LIGHTING_ORG_LINEAR). The local map's land takes it: its
	 * swap target stores what the shader outputs, so the pipeline sets the image, and plain D3D11 draws it with Linear Lighting's.
	 */
	inline constexpr std::uint32_t kLightingLinearVariant = 1;

	/** @brief LightingOrgDraw in Lighting.hlsl, 1376 bytes. */
	struct LightingSceneConstants
	{
		float vsTechnique[kLightingVSTechniqueRows][4];
		float vsMaterial[kLightingVSMaterialRows][4];
		float vsGeometry[kLightingVSGeometryRows][4];
		float psTechnique[kLightingPSTechniqueRows][4];
		float psMaterial[kLightingPSMaterialRows][4];
		float psGeometry[kLightingPSGeometryRows][4];
		std::uint32_t descriptors[4];  // CS's permutation block: vertex, pixel, extra and extra feature descriptors
		float effectRadius;
		std::uint32_t baseTextureIsWorking;
		std::uint32_t renderToUI;
		float alphaTestRef;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t nullTexture;
		std::uint32_t padding;
		std::uint32_t textures[kLightingSlots];
		std::uint32_t samplers[kLightingSlots];
		RenderGraph::BufferRef vertexRef;  // the vertex buffer view ref the vertex stage pulls through
		RenderGraph::BufferRef indexRef;   // the index buffer's
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(LightingSceneConstants) == 1376 && sizeof(LightingSceneConstants) % 16 == 0, "LightingOrgDraw's layout");

	inline constexpr std::uint32_t kEffectVSTechniqueRows = 3;
	inline constexpr std::uint32_t kEffectVSMaterialRows = 3;
	inline constexpr std::uint32_t kEffectVSGeometryRows = 15;
	inline constexpr std::uint32_t kEffectPSTechniqueRows = 2;
	inline constexpr std::uint32_t kEffectPSMaterialRows = 3;
	inline constexpr std::uint32_t kEffectPSGeometryRows = 12;
	inline constexpr std::uint32_t kEffectSlots = 8;
	/** @brief The slots Effect.hlsl declares. The rest are latched as null views. */
	inline constexpr std::uint32_t kEffectSlotsRead = 5;

	/** @brief EffectOrgDraw in Effect.hlsl, 752 bytes. */
	struct EffectSceneConstants
	{
		float vsTechnique[kEffectVSTechniqueRows][4];
		float vsMaterial[kEffectVSMaterialRows][4];
		float vsGeometry[kEffectVSGeometryRows][4];
		float psTechnique[kEffectPSTechniqueRows][4];
		float psMaterial[kEffectPSMaterialRows][4];
		float psGeometry[kEffectPSGeometryRows][4];
		std::uint32_t descriptors[4];
		float effectRadius;
		std::uint32_t baseTextureIsWorking;
		std::uint32_t renderToUI;
		float alphaTestRef;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t nullTexture;
		std::uint32_t streamStride;  // a DrawData copy's stream 1 (the dynamic ring), with streamRef; 0 for a static draw
		std::uint32_t textures[kEffectSlots];
		std::uint32_t samplers[kEffectSlots];
		RenderGraph::BufferRef vertexRef;  // the vertex buffer view ref the vertex stage pulls through
		RenderGraph::BufferRef indexRef;   // the index buffer's
		std::uint32_t vertexLayout[2];
		RenderGraph::BufferRef streamRef;  // stream 1's (null for a static draw)
	};
	static_assert(sizeof(EffectSceneConstants) == 752 && sizeof(EffectSceneConstants) % 16 == 0, "EffectOrgDraw's layout");
	/** @brief A TEXCOORD_INDEX draw's indexed texture coordinates (VS b11), latched right after its constants. */
	inline constexpr std::uint32_t kEffectIndexedTexCoordRows = 128;
	inline constexpr std::uint32_t kEffectIndexedTexCoordBytes = kEffectIndexedTexCoordRows * 16;

	/** @brief FrameBuffer (b12) c0-c44 as the draw had it bound, latched with the draw. */
	inline constexpr std::uint32_t kFrameRows = 45;
	inline constexpr std::uint32_t kFrameBytes = kFrameRows * 16;

	/** @brief A skinned draw's palettes for ORG_LATCHED_BONES: previous frame's (VS b9), then this one's (b10). */
	inline constexpr std::uint32_t kPaletteRows = 240;
	inline constexpr std::uint32_t kBonesBytes = 2 * kPaletteRows * 16;

	/** @brief The engine renders at most four of a scene's shadow lights. */
	inline constexpr std::uint32_t kMaxShadowSlices = 4;

	/** @brief The most draws and latch bytes a menu batch takes. */
	inline constexpr std::uint32_t kMaxDraws = 2048;
	inline constexpr std::uint32_t kLatchBytes = 1u << 20;
	/**
	 * @brief A batch's draw limit. The local map's land and objects draw one pass per visible shape of the loaded cells (about 1,380 in
	 * Whiterun); a city's can pass 2048, so its batch takes four times as many.
	 */
	constexpr std::uint32_t MaxDrawsOf(View a_view)
	{
		return a_view == View::LocalMap ? 4u * kMaxDraws : kMaxDraws;
	}
	/** @brief A batch's latch size: room for its draw limit's Utility constants and a share of Lighting ones. */
	constexpr std::uint32_t LatchBytesOf(View a_view)
	{
		return a_view == View::LocalMap ? 8u * kLatchBytes : kLatchBytes;
	}

	enum class Shader : std::uint8_t
	{
		Utility,
		Lighting,
		Effect,
		Water,
	};

	struct Draw
	{
		Shader shader = Shader::Utility;
		Family::ProgramKey key;
		std::shared_ptr<const void> program;              // the RasterProgram (MenuScenesDraws.cpp)
		std::array<std::uint8_t, kMaxColours> colours{};  // indices into the view's imports (TargetsOf)
		std::uint32_t colourCount = 0;
		std::uint32_t latchOffset = 0;  // its constants in the run's latch
		std::uint32_t bonesOffset = 0;  // its palettes in the run's latch (a skinned draw's; 0 otherwise, never read)
		std::uint32_t frameOffset = 0;  // its FrameBuffer rows in the run's latch
		std::uint32_t stencilRef = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t depthSlice = 0;  // the slice of the view's depth target it draws into
		Family::Viewport viewport;
	};

	/** @brief A batch's draws and their latched bytes. */
	struct Run
	{
		View view = View::LocalMap;
		std::vector<Draw> draws;
		std::vector<std::byte> latch;  // every draw's constants and palettes, written into the client's latch at the run's commit
		std::uint64_t revision = 0;    // what the recording depends on (MenuScenesDraws.cpp, Run::Hash)
		Family::Owners owners;         // the textures' imports and the buffers' leases

		/** @brief Appends a_bytes to the latch, 16-byte aligned. Returns the offset, or ~0u when full. */
		std::uint32_t Append(std::span<const std::byte> a_bytes)
		{
			const std::size_t offset = (latch.size() + 15) & ~std::size_t{ 15 };
			if (offset + a_bytes.size() > LatchBytesOf(view))
				return ~0u;
			latch.resize(offset + a_bytes.size());
			std::copy(a_bytes.begin(), a_bytes.end(), latch.begin() + static_cast<std::ptrdiff_t>(offset));
			return static_cast<std::uint32_t>(offset);
		}

		/** @brief What the recording depends on: everything but the latched values and counts (MenuScenesDraws.cpp). */
		std::uint64_t Hash() const;
	};

	/** @brief Whether the local map's batches are the family's (graph mode). */
	bool Enabled();
	/** @brief Whether a_view's batches are the family's, per MapEnabled or Menu3DEnabled. */
	bool ViewEnabled(View a_view);
	/** @brief Whether any view is the family's. */
	bool AnyEnabled();

	void InstallHooks();
	void PreflightHooks();

	/**
	 * @brief The native menu scenes (MenuScenesNative.cpp): a menu scene's draws computed by CS from the menu's scene graph, lights and
	 * camera, with DCLF's ports. No engine setup function runs.
	 */
	namespace Native
	{
		/** @brief The menu scene's camera last rendered, for its previous-frame rows. */
		struct PreviousCamera
		{
			bool valid = false;
			Snapshot::FrameCamera::Previous camera;
		};

		/** @brief What a menu render's draws are computed from, read once at the render. */
		struct Inputs
		{
			RE::NiPoint3 eye{}, previousEye{};
			const RE::ShadowSceneNode* sceneNode = nullptr;
			TechniqueInputs technique;
			GeometryPort::PipelineFrame pipeline;
			MaterialPort::MaterialFrame material;
			Snapshot::CameraView camera;
			Snapshot::FrameCamera::Previous previous;
			bool havePrevious = false;
			bool shadowMaps = false;  // the render renders shadow maps (scheme 4 with a depth pass): shadow lights add their descriptor bits
			// The local map's draws (SampleLocalMapInputs): no previous camera (FrameBuffer c16-c19 and c41 zero, PreviousWorld from a zero
			// eye), the sun as the only light, and the land's descriptor bits as the engine's local map passes carry them.
			bool localMap = false;
			// The scene node's shadow lights' dimmers as MistMenu's shadow render sets them (ShadowLightDimmers); empty outside scheme 4.
			std::vector<std::pair<const RE::BSLight*, float>> shadowDimmers;
		};
		Inputs SampleInputs(const RE::UI3DSceneManager& a_manager, const RE::NiCamera& a_camera, const PreviousCamera& a_previous, bool a_shadowMaps);
		/** @brief The inputs a scene node and a camera give, with a_hdrOverride the ImageSpace HDR row to light by (null: the current data's). */
		Inputs SampleSceneInputs(const RE::ShadowSceneNode* a_sceneNode, const RE::NiCamera& a_camera, const PreviousCamera& a_previous, const float* a_hdrOverride);

		/**
		 * @brief A root of the local map's culls. A cell's node children (FUN_140243ea0) are culled by the frustum (cull mode 3) from their
		 * children down, shown whatever their hidden flag and their stale bound. The other roots take the local map process's own mode,
		 * which keeps every object outside hidden subtrees (observed: ObjectLODRoot's whole contents and every fog of war tile draw).
		 */
		struct LocalMapRoot
		{
			RE::NiAVObject* object = nullptr;
			bool cell = false;
		};

		/** @brief What LocalMapRenderer::Render (AE 16335, SE 16094) draws from: its camera, its roots, the fog of war's root. */
		struct LocalMapScene
		{
			RE::NiCamera* camera = nullptr;
			RE::NiAVObject* fogRoot = nullptr;  // renderer +0x30358, culled into the fog of war batch
			bool fogOfWar = false;              // the global AE 0x141fd6f46: the fog of war batch runs
			bool water = false;                 // water draws: an exterior that is not a small world (the render's 0x142033510)
			std::vector<LocalMapRoot> roots;    // the land and objects batch's
		};
		/** @brief The local map's camera and roots, read from the renderer and the loaded cells as its render reads them. */
		LocalMapScene CollectLocalMap(const void* a_renderer);

		/**
		 * @brief The geometry the local map's culls keep: under a cell root the frustum (cull mode 3, no multibounds) by DCLF's SIMD sphere
		 * cull over each level, elsewhere everything; hidden objects are left out, a switch node gives its active child, a billboard turns
		 * to the camera.
		 */
		void LocalMapVisible(std::span<const LocalMapRoot> a_roots, const RE::NiCamera& a_camera, std::vector<RE::BSGeometry*>& a_out);

		/** @brief The pass a visible geometry makes in a local map batch. */
		enum class LocalMapPass : std::uint8_t
		{
			None,
			Utility,  // a Lighting property's base texture (render mode 0x12) or fog of war (0x13) Utility pass
			Land,     // a landscape Lighting property's own Lighting passes
			Water,    // BSWaterShader's local map pass (technique LOD)
			Tree,     // BSDistantTreeShader's colour pass
		};
		LocalMapPass LocalMapPassOf(const RE::BSGeometry& a_geometry, View a_view, bool a_water);

		/** @brief The render's tree detail: each loaded tree's LOD switch shows child 1 (FUN_140438580(BSTreeManager, 1)). */
		void SetLocalMapTreeDetail();

		/** @brief The inputs of the local map's Lighting draws: the world's scene node and the local map camera, with no previous camera. */
		Inputs SampleLocalMapInputs(const RE::NiCamera& a_camera);

		/**
		 * @brief The water port's frame inputs as BSWaterShader sees them in the local map's render (AE 0x140242e00): the local map camera's
		 * posAdjust, matrices and planes, no previous camera, the accumulator's zero eye, render mode 0x12 with the local map's pass branch,
		 * no raw water target.
		 */
		Water::Port::FrameInputs SampleLocalMapWater(const Inputs& a_in, bool a_water);

		/** @brief The FrameBuffer (VS_PerFrame, b12) rows SetCameraData writes for the menu camera. */
		void FrameRows(const Inputs& a_in, float (*a_out)[4]);

		/** @brief What a draw needs besides its constants: the states the engine's batch leaves for it. */
		struct DrawState
		{
			std::uint32_t blend = 0, write = 1, depth = 3, cull = 1, depthBias = 0;
			std::uint32_t stencil = 2, stencilRef = 0xff;  // the colour pass writes stencil bit 0 everywhere it draws
			float alphaRef = 0.0f;
		};
		/** @brief A menu draw's viewport: the target's, with the depth range 0..1 (decision 148) less its depth bias mode's offset. */
		Family::Viewport MenuViewport(std::int32_t a_target, const DrawState& a_state);

		/** @brief A local map water draw: the latch rows, descriptors, states and the normal map LOD reads. */
		struct WaterResult
		{
			Water::DrawConstants constants{};
			std::uint32_t descriptor = 0, vertex = 0, pixel = 0;
			DrawState state;
			struct Slot
			{
				ID3D11ShaderResourceView* view = nullptr;
				std::uint32_t address = 0, filter = 0;
			};
			Slot normals;  // t4
			const RE::BSShader* shader = nullptr;
			std::uint64_t vertexDesc = 0;  // CS's vertex shader's input description
		};
		/** @brief The engine's water shader object (GetRenderPasses' pass shader). Null before the engine makes it. */
		RE::BSShader* WaterShader();
		/**
		 * @brief The water pass a_geometry makes in the local map (BSWaterShaderProperty::GetRenderPasses' local map branch, AE 0x1414c02e0),
		 * with BSWaterShader's Setup functions through the water port. False, with why, when it cannot.
		 */
		bool ComputeWater(const Water::Port::FrameInputs& a_frame, const RE::BSGeometry& a_geometry, WaterResult& a_out, std::string& a_why);

		/** @brief One pass of a menu scene in draw order: a geometry's own pass, or the membrane pass its Lighting property adds. */
		struct ScenePass
		{
			RE::BSGeometry* geometry = nullptr;
			bool membrane = false;
		};
		/**
		 * @brief A menu root's visible passes in draw order: the batches' in scene order, the membrane group, then the blended list sorted
		 * far to near.
		 */
		void SceneGeometry(RE::NiAVObject* a_root, const RE::NiCamera& a_camera, bool a_closestPoint, std::vector<ScenePass>& a_out);
		/** @brief Whether the menu's colour accumulator sorts its blended list by each bound's closest point. */
		bool SortsByClosestPoint(const RE::UI3DSceneManager& a_manager);

		/** @brief What a Utility draw of a menu model is drawn for: the depth pass with the menu camera, or a shadow light's map. */
		struct UtilityView
		{
			View view = View::MenuDepth;
			std::uint32_t renderMode = 0;              // the accumulator's render mode: the technique's mode bits for a shadow map
			RE::NiPoint3 eye{};                        // the camera's eye, SetCameraData's posAdjust
			std::array<float, 2> parabola{};           // ParabolaParam for a paraboloid's map: 1 / radius, and 1 or -1 by hemisphere
			std::uint32_t slice = 0;                   // the shadow map slice it draws into
			std::uint32_t textureTransformBuffer = 0;  // the material's TexcoordOffset buffer (MaterialFrame)
			Family::Viewport viewport;
			float frame[kFrameRows][4]{};  // FrameBuffer (b12) as SetCameraData writes it for the camera
		};

		/**
		 * @brief Appends a_geometry's Utility draw in a_view to a_run (none when it has no pass there). Null when added or when it has none,
		 * else why not; LeftOut tells the reasons that leave a draw out of this render only.
		 */
		const char* AddUtility(const UtilityView& a_view, const RE::BSGeometry& a_geometry, Run& a_run, std::string& a_detail);
		/** @brief Whether a draw's refusal only leaves it out of this render: a program still building, no buffers yet, a streaming texture. */
		bool LeftOut(const char* a_why);
		/**
		 * @brief A draw's refusal (decision 149): one that only leaves it out of this render (LeftOut) is logged when a_leftOut counts its
		 * first, any other is fatal. a_scene names the scene for the log, a_batch the draw's batch.
		 */
		void Refuse(const char* a_scene, std::string_view a_batch, const RE::BSGeometry& a_geometry, const char* a_why, const std::string& a_detail,
			std::uint64_t& a_leftOut);

		/** @brief FrameBuffer rows for a camera: SetCameraData's camera rows, no previous camera, the frame's ratios. */
		void UtilityFrameRows(const RE::NiCamera& a_camera, float (*a_out)[4]);

		/**
		 * @brief A loading model's shadow lights, depth pass and shadow masks for a native render of scheme 4 with its depth pass, in
		 * UI3DSceneManager::Render's order (MenuScenesShadows.cpp): FUN_140973210's light loop by CS's ports (camera update, keep, mask
		 * index, scissor, Accumulate's bookkeeping, Render's targets, slices and light transforms) with each map drawn from the menu
		 * geometry, then the depth pass of a_passes (the colour pass's SceneGeometry) into kMAIN's depth, then the menu masks. Fatal when a
		 * draw CS cannot compute is met.
		 */
		void RenderShadowsAndDepth(const RE::UI3DSceneManager& a_manager, const RE::NiCamera& a_camera, RE::NiAVObject* a_root, const Inputs& a_in,
			std::span<const ScenePass> a_passes);

		/**
		 * @brief LocalMapRenderer::Render (AE 16335) in native mode: the land and objects, their water and the fog of war computed from the
		 * scene (CollectLocalMap, LocalMapVisible) into kLOCAL_MAP_SWAP, then ISLocalMap staged into kLOCAL_MAP. No engine render function
		 * runs. Fatal when a draw CS cannot compute is met (decision 149).
		 */
		void RenderLocalMap(const void* a_renderer);
		/** @brief Render thread, at setup: queues the programs the local map's draws take, so its first render draws them all. */
		void QueueLocalMapPrograms();

		/** @brief Whether CS draws a light scheme's menu scene natively (native mode, a scheme whose draws CS computes). */
		bool Owns(std::int32_t a_scheme);

		/**
		 * @brief UI3DSceneManager::Render (AE 52727) in native mode for a scheme CS owns: its update half (lights, the root's update, the
		 * scene node's light update) as the engine runs it, then the colour pass CS builds from the scene graph into its own frame.
		 */
		DWORD Render(RE::UI3DSceneManager* a_manager, std::int32_t a_scheme, RE::NiCamera* a_camera, bool a_keepDepth);
	}

	namespace SceneDraws
	{
		/** @brief Whether the anchor and runtime are up. False for good once a client failed. */
		bool Available();
		/** @brief Makes the stages, programs and clients. Main thread. */
		bool Setup();
		/** @brief Whether a_view's batch is taken now. Its targets must still be its client's imports. */
		bool CanTake(View a_view);
		/** @brief How a_view's client reads the published id a bound slot exports. */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key, one per shader. Null when it cannot be built. */
		std::shared_ptr<const void> UtilityProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building = nullptr);
		/** @brief a_building, when given, is set when null means the program is still building on the program service. */
		std::shared_ptr<const void> LightingProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building = nullptr);
		std::shared_ptr<const void> EffectProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building = nullptr);
		std::shared_ptr<const void> WaterProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building = nullptr);
		/** @brief The format of a_view's import a_index, or 0 without one. */
		std::uint32_t ColourFormat(View a_view, std::uint32_t a_index);
		std::uint32_t DepthFormat(View a_view);
		/** @brief Keeps a batch's imports and leases alive when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor. True when submitted. */
		bool Submit(std::shared_ptr<Run> a_run);
		/**
		 * @brief Hands a run to its view's client, or keeps its imports alive when the client cannot take it now. A run past its view's
		 * draw limit is fatal. True when submitted.
		 */
		bool Hand(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
