#pragma once

#include "RenderGraph/BufferRef.h"

// Menu scenes as an engine draw family (docs/development/engine/menus.md). The engine culls, registers and sets up every pass as
// usual, DrawTriShape latches each draw, and an ORG pass at the EngineDraws anchor draws the batch in engine order.
// Batches are the local map (LocalMapRenderer::Render, SE 16094, AE 16335) with its fog of war, and a menu model's depth and
// colour passes (UI3DSceneManager::Render, SE 51855, AE 52727). In local map mode a Lighting property makes a BSUtilityShader
// RENDER_BASE_TEXTURE pass instead (SE 0x1412c7cd0), while landscape keeps its Lighting passes.
// Programs are ORG builds of Utility.hlsl, Lighting.hlsl and Effect.hlsl, with the world's CS features off. A draw with no ORG
// build, such as tree LOD, or a refused draw ends its batch's latched part, and the engine draws the rest.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPortFrame.h"
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

	/** @brief The most draws and latch bytes one batch takes. Past either, the rest of the batch is the engine's. */
	inline constexpr std::uint32_t kMaxDraws = 2048;
	inline constexpr std::uint32_t kLatchBytes = 1u << 20;

	enum class Shader : std::uint8_t
	{
		Utility,
		Lighting,
		Effect,
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
			if (offset + a_bytes.size() > kLatchBytes)
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
			// The scene node's shadow lights' dimmers as MistMenu's shadow render sets them (ShadowLightDimmers); empty outside scheme 4.
			std::vector<std::pair<const RE::BSLight*, float>> shadowDimmers;
		};
		Inputs SampleInputs(const RE::UI3DSceneManager& a_manager, const RE::NiCamera& a_camera, const PreviousCamera& a_previous, bool a_shadowMaps);

		/** @brief A draw's lights as GetRenderPasses gives a menu pass: the scene node's sun, its shadow lights, then its point lights. */
		struct Lights
		{
			std::array<RE::BSLight*, 16> lights{};
			std::uint32_t count = 0;
			std::uint32_t shadow = 0;
		};
		void SelectLights(const Inputs& a_in, const RE::BSGeometry& a_geometry, Lights& a_out);

		/** @brief The FrameBuffer (VS_PerFrame, b12) rows SetCameraData writes for the menu camera. */
		void FrameRows(const Inputs& a_in, float (*a_out)[4]);

		/** @brief A Lighting draw's constants, which floats of them the engine writes (1.0), and its descriptors and material record. */
		struct LightingResult
		{
			LightingSceneConstants constants{};
			LightingSceneConstants written{};
			std::uint32_t pass = 0, vertex = 0, pixel = 0, rawPixel = 0;
			MaterialRecord record{};
			Lights lights;
		};
		/** @brief The Lighting draw of a_geometry as SetupTechnique, SetupMaterial and SetupGeometry would bind it. False, with why, when it cannot. */
		bool ComputeLighting(const Inputs& a_in, const RE::BSGeometry& a_geometry, LightingResult& a_out, std::string& a_why);

		/** @brief An Effect draw's constants, written floats, descriptors and material record. */
		struct EffectResult
		{
			EffectSceneConstants constants{};
			EffectSceneConstants written{};
			std::uint32_t pass = 0, vertex = 0, pixel = 0, rawPixel = 0;
			MaterialRecord record{};
		};
		/** @brief The Effect draw of a_geometry as BSEffectShader's Setup functions would bind it. Particles and the lit, membrane and soft variants are refused. */
		bool ComputeEffect(const Inputs& a_in, const RE::BSGeometry& a_geometry, EffectResult& a_out, std::string& a_why);

		/** @brief A menu root's visible geometry in draw order: the batches' in scene order, then the blended list sorted far to near. */
		void SceneGeometry(RE::NiAVObject* a_root, const RE::NiCamera& a_camera, bool a_closestPoint, std::vector<RE::BSGeometry*>& a_out);
		/** @brief Whether a_geometry's pass goes to the accumulator's blended list (accumulation hint 1). */
		bool BlendedList(const RE::BSGeometry& a_geometry);
		/** @brief Whether the menu's colour accumulator sorts its blended list by each bound's closest point. */
		bool SortsByClosestPoint(const RE::UI3DSceneManager& a_manager);

		/** @brief CS's permutation block for a draw: the extra shader descriptor and Linear Lighting's two words, as CS's hooks set them. */
		struct Permutation
		{
			std::uint32_t extra = 0, baseTextureIsWorking = 0, renderToUI = 0;
		};
		Permutation PermutationOf(const RE::BSGeometry& a_geometry, bool a_effect);

		/**
		 * @brief A skinned geometry's palettes as VS b9 (previous) and b10 (current) hold them, after the engine's per-frame palette update.
		 * False without skin or with more bones than the palettes hold.
		 */
		bool BoneRows(const RE::BSGeometry& a_geometry, std::span<std::byte, kBonesBytes> a_out);

		/**
		 * @brief Whether CS draws a light scheme's menu scene natively (native mode, a scheme whose draws CS computes), for a render with or
		 * without its depth pass (a_keepDepth, UI3DSceneManager::Render's fourth argument).
		 */
		bool Owns(std::int32_t a_scheme, bool a_keepDepth);
		/** @brief Notes a render's scheme and depth pass flag, for the detector's ownership (MenuViews::NativeOwns). Main thread. */
		void NoteRender(std::int32_t a_scheme, bool a_keepDepth);

		/**
		 * @brief UI3DSceneManager::Render (AE 52727) in native mode for a scheme CS owns: its update half (lights, the root's update, the
		 * scene node's light update) as the engine runs it, then the colour pass CS builds from the scene graph into its own frame.
		 */
		DWORD Render(RE::UI3DSceneManager* a_manager, std::int32_t a_scheme, RE::NiCamera* a_camera, bool a_keepDepth);
	}

	/** @brief Temporary observe step of the native menu scenes (MenuScenesObserve.cpp): CS's draws against the engine's. */
	namespace Observe
	{
		bool Enabled();
		void Begin(View a_view, RE::NiCamera* a_camera);
		void End();
		void Other(const char* a_shader, const RE::BSRenderPass* a_pass, std::uint32_t a_vertex, std::uint32_t a_pixel);
		void Lighting(const RE::BSRenderPass* a_pass, const LightingSceneConstants& a_engine, const float (*a_frame)[4], const ID3D11ShaderResourceView* const* a_views,
			const std::uint32_t* a_address, const std::uint32_t* a_filter, std::uint32_t a_vertex, std::uint32_t a_pixel);
		void Effect(const RE::BSRenderPass* a_pass, const EffectSceneConstants& a_engine);
		void State(const RE::BSRenderPass* a_pass, std::uint32_t a_blend, std::uint32_t a_coverage, std::uint32_t a_write, std::uint32_t a_extra, std::uint32_t a_depth,
			std::uint32_t a_stencil, std::uint32_t a_stencilRef, std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_depthBias, float a_alphaRef,
			const Family::Viewport& a_viewport, std::int32_t a_target);
		void Bones(const RE::BSRenderPass* a_pass, std::span<const std::byte> a_engine);
		void Report();
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
		std::shared_ptr<const void> UtilityProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief a_building, when given, is set when null means the program is still building on the program service. */
		std::shared_ptr<const void> LightingProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building = nullptr);
		std::shared_ptr<const void> EffectProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building = nullptr);
		/** @brief The format of a_view's import a_index, or 0 without one. */
		std::uint32_t ColourFormat(View a_view, std::uint32_t a_index);
		std::uint32_t DepthFormat(View a_view);
		/** @brief Keeps a batch's imports and leases alive when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor. True when submitted. */
		bool Submit(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
