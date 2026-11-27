#pragma once

// The water port: every BSWaterShader constant computed from engine objects without the engine's setup.
// BSWaterShader SetupTechnique, SetupMaterial and SetupGeometry (SE 100601-100605, AE 107362-107365) and
// BSWaterShaderProperty::GetRenderPasses (SE 100238, AE 106945) as pure functions, with the rewrites CS's own hooks apply.
// The outputs are split by scope: a material row,
// an object row, a frame row, and the object-by-frame geometry rows. Assemble puts them in the latch's layout (Water.h DrawConstants).
// DCLF's water rows and the batch hooks' computed draws (WaterNative.cpp) both come from them.
// The captures read the engine on the render thread. Everything else is pure.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"
#include "Snapshot/FrameSnapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Features::Water::Port
{
	// Address Library ids of what the setup functions read. The comments give SE 1.5.97's addresses.
	namespace Address
	{
		inline constexpr Engine::Offset kViewProjJittered{ { 391664 }, { 513713 } };    // bool 0x141e103a0: WorldViewProj uses the jittered view-projection
		inline constexpr Engine::Offset kSunDir{ { 415113 }, { 528168 } };              // float[4] 0x143233640
		inline constexpr Engine::Offset kSunColor{ { 415117 }, { 528172 } };            // float[4] 0x143233650
		inline constexpr Engine::Offset kRenderWidth{ { 411453 }, { 524972 } };         // std::uint32_t 0x14302bb3c
		inline constexpr Engine::Offset kRenderHeight{ { 411454 }, { 524973 } };        // std::uint32_t 0x14302bb40
		inline constexpr Engine::Offset kRawWater{ { 415112 }, { 528167 } };            // bool 0x143233639: the surface draws into kRAW_WATER
		inline constexpr Engine::Offset kRenderMode{ { 414601 }, { 527651 } };          // std::uint32_t 0x1431d0e28: 0x12 is the local map
		inline constexpr Engine::Offset kWaterLighting{ { 414848 }, { 527901 } };       // float[4] 0x143232d18: multiplies the water colours
		inline constexpr Engine::Offset kDefaultFogColor{ { 410515 }, { 523935 } };     // float[3] 0x1430135f0: the fog colours without a fog property
		inline constexpr Engine::Offset kSsrIntensity{ { 391666 }, { 513715 } };        // float 0x141e103b0: SSRParams.x while SSR is on
		inline constexpr Engine::Offset kSsrBlur{ { 391669 }, { 513717 } };             // float 0x141e103c8: SSRParams.y
		inline constexpr Engine::Offset kSsrPerturbation{ { 391672 }, { 513719 } };     // float 0x141e103e0: SSRParams2.x
		inline constexpr Engine::Offset kDefaultLightColor{ { 388604 }, { 512012 } };   // float[4] 0x141e06460: LightColor of a light that does not affect water
		inline constexpr Engine::Offset kFlowmapSource{ { 414616 }, { 527694 } };     // NiPointer<NiSourceTexture>: SetupGeometry binds its view at t8
		inline constexpr Engine::Offset kFlowmapSize{ { 414596 }, { 527644 } };         // std::int32_t 0x1431d0e10 (Unified Water's gFlowMapSize)
		inline constexpr Engine::Offset kMeshFlowCellOffset{ { 415109 }, { 528164 } };  // float[2] 0x143233630 (Unified Water's gDisplacementMeshFlowCellOffset)
		inline constexpr Engine::Offset kDisplacementOffset{ { 415129 }, { 528184 } };  // float[4] 0x143233680 (Unified Water's gDisplacementCellTexCoordOffset)
		inline constexpr Engine::Offset kCameraUnderwater{ { 415111 }, { 528166 } };    // bool 0x143233638: TESWaterSystem::SetUnderwaterVariables writes it
		inline constexpr Engine::Offset kWaterEnabled{ { 391120 }, { 513342 } };        // bool 0x141e0e350: TESWaterSystem::Enable writes it
		inline constexpr Engine::Offset kLocalMapPasses{ { 414746 }, { 527793 } };      // bool 0x1431d1d30: GetRenderPasses takes the local map's path
		inline constexpr Engine::Offset kWaterLOD{ { 402322 }, { 516171 } };            // NiNode* 0x142f23e98: BGSTerrainManager's "WaterLOD" node
		inline constexpr Engine::Offset kReflectionTimer{ { 390953 }, { 513213 } };     // float 0x142033080: SetupMaterial's ReflectionColor.w
	}

	// The engine's variable indices, as BSWaterShader writes them through the bound shader's constant table (VS at +0x50 + i,
	// PS at +0x40 + i). ShaderCache.cpp gives Water.hlsl's names the same indices.
	enum class VsVariable : std::uint8_t
	{
		WorldViewProj,
		World,
		PreviousWorld,
		QPosAdjust,
		ObjectUV,
		NormalsScroll0,
		NormalsScroll1,
		NormalsScale,
		VSFogParam,
		VSFogNearColor,
		VSFogFarColor,
		CellTexCoordOffset,
		Count
	};

	enum class PsVariable : std::uint8_t
	{
		TextureProj,
		ShallowColor,
		DeepColor,
		ReflectionColor,
		FresnelRI,
		BlendRadius,
		PosAdjust,
		ReflectPlane,
		CameraDataWater,
		ProjData,
		VarAmounts,
		FogParam,
		FogNearColor,
		FogFarColor,
		SunDir,
		SunColor,
		NumLights,  // not in Water.hlsl: its table entry is 0, so the engine's write lands on PS b2's first float
		LightPos,
		LightColor,
		WaterParams,
		DepthControl,
		SSRParams,
		SSRParams2,
		NormalsAmplitude,
		VPOSOffset,
		Count
	};

	/** @brief Where a variable sits in Water.hlsl's packoffsets: its group (b0-b2), first row and row count. */
	struct Placement
	{
		std::uint8_t group = 0;
		std::uint8_t row = 0;
		std::uint8_t rows = 0;
		bool present = true;  // false: the D3D11 shader has no such variable
	};
	Placement PlacementOf(VsVariable a_variable);
	Placement PlacementOf(PsVariable a_variable);
	/** @brief A variable's name in Water.hlsl. */
	std::string_view VsName(VsVariable a_variable);
	std::string_view PsName(PsVariable a_variable);

	/**
	 * @brief A bound shader's constant tables: each variable's first float in its group. CS's reflection gives a variable the
	 * shader lacks offset 0, so the engine's write to it lands on the group's first float.
	 */
	struct ConstantTables
	{
		std::array<std::int8_t, 20> vs{};
		std::array<std::int8_t, 64> ps{};
	};
	/** @brief The tables of a D3D11 Water.hlsl that has every variable: its packoffsets, NumLights at 0. */
	ConstantTables CanonicalTables();

	/** @brief One float4 row as the engine writes it. written has a bit per component: the others keep whatever the buffer held. */
	struct Row
	{
		std::array<float, 4> v{};
		std::uint8_t written = 0;
	};

	// The pass descriptor bits (ShaderCache::WaterShaderFlags) and techniques.
	namespace Flag
	{
		inline constexpr std::uint32_t kVc = 1u << 0;
		inline constexpr std::uint32_t kNormalTexCoord = 1u << 1;
		inline constexpr std::uint32_t kReflections = 1u << 2;
		inline constexpr std::uint32_t kRefractions = 1u << 3;
		inline constexpr std::uint32_t kDepth = 1u << 4;
		inline constexpr std::uint32_t kInterior = 1u << 5;
		inline constexpr std::uint32_t kWading = 1u << 6;
		inline constexpr std::uint32_t kVertexAlphaDepth = 1u << 7;
		inline constexpr std::uint32_t kCubemap = 1u << 8;
		inline constexpr std::uint32_t kFlowmap = 1u << 9;
		inline constexpr std::uint32_t kBlendNormals = 1u << 10;
	}
	inline constexpr std::uint32_t kTechniqueUnderwater = 8;
	inline constexpr std::uint32_t kTechniqueLod = 9;
	inline constexpr std::uint32_t kTechniqueStencil = 10;
	inline constexpr std::uint32_t kTechniqueSimple = 11;
	inline constexpr std::uint32_t kSurfacePassBase = 0x5C000071;

	/** @brief SetupTechnique's switch: the raw descriptor of a pass id. */
	std::uint32_t RawDescriptor(std::uint32_t a_passEnum);
	/** @brief The technique of a raw descriptor: 0 surface, 1-7 lights, 8 underwater, 9 LOD, 10 stencil, 11 simple. */
	constexpr std::uint32_t TechniqueOf(std::uint32_t a_descriptor) { return (a_descriptor >> 11) & 0xF; }
	/** @brief How many lights a light technique carries: the technique when below 8, else 0. */
	constexpr std::uint32_t LightCountOf(std::uint32_t a_descriptor) { return TechniqueOf(a_descriptor) < 8 ? TechniqueOf(a_descriptor) : 0; }

	/** @brief The per-frame engine state the setup functions read. Captured once a frame on the render thread. */
	struct FrameInputs
	{
		std::uint32_t frame = 0;
		// RendererShadowState
		std::array<float, 3> posAdjust{};
		std::array<float, 3> previousPosAdjust{};
		std::array<float, 16> viewProj{};  // the jittered view-projection or the unjittered one, as kViewProjJittered picks
		float proj00 = 0.0f, proj11 = 0.0f;
		// BSShaderManager::State
		float invFrameBufferRange = 0.0f;  // +0x9C: PosAdjust.w, and 1 / it is WaterParams.w
		float timer = 0.0f;                // Address::kReflectionTimer: ReflectionColor.w
		bool interior = false;             // +0x44: the Interior descriptor bit
		// The camera's planes (NiCamera +0x160 / +0x164)
		float cameraNear = 0.0f, cameraFar = 0.0f;
		std::array<float, 4> sunDir{}, sunColor{};
		std::uint32_t renderWidth = 0, renderHeight = 0;
		// Screen-space reflections: ImageSpaceManager +0x1C0's effect, its +0x10 enabled byte, and the INI values
		bool ssrEnabled = false;
		float ssrIntensity = 0.0f, ssrBlur = 0.0f, ssrPerturbation = 0.0f;
		std::array<float, 4> waterLighting{};
		// The scene graph's fog: ShadowSceneNode[State.sceneGraph]->fogProperty
		bool hasFog = false;
		std::array<float, 3> fogNearColor{}, fogFarColor{};  // the default colour without a fog property
		float fogNear = 0.0f, fogFar = 0.0f, fogPower = 0.0f, fogClamp = 0.0f;
		std::array<float, 4> defaultLightColor{};
		// Flowmap globals as the engine holds them. Unified Water rewrites the first two per draw (unifiedWater below).
		std::int32_t flowmapSize = 0;
		std::array<float, 2> meshFlowCellOffset{};
		std::array<float, 4> displacementOffset{};
		// BGSTerrainManager's "WaterLOD" node, which vanilla LOD water and Unified Water's tiles hang under
		const RE::NiAVObject* waterLodRoot = nullptr;
		// The main camera accumulator's eye position (ReflectPlane.w)
		std::array<float, 3> eyePosition{};
		// GetRenderPasses' globals and the surface's target
		bool cameraUnderwater = false;
		bool waterEnabled = false;
		bool localMapPasses = false;
		bool rawWater = false;
		std::uint32_t renderMode = 0;
		// CS's hooks
		bool linearLighting = false;  // Linear Lighting rewrites the colours (WaterMaterialUpload, WaterGeometryUpload)
		struct Flowmap
		{
			bool active = false;  // Unified Water's SetupGeometry hook rewrites (UnifiedWater::GetFlowRewrite)
			std::int32_t width = 0, height = 0, offsetX = 0, offsetY = 0;
			bool operator==(const Flowmap&) const = default;
		} unifiedWater;
		bool physicalSkySampler = false;  // Physical Sky's ModifyWater replaces sampler s13
		bool cubemapFixInterior = false;  // the cubemap fix's interior test: TES's interior cell or the player's cell
		bool mapMenuOpen = false;         // the cubemap fix leaves the map menu alone
		bool operator==(const FrameInputs&) const = default;
	};

	/**
	* @brief Native: the FrameInputs fields that are the game's (ReadGame), taken once per update in the snapshot capture after the
	* water update, so the render side reads none of them from the engine (render-thread roadmap phase 7).
	*/
	struct GameBlock
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::WaterPort;
		FrameInputs inputs;
		bool operator==(const GameBlock&) const = default;
	};

	/** @brief Game thread, native: the snapshot capture of GameBlock. */
	void CaptureGame(Snapshot::Builder& a_builder);

	/**
	* @brief Render thread. The frame's FrameInputs: in native the game's fields from the adopted snapshot's GameBlock and the view's
	* from the render side; in guest every field read now. a_accumulator is the batch's (the main camera's).
	*/
	FrameInputs CaptureFrame(const RE::BSShaderAccumulator* a_accumulator);

	/**
	* @brief The game half's FrameInputs, for the material rows. Native: the game's fields from the snapshot being built (GameBlock)
	* and CS's hooks; the view's fields stay zero, since the render side owns the camera and the target size. Guest: CaptureFrame.
	* Game thread.
	*/
	FrameInputs CaptureGameFrame(const RE::BSShaderAccumulator* a_accumulator);

	/** @brief SetupTechnique's rows, a frame row once the descriptor's Depth bit is known: VS b0 and PS b0. */
	struct TechniqueRows
	{
		Row qPosAdjust;                                           // VS b0 c0
		Row vposOffset, posAdjust, cameraData, sunDir, sunColor;  // PS b0 c0-c4. VPOSOffset only with the Depth bit
	};
	TechniqueRows EvaluateTechnique(std::uint32_t a_descriptor, const FrameInputs& a_frame);

	/** @brief BSWaterShaderMaterial's bytes, as SetupMaterial reads them (0x180). */
	inline constexpr std::size_t kMaterialBytes = 0x180;
	using MaterialBytes = std::span<const std::byte, kMaterialBytes>;
	MaterialBytes BytesOf(const RE::BSWaterShaderMaterial& a_material);

	/**
	 * @brief SetupMaterial's rows: VS b1 and PS b1, then Linear Lighting's colour rewrite. The material record times the frame:
	 * the fog, the water lighting, the timer, the framebuffer range and SSR come from the frame (plan section 2.1).
	 */
	struct MaterialRow
	{
		std::array<Row, 6> vs;   // VSFogParam, VSFogNearColor, VSFogFarColor, NormalsScroll0, NormalsScroll1, NormalsScale
		std::array<Row, 14> ps;  // Water.hlsl's PS b1 rows c0-c13
	};
	MaterialRow EvaluateMaterial(MaterialBytes a_material, const FrameInputs& a_frame);

	/** @brief What SetupGeometry reads from the property and the geometry. Fixed per water shape until an engine write. */
	struct ObjectInputs
	{
		std::uint32_t waterFlags = 0;                             // BSWaterShaderProperty +0x88
		std::uint32_t stencilBit = 0;                             // +0x8C: the water type's stencil bit (2..0x80)
		std::int32_t cellX = 0, cellY = 0, flowX = 0, flowY = 0;  // +0x90..+0x9C as the property holds them
		std::array<float, 4> plane{};                             // +0xA0: normal and constant
		RE::NiTransform world;
		float localScale = 1.0f;
		std::uint32_t vertexDescLow = 0;  // the geometry's VertexDesc low word (+0x148)
	};
	ObjectInputs CaptureObject(const RE::BSWaterShaderProperty& a_property, const RE::BSGeometry& a_geometry);

	/** @brief The per-object values: ObjectUV and CellTexCoordOffset (VS b2 c12, c13), after Unified Water's rewrite. */
	struct ObjectRow
	{
		Row objectUV;
		Row cellTexCoordOffset;             // written only with the Flowmap bit; the displacement mesh's (Wading) is the frame's
		std::int32_t flowX = 0, flowY = 0;  // what the property holds after Unified Water's hook
		std::int32_t cellX = 0, cellY = 0;
		std::uint32_t stencilBit = 0;
	};
	ObjectRow EvaluateObject(const ObjectInputs& a_object, std::uint32_t a_descriptor, const FrameInputs& a_frame);

	/** @brief SetupGeometry's object-by-frame rows: the matrices (VS b2 c0-c11, PS b2 c0-c3), ReflectPlane and ProjData. */
	struct GeometryRows
	{
		std::array<Row, 4> world, previousWorld, worldViewProj;
		std::array<Row, 4> textureProj;  // every technique but the light ones. TextureProj includes the object's world
		Row reflectPlane;                // xyz the property's plane, w its distance to the eye
		Row projData;                    // a frame value: the projection's [0][0] and [1][1]
	};
	GeometryRows EvaluateGeometry(const ObjectInputs& a_object, std::uint32_t a_descriptor, const FrameInputs& a_frame);

	/** @brief A light pass's lights (PS b2 c6-c21), after Linear Lighting's colour rewrite. Empty for other techniques. */
	struct PassLights
	{
		float numLights = 0.0f;  // NumLights.x, which aliases PS b2's first float in CS's shader
		bool any = false;
		std::array<Row, 8> lightPos, lightColor;
	};
	PassLights EvaluateLights(const RE::BSRenderPass& a_pass, MaterialBytes a_material, std::uint32_t a_descriptor, const FrameInputs& a_frame);

	/**
	 * @brief The six groups in the latch's layout (Water.h kGroupRows), with each component's written bit. Written through
	 * a_tables in the engine's order, so a write to a variable the shader lacks lands where the engine's does.
	 */
	struct Constants
	{
		std::array<Row, kGroupRows[0]> vsTechnique;
		std::array<Row, kGroupRows[1]> vsMaterial;
		std::array<Row, kGroupRows[2]> vsGeometry;
		std::array<Row, kGroupRows[3]> psTechnique;
		std::array<Row, kGroupRows[4]> psMaterial;
		std::array<Row, kGroupRows[5]> psGeometry;
	};
	Constants Assemble(const TechniqueRows& a_technique, const MaterialRow& a_material, const ObjectRow& a_object, const GeometryRows& a_geometry,
		const PassLights& a_lights, const ConstantTables& a_tables);

	/** @brief One pass GetRenderPasses makes. */
	struct PassSpec
	{
		std::uint32_t passEnum = 0;
		std::uint8_t accumulationHint = 0;
		bool present = false;
	};

	/** @brief GetRenderPasses for the main path: the pass list and each pass's raw descriptor, and CS's lookup descriptors. */
	struct RenderPasses
	{
		bool none = false;                    // the water system is off or the render mode takes no water passes
		bool localMap = false;                // the local map's path, not ported here
		std::uint32_t waterFlags = 0;         // the property's flags after kUnderwater follows the camera (GetRenderPasses writes bit 5)
		PassSpec surface;                     // technique 0, 8 (underwater) or 9 (LOD)
		PassSpec stencil;                     // the mask's: 0x5C00006D/6E, or 6F/70 when the surface has hint 13
		PassSpec lights;                      // technique = light count (1-7), only when lit and not underwater
		std::uint32_t surfaceDescriptor = 0;  // the surface pass's raw descriptor
		std::uint32_t lookupDescriptor = 0;   // CS's lookup of it (State::ModifyShaderLookup drops Reflections, Cubemap and Interior)
	};

	/**
	 * @brief GetRenderPasses (SE 100238, AE 106945) as a pure function. a_lightCount is what the property's light data returns
	 * for the pass's lights (the engine's FUN_141310e10), including the first entry: a light pass exists when it is above 1.
	 */
	RenderPasses DeriveRenderPasses(const ObjectInputs& a_object, std::uint32_t a_renderMode, std::uint32_t a_lightCount, const FrameInputs& a_frame);

	/** @brief The hooks that change what a water draw binds rather than its constants. */
	struct BindingRewrites
	{
		bool physicalSkySampler = false;  // Physical Sky binds its own sampler at s13 after SetupGeometry
		bool nullReflectionCube = false;  // the cubemap fix unbinds t3 when it holds kREFLECTIONS (interiors, not the map menu)
	};
	BindingRewrites EvaluateBindingRewrites(const FrameInputs& a_frame);
}
