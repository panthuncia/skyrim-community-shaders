#pragma once

// The scene's record types and their GPU layouts. The coordinator writes them into SceneTables, and the render thread's builds and
// the shaders read them. The HLSL layouts must match these.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <type_traits>
#include "RenderGraph/BufferRef.h"

struct ID3D11Buffer;

namespace DCLF
{
	/** @brief The object table's GPU layout, uploaded as is. 16-byte aligned with no implicit padding. */
	struct ObjectRecord
	{
		// The placement is not in the record. FrameValues samples it into the frame's rows.
		std::uint32_t geometryIndex;
		std::uint32_t materialIndex;
		std::uint32_t pipelineIndex;
		std::uint32_t flags;  // ObjectFlags, alpha-test threshold in bits 8-15
	};
	static_assert(sizeof(ObjectRecord) == 16);

	enum ObjectFlags : std::uint32_t
	{
		kObjectAlphaTest = 1u << 0,  // alpha test on, reference = threshold / 255 (as the native draw's AlphaTestRef)
		kObjectTwoSided = 1u << 1,   // no culling (the native main pass culls back faces otherwise)
		kObjectSuppressExternalEmittance = 1u << 2,  // ExtraShaderDescriptors::SuppressExternalEmittance in the permutation buffer
		// 1u << 3 belongs to a feature. See kFeatureObjectFlags.
		// Rows at BindlessObject::boneOffset / previousBoneOffset, for a skin's bone palette or a particle member's quads.
		// The shaders call it kObjectSkinned.
		kObjectPaletteBlock = 1u << 4,
		// A culling candidate only. materialIndex and pipelineIndex mean nothing, and even index 0 is unsafe because the tables may be empty.
		kObjectNoBindings = 1u << 5,
		// 1u << 6 belongs to a feature. See kFeatureObjectFlags.
		// Never an occluder. Drawn in the colour segment's second pass after the opaque draws, in its group's order (bits 20-21).
		kObjectDecal = 1u << 7,
		kObjectAlphaThresholdShift = 8,
		// Alpha-tested and blended. The depth pass uses its own alpha reference.
		kObjectAlphaBlended = 1u << 17,
		// Extra PerGeometry rows at BindlessObject::extraOffset, for ProjectedUV and MTLand.
		kObjectProjectedUV = 1u << 18,
		kObjectLandBlend = 1u << 19,
		kObjectDecalGroupShift = 20,
		// Without kObjectDecal, these bits are a terrain group while Terrain Blending holds the terrain. 1 is a landscape block,
		// 2 is a no-transparency mesh drawn after it with depth EQUAL.
		kObjectTerrainGroupShift = kObjectDecalGroupShift,
		// The engine draws no shadow-map pass for it, so no shadow view may accept it.
		kObjectNoShadow = 1u << 22,
		// The engine draws it only into the sun's volumetric lighting copy, so every other view skips it.
		kObjectVolumetricOnly = 1u << 23,
		// A record for the shadow epochs alone. Always with kObjectNoBindings.
		kObjectShadowOnly = 1u << 24,
		// A free slot. Always with kObjectNoBindings and kObjectNoShadow, so loops that skip those skip it too.
		kObjectFree = 1u << 25,
		// The colour BuildDraws tests its bound against the cascades and sets kObjectSunMiss on a miss, as GetRenderPasses would.
		kObjectSunTest = 1u << 26,
		// Dropped for good where BSFadeNode::OnVisible would snap its fade to 0.
		kObjectFadeTest = 1u << 27,
		// Dropped for good where BSTreeNode::OnVisible's height test would drop it.
		kObjectHeightTest = 1u << 28,
		// Gets only the local shadow lights that take landscape (BSShadowLight +0x61).
		kObjectLandscapeLights = 1u << 29,
		// Bound by scene membership. Every record with bindings has it.
		kObjectMember = 1u << 30,
		// A blended Lighting object. Never an occluder. Sorted back to front and shaded at the engine's blended point (FUN_1414b39d0).
		kObjectForward = 1u << 31,
	};

	/**
	 * @brief The object flag bits that features own. Features/Manifest.cpp checks them against Core's and each other's.
	 * Bits 27 and 28 are still declared in Core's enum until Features/Fades holds them.
	 */
	inline constexpr std::uint32_t kFeatureObjectFlags = (1u << 3) | (1u << 6) | (1u << 27) | (1u << 28);

	/** @brief The high bit of a draw's object-index word, set when the draw misses every sun cascade. */
	inline constexpr std::uint32_t kObjectSunMiss = 1u << 31;
	// The draw's local shadow lights in the object word, as Light Limit Fix's ShadowBitMask. The object index sits below them.
	inline constexpr std::uint32_t kObjectLocalShadowShift = 26;
	inline constexpr std::uint32_t kObjectLocalShadowMask = 0xFu << kObjectLocalShadowShift;
	inline constexpr std::uint32_t kObjectIndexMask = (1u << kObjectLocalShadowShift) - 1;

	/** @brief A stream row's float4 per vertex is bound as the second vertex stream, where the engine puts BSDynamicTriShape::dynamicData. */
	inline constexpr std::uint32_t kNoStreamRow = ~0u;

	/** @brief Rows of per-object extras in the row buffer: LandBlendParams, TextureProj x3, ProjectedUVParams x3. */
	inline constexpr std::uint32_t kExtraRows = 7;
	inline constexpr std::uint32_t kExtraRowLandBlend = 0;
	inline constexpr std::uint32_t kExtraRowTextureProj = 1;
	inline constexpr std::uint32_t kExtraRowProjectedParams = 4;
	inline constexpr std::uint32_t kNoExtraRows = ~0u;

	/**
	 * @brief A shadow-only record that also has kObjectDecal, which no other shadow-only record does. It is an actor's hidden part,
	 * such as the player's body in first person. Only focus views draw it, since the engine registers the focus subtree whole.
	 */
	inline constexpr std::uint32_t kObjectFocusOnly = kObjectShadowOnly | kObjectDecal;
	inline constexpr bool ObjectFocusOnly(std::uint32_t a_flags) { return (a_flags & kObjectFocusOnly) == kObjectFocusOnly; }

	inline constexpr std::uint32_t ObjectDecalGroup(std::uint32_t a_flags) { return (a_flags & kObjectDecal) ? (a_flags >> kObjectDecalGroupShift) & 3u : 0u; }
	inline constexpr std::uint32_t ObjectTerrainGroup(std::uint32_t a_flags) { return (a_flags & kObjectDecal) ? 0u : (a_flags >> kObjectTerrainGroupShift) & 3u; }

	/** @brief The last partition, or a TriShape that is not a partition. */
	inline constexpr std::uint32_t kNoPartition = ~0u;
	/** @brief One bit each in the draw's partition mask. */
	inline constexpr std::uint32_t kMaxSkinPartitions = 8;
	/** @brief The current LOD level draws no partition. Still a member, but no draw. */
	inline constexpr std::uint32_t kNoPartitions = 1u << kMaxSkinPartitions;
	/** @brief For an Object LOD chain, the low bits count the nextPartition links drawn. */
	inline constexpr std::uint32_t kPartitionChain = 1u << 15;
	inline constexpr std::uint32_t kPartitionChainCount = kPartitionChain - 1;
	/** @brief The slots a blood decal takes, one per partition up to the last its mask names. BuildDrawsCS writes each every frame. */
	inline constexpr std::uint32_t DecalSpanOf(std::uint32_t a_partitions)
	{
		if (a_partitions & kPartitionChain)
			return std::max(a_partitions & kPartitionChainCount, 1u);
		const std::uint32_t mask = a_partitions & (kNoPartitions - 1);
		return a_partitions == kNoPartitions || !mask ? 1u : static_cast<std::uint32_t>(std::bit_width(mask));
	}
	inline constexpr std::uint32_t PartitionDraws(std::uint32_t a_partitions)
	{
		if (a_partitions & kPartitionChain)
			return a_partitions & kPartitionChainCount;
		return a_partitions == kNoPartitions ? 0u : a_partitions ? static_cast<std::uint32_t>(std::popcount(a_partitions)) : 1u;
	}

	/** @brief Shared by every object that uses the same BSGraphics::TriShape. The buffers are the game's. */
	struct GeometryBuffers
	{
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		std::uint64_t vertexDesc = 0;  // BSGraphics::VertexDesc (stride, attribute offsets, flags)
		std::uint32_t vertexStride = 0;
		std::uint32_t vertexCount = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t firstIndex = 0;
		// 0 when the render graph is off.
		std::uint64_t vertexAddress = 0;
		std::uint64_t indexAddress = 0;
		RenderGraph::BufferRef vertexRef;
		RenderGraph::BufferRef indexRef;
		std::uint64_t vertexBytes = 0;
		std::uint64_t indexBytes = 0;
		// For a skin partition, the next partition slot whatever the frame's mask.
		std::uint32_t nextPartition = kNoPartition;
	};

	/** @brief Light Limit Fix's room index, the record's word 0. -1 until a join takes it. */
	struct ObjectAncestorValue
	{
		std::int32_t value = -1;
	};

	/**
	 * @brief Community Shaders' permutation buffer (b4) as BeginTechnique and the SetupGeometry hooks leave it.
	 * Per-object bits go on top of extraShaderDescriptor.
	 */
	struct PipelinePermutation
	{
		std::uint32_t vertexShaderDescriptor = 0;  // descriptor BeginTechnique receives
		std::uint32_t pixelShaderDescriptor = 0;   // received pixel descriptor bits the shader lookup dropped
		std::uint32_t extraShaderDescriptor = 0;
		std::uint32_t extraFeatureDescriptor = 0;
	};

	struct PipelineKey
	{
		std::uint32_t vertexDescriptor = 0;  // after State::ModifyShaderLookup, as the native draw uses
		std::uint32_t pixelDescriptor = 0;
		std::uint32_t rasterFlags = 0;     // PipelineRasterFlags
		std::uint32_t passDescriptor = 0;  // raw technique the Setup* functions read (selects the per-frame constants)
		std::uint64_t vertexLayout = 0;    // geometry's BSGraphics::VertexDesc without the stride (VertexInput.h)
		// The permutation block's runtime descriptors (PipelinePermutation's first two words). They follow from the pass and pixel
		// descriptors, and the pulled stages are compiled for them (ShaderPrograms::ProgramDescriptorsOf).
		std::uint32_t runtimeVertex = 0;
		std::uint32_t runtimePixel = 0;

		bool operator==(const PipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<PipelineKey>);

	/** @brief A separate key space, because a caster's Utility technique does not follow its Lighting descriptors. */
	struct ShadowPipelineKey
	{
		std::uint32_t technique = 0;     // Utility technique, mode bits included
		std::uint32_t rasterFlags = 0;   // kRasterTwoSided
		std::uint64_t vertexLayout = 0;  // as PipelineKey::vertexLayout
		std::uint32_t viewState = 0;     // the view's rasterizer state id; 0 for a caster's base key (a key slot)
		std::uint32_t reserved = 0;      // hashed as bytes: no padding

		bool operator==(const ShadowPipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<ShadowPipelineKey>);

	/**
	 * @brief Reduces a shadow Utility technique to the bits its depth-only output depends on, so more casters share a pipeline.
	 * Must match package/Shaders/Utility.hlsl. Occlusion map techniques are kept whole.
	 */
	inline std::uint32_t CanonicalShadowTechnique(std::uint32_t a_technique)
	{
		constexpr std::uint32_t kVc = 1u << 0, kTexture = 1u << 1, kSkinned = 1u << 2, kNormals = 1u << 3, kNoDefine = 0x70u, kAlphaTest = 1u << 7,
								kRenderDepth = 1u << 13, kRenderShadowmap = 1u << 14, kTreeAnim = 1u << 26, kLodObject = 1u << 27;
		if ((a_technique & kRenderShadowmap) == 0 || (a_technique & kRenderDepth) != 0)
			return a_technique;
		std::uint32_t technique = a_technique & ~kNoDefine;
		const bool vc = (technique & kVc) != 0, normals = (technique & kNormals) != 0, treeAnim = (technique & kTreeAnim) != 0;
		if ((technique & kAlphaTest) == 0) {
			technique &= ~(kTexture | kLodObject);
			if (!(vc && normals && treeAnim))
				technique &= ~(kVc | kNormals | kTreeAnim);
		} else if (!(vc && treeAnim)) {
			technique &= ~kNormals;
			if (!treeAnim)
				technique |= kVc;
		}
		return technique | kSkinned;
	}

	struct ShadowPipelineKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const ShadowPipelineKey& a_key) const noexcept
		{
			return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key));
		}
	};

	struct PipelineKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const PipelineKey& a_key) const noexcept
		{
			return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key));
		}
	};

	/**
	 * @brief Fixed-function state of a pipeline, packed.
	 *
	 * An opaque object sets at most bit 0. Decal, terrain and forward keys carry indices of the engine's state objects,
	 * which DrawPipelines reads back from the engine's tables (EngineStates.h) rather than re-deriving them.
	 */
	enum PipelineRasterFlags : std::uint32_t
	{
		kRasterTwoSided = 1u << 0,
		kRasterDecalGroupShift = 1,  // 2 bits: ObjectDecalGroup
		// Moves the decal toward the eye and draws its depth bias. Not an engine state index, so RasterStateBits leaves it out.
		kRasterDecalNudge = 1u << 3,
		// 4 bits of rasterStateDepthBiasMode. Only a kRasterDecalNudge key draws the bias.
		kRasterDepthBiasShift = 4,
		kRasterBlendModeShift = 8,   // 3 bits: alphaBlendMode (0-6)
		kRasterAlphaToCoverage = 1u << 11,
		kRasterWriteModeShift = 12,  // 4 bits: alphaBlendWriteMode (0-12)
		kRasterBlendExtra = 1u << 16,
		// An Effect shader key. Not an engine state index, so RasterStateBits leaves it out.
		kRasterEffect = 1u << 17,
		// A Water shader key (ProgramFamily::Water). Not an engine state index, so RasterStateBits leaves it out.
		kRasterWater = 1u << 18,
		// The next three bits mean different things by key family. On Lighting keys they belong to Api::LightingKeyBits' claimant.
		kRasterLightingSpareShift = 21,
		// On Effect keys, the billboard mode + 1, or 0 when not under an NiBillboardNode.
		kRasterEffectBillboardShift = kRasterLightingSpareShift,
		// On refraction keys, the billboard mode + 1 of a refraction leaf. Its Lighting pipeline is never drawn.
		kRasterRefractionBillboardShift = kRasterLightingSpareShift,
		// Group 1 blends with depth LESS_EQUAL and writes. Group 2 does not blend and tests EQUAL. No depth bias.
		kRasterTerrainGroupShift = 24,
		// Drawn after the deferred composite onto the main target alone, with the engine's blend and write modes.
		kRasterForward = 1u << 26,
		kRasterForwardDepthTest = 1u << 27,
		kRasterForwardDepthWrite = 1u << 28,
		// Always with kRasterForward. Draws Utility RENDER_NORMAL into the refraction normals instead of its Lighting pipeline.
		kRasterRefraction = 1u << 29,
		kRasterRefractionFalloff = 1u << 30,
		kRasterRefractionClamp = 1u << 31,
	};
	inline constexpr std::uint32_t kRasterLightingSpareMask = 7u << kRasterLightingSpareShift;

	inline constexpr std::uint32_t RasterDecalGroup(std::uint32_t a_flags) { return (a_flags >> kRasterDecalGroupShift) & 3u; }
	inline constexpr std::uint32_t RasterDepthBiasMode(std::uint32_t a_flags) { return (a_flags >> kRasterDepthBiasShift) & 15u; }
	inline constexpr std::uint32_t RasterBlendMode(std::uint32_t a_flags) { return (a_flags >> kRasterBlendModeShift) & 7u; }
	inline constexpr std::uint32_t RasterWriteMode(std::uint32_t a_flags) { return (a_flags >> kRasterWriteModeShift) & 15u; }
	inline constexpr std::uint32_t RasterTerrainGroup(std::uint32_t a_flags) { return (a_flags >> kRasterTerrainGroupShift) & 3u; }
	inline constexpr std::uint32_t RasterStateBits(std::uint32_t a_flags) { return a_flags & ~(kRasterTwoSided | kRasterLightingSpareMask | kRasterEffect | kRasterWater | kRasterDecalNudge); }
	inline constexpr std::uint32_t RasterLightingSpare(std::uint32_t a_flags) { return (a_flags & kRasterLightingSpareMask) >> kRasterLightingSpareShift; }

	inline constexpr std::uint32_t PackDecalRasterFlags(std::uint32_t a_group, std::uint32_t a_depthBiasMode, std::uint32_t a_blendMode,
		std::uint32_t a_writeMode)
	{
		return ((a_group & 3u) << kRasterDecalGroupShift) | ((a_depthBiasMode & 15u) << kRasterDepthBiasShift) |
		       ((a_blendMode & 7u) << kRasterBlendModeShift) | ((a_writeMode & 15u) << kRasterWriteModeShift);
	}

	inline constexpr std::uint32_t PackTerrainRasterFlags(std::uint32_t a_group, std::uint32_t a_blendMode, std::uint32_t a_writeMode)
	{
		return ((a_group & 3u) << kRasterTerrainGroupShift) | ((a_blendMode & 7u) << kRasterBlendModeShift) | ((a_writeMode & 15u) << kRasterWriteModeShift);
	}

	inline constexpr bool RasterForward(std::uint32_t a_flags) { return (a_flags & kRasterForward) != 0; }
	inline constexpr bool RasterRefraction(std::uint32_t a_flags) { return (a_flags & kRasterRefraction) != 0; }
	inline constexpr std::uint32_t PackRefractionRasterFlags(bool a_falloff, bool a_clamp)
	{
		return kRasterRefraction | (a_falloff ? kRasterRefractionFalloff : 0u) | (a_clamp ? kRasterRefractionClamp : 0u);
	}

	inline constexpr std::uint32_t PackForwardRasterFlags(std::uint32_t a_blendMode, std::uint32_t a_writeMode, bool a_depthTest, bool a_depthWrite)
	{
		return kRasterForward | ((a_blendMode & 7u) << kRasterBlendModeShift) | ((a_writeMode & 15u) << kRasterWriteModeShift) |
		       (a_depthTest ? kRasterForwardDepthTest : 0u) | (a_depthWrite ? kRasterForwardDepthWrite : 0u);
	}

	/** @brief Per-object PerGeometry pixel constants. Components the engine does not write for this object hold kUnwrittenBits. */
	struct ObjectShading
	{
		float materialData[4];  // MaterialData: envmap LOD fade, specular LOD fade, alpha
		float emitColor[3];     // EmitColor: emissive colour * emissive multiplier
		float ssrSpecular;      // SSRParams.w: specular LOD fade (0 when the pass disables it)
	};
	static_assert(sizeof(ObjectShading) == 32);


	/**
	 * @brief The occlusion maps DCLF draws from its tables instead of the engine's scene list cull: Skylighting's sky map and the
	 * precipitation mask. Both are views of the precipitation accumulator (render mode 0x1C).
	 */
	inline constexpr std::uint32_t kOcclusionViews = 2;
	inline constexpr std::uint32_t kOcclusionSky = 0;
	inline constexpr std::uint32_t kOcclusionPrecipitation = 1;


	/**
	 * @brief GPU fade state for a fade root, mirroring BSFadeNode's own fields (offsets below). FadeStateCS runs the node's OnVisible
	 * for the main camera. The C++ port is Features/Fades/FadeState.h.
	 */
	struct FadeNodeState
	{
		std::uint32_t flags = 0;          // +0xF4, only kFadeFlag* (bits 14, 15, 27)
		float currentFade = 1.0f;         // +0x130
		float snapRadius = 0.0f;          // +0x134: the bound radius at the placement snap (FUN_14147aa20)
		std::int32_t lastVisible = 0;     // +0x13C: the fade frame counter it was last in view
		float amountFade = 1.0f;          // +0x140: the last fadeAmount-driven fade
		float metric = 0.0f;              // +0x144: the LOD metric (GetRenderPasses' LOD fades)
		float previousMetric = 0.0f;      // +0x148
		float blend = 0.0f;               // +0x14C: the LOD cross-fade
		std::uint32_t levels = 0;         // +0x152 (levels: current, previous) | +0x153 (LOD type, transition) << 8
		std::uint32_t generation = 0;     // the static row's it was seeded from
		std::uint32_t verdict = 0;        // kFadeVerdict*: what its last update saw and decided
		std::uint32_t frame = 0;          // the scene frame of its last update
	};
	static_assert(sizeof(FadeNodeState) == 48);
	inline constexpr std::uint32_t kFadeFlagFadedIn = 1u << 14;    // the fade's target: faded in
	inline constexpr std::uint32_t kFadeFlagSettled = 1u << 15;    // OnVisible skips the update while fully faded in
	inline constexpr std::uint32_t kFadeFlagLodInUpdate = 1u << 27;  // FUN_14147a160 runs the LOD step first
	inline constexpr std::uint32_t kFadeFlagMask = kFadeFlagFadedIn | kFadeFlagSettled | kFadeFlagLodInUpdate;
	inline constexpr std::uint32_t kFadeVerdictInView = 1u << 0;     // the bound was in the frustum
	inline constexpr std::uint32_t kFadeVerdictAboveLimit = 1u << 1;  // a tree above the height limit: no update
	inline constexpr std::uint32_t kFadeVerdictServiced = 1u << 2;   // OnVisible ran
	inline constexpr std::uint32_t kFadeVerdictDrawn = 1u << 3;      // OnVisible went on into the children
	inline constexpr std::uint32_t kFadeVerdictAnimated = 1u << 4;   // the animation job's update ran first (FadeFrame anim*)

	struct FadeRootStatic
	{
		FadeNodeState initial;            // the node as listed
		float radius = 0.0f;              // +0xF0 at the seed, for lodScale. The root test reads FadeRootBound instead.
		float fadeAmount = 1.0f;          // NiAVObject +0x100
		float nearDistance = 0.0f;        // +0x128
		float farDistance = 0.0f;         // +0x12C
		std::uint32_t object = ~0u;       // one member's object slot. Marks the row listed; the census names it.
		std::uint32_t bits = 0;           // kFadeRoot*, and +0x109 in bits 8-15
		float lodScale = 1.0f;            // FUN_14147a430's distance scale: powf(e44, logf(+0xF0 / e40) * e54) with +0x109 bit 1
		std::uint32_t generation = 0;     // new for every listing (and every input refresh): the state row is seeded again
	};
	static_assert(sizeof(FadeRootStatic) == 80);
	/**
	 * @brief A root node's world bound, sampled every frame by the "fade-root-bounds" pool. The root test and the fade distance
	 * read it, as BSFadeNode::OnVisible reads the node's own bound. The row counts only when its generation is the root row's.
	 */
	struct FadeRootBound
	{
		float centre[3] = {};
		float radius = 0.0f;
		std::uint32_t generation = 0;
		std::uint32_t pad[3] = {};
	};
	static_assert(sizeof(FadeRootBound) == 32);
	inline constexpr std::uint32_t kFadeRootPlanMask = 0x3u;  // which OnVisible the node has
	inline constexpr std::uint32_t kFadeRootFade = 0;         // BSFadeNode's
	inline constexpr std::uint32_t kFadeRootLeaf = 1;         // BSLeafAnimNode's: the LOD step, then BSFadeNode's
	inline constexpr std::uint32_t kFadeRootTree = 2;         // BSTreeNode's: the height test, BSLeafAnimNode's, the LOD fix-up
	inline constexpr std::uint32_t kFadeRootOther = 3;        // another class's: not serviced
	inline constexpr std::uint32_t kFadeRootBitsShift = 8;    // +0x109
	inline constexpr std::uint32_t kFadeRootTreeLod = 1u << 16;     // a tree whose LOD switch selects past child 0 (+0x180's +0x12C)
	inline constexpr std::uint32_t kFadeRootTreeThresholds = 1u << 17;  // a BSTreeNode: a type-4 node takes the tree LOD thresholds
	// Members of a listed root follow FadeStateCS. Roots DCLF does not list keep the engine's update and the distance test.
	inline constexpr std::uint32_t kFadeRootOwned = 1u << 18;
	// Flags BSCullingProcess::Process1 (AE 0x140e28390) reads before OnVisible. kAlwaysDraw and kPreProcessedNode skip the bound tests.
	inline constexpr std::uint32_t kFadeRootAlwaysDraw = 1u << 20;
	inline constexpr std::uint32_t kFadeRootPreprocessed = 1u << 21;
	inline constexpr std::uint32_t kFadeRootPreprocessHidden = 1u << 22;
	// An owned root with no engine-drawn part. FadeStateCS alone owns its state, and the fade write-back gives the node the GPU's result.
	// An owned root with engine-drawn parts is culled by the engine instead.
	inline constexpr std::uint32_t kFadeRootStoodIn = 1u << 19;
	inline constexpr std::uint32_t kNoFadeRoot = ~0u;
	// Which of the slot's two seed rows holds the current generation. Toggling it keeps a new seed off the row the installed tables still read.
	inline constexpr std::uint32_t kFadeRootSeedOdd = 1u << 23;
	// The node is DCLF's alone to write. Neither OnVisible nor the animation gate steps it.
	inline constexpr std::uint32_t kFadeRootWritten = 1u << 24;
	// Bits from the node's seed, and bits from the coordinator's row.
	inline constexpr std::uint32_t kFadeRootNodeBits = kFadeRootPlanMask | (0xFFu << kFadeRootBitsShift) | kFadeRootTreeThresholds | kFadeRootAlwaysDraw |
	                                                   kFadeRootPreprocessed | kFadeRootPreprocessHidden;
	inline constexpr std::uint32_t kFadeRootCoordinatorBits = kFadeRootTreeLod | kFadeRootOwned | kFadeRootStoodIn | kFadeRootSeedOdd | kFadeRootWritten;
	inline constexpr std::size_t FadeSeedRow(std::uint32_t a_slot, std::uint32_t a_bits) { return 2ull * a_slot + ((a_bits & kFadeRootSeedOdd) ? 1u : 0u); }
	/** @brief False while no seed of the row's generation is there. Must match FadeStateCS.hlsl's MergeSeed. */
	inline bool MergeFadeSeed(const FadeRootStatic& a_root, const FadeRootStatic* a_seeds, std::size_t a_seedCount, std::uint32_t a_slot, FadeRootStatic& a_out)
	{
		const std::size_t at = FadeSeedRow(a_slot, a_root.bits);
		if (!a_seeds || at >= a_seedCount || a_seeds[at].generation != a_root.generation)
			return false;
		// a_out may be a_root.
		FadeRootStatic merged = a_seeds[at];
		merged.object = a_root.object;
		merged.generation = a_root.generation;
		merged.fadeAmount = a_root.fadeAmount;
		merged.bits = (a_seeds[at].bits & kFadeRootNodeBits) | (a_root.bits & kFadeRootCoordinatorBits);
		a_out = merged;
		return true;
	}

	/** @brief The main camera and the engine's fade globals. Addresses are AE. */
	struct FadeFrame
	{
		float eye[3]{};                   // the main camera's world position
		float lodAdjust = 0.0f;           // its +0x184; 0: no camera this frame, nothing is updated
		std::int32_t counter = 0;         // the fade frame counter (0x142032e50)
		float deltaTime = 0.0f;           // 0x142033084
		std::uint32_t fadesOn = 0;        // 0x142032dfd: OnVisible updates fades
		std::uint32_t lodUpdates = 0;     // 0x142032dfc
		float fadeInTime = 0.0f;          // 0x142032e2c
		float fadeOutTime = 0.0f;         // 0x142032e30
		float fadeInAbove = 0.0f;         // 0x142032e34
		float fadeOutBelow = 0.0f;        // 0x142032e38
		float blendTime = 0.0f;           // 0x142032e3c
		float distanceMult = 0.0f;        // 0x142032e48
		float stepMax = 0.0f;             // 0x142032e4c
		float amountTime = 0.0f;          // 0x14332a214
		float metricScale = 0.0f;         // 0x141aa6300
		float defaultScale = 0.0f;        // 0x141ad2840
		float metricOverride = 0.0f;      // 0x14332a254
		std::uint32_t overridden = 0;     // 0x14332a254 != 0x141769578 (also selects OnVisible's type-6 branch when clear)
		float lodFar = 0.0f;              // 0x14332a22c
		float lodNear = 0.0f;             // 0x14332a25c
		float treeLodFar = 0.0f;          // 0x14332a238
		float treeLodNear = 0.0f;         // 0x14332a268
		float lodMinimum = 0.0f;          // 0x1433dcfa8
		float one = 1.0f;                 // 0x141ad2870
		float amountSnapAbove = 0.0f;     // 0x141ad2874
		float amountSnapOffset = 0.0f;    // 0x141ad288c
		float snapRadiusLimit = 0.0f;     // 0x141ad29f4
		float treeHeightBase = 0.0f;      // BSTreeNode::OnVisible's height test: 0x14332a2f0
		float treeHeightLimit = 0.0f;     // 0x142032fa0, +infinity while the test is off
		std::uint32_t padding = 0;
		float divisors[16]{};             // 0x142032e00, per LOD type
		// Per-frame values, since the invocation is prepared ahead of the commit. outSlot is the published states this render writes
		// (FrameView::RenderSerial's parity). The reserved word keeps the shaders' layout.
		std::uint32_t rootCount = 0, sceneFrame = 0, outSlot = 0, reserved1 = 0;
		// Inputs for the animation job's fade update (FUN_1402cff60) on a root its last cull missed.
		float animEye[3]{};
		float animLodAdjust = 0.0f;
		std::int32_t animCounter = 0;
		float animDeltaTime = 0.0f;
		std::uint32_t animPadding[2]{};
	};
	// Per root, the scene frame of the batch that updated it and how many updates it made. The job can update a node more than once between culls.
	inline constexpr std::uint32_t kFadeAnimatedCountBits = 4;
	inline constexpr std::uint32_t kFadeAnimatedCountMask = (1u << kFadeAnimatedCountBits) - 1;
	inline constexpr std::uint32_t FadeAnimatedWord(std::uint32_t a_frame, std::uint32_t a_count)
	{
		return (a_frame << kFadeAnimatedCountBits) | (a_count < kFadeAnimatedCountMask ? a_count : kFadeAnimatedCountMask);
	}
	static_assert(sizeof(FadeFrame) == 240);

	/**
	 * @brief One indirect draw in the command signature's argument order. BasicRHI packs arguments like D3D12, 4-byte aligned.
	 *
	 * Rows and blocks are device addresses, while the vertex buffer views hold refs because only vertex stages read them.
	 * No DCLF pipeline has input assembly. The vertex stages pull attributes through the views, and no draw reads their sizes.
	 */
#pragma pack(push, 4)
	struct DrawSequence
	{
		std::uint32_t pipelineIndex;  // in its state class's execution sets (a device-generated draw's); 0 for a plain draw
		// The next 24 words are one Constant indirect argument, the per-draw push data. Keep them together and in order.
		// DCLFObjects.hlsli's DCLFPushData must match.
		std::uint64_t pipelineRowAddress;
		std::uint64_t materialRowAddress;
		std::uint32_t objectIndex;
		RenderGraph::BufferRef vertexBufferRef;  // GeometryBuffers::vertexRef (null without the render graph)
		std::uint32_t vertexBufferSize;  // written; no draw reads it
		std::uint32_t vertexStride;
		RenderGraph::BufferRef streamBufferRef;  // slot 1: a face shape's positions, else the slot 0 view again
		std::uint32_t streamBufferSize;  // written; no draw reads it
		std::uint32_t streamStride;
		std::uint32_t blockPad;  // 0: the block addresses after it start at an even push word (BasicRHI: pushed addresses 8-aligned)
		std::uint64_t psTechniqueAddress;  // PS b0: pipeline row + kPipelineRowTechniquePS
		std::uint64_t vsGeometryAddress;  // VS b2: pipeline row + kPipelineRowGeometryVS
		std::uint64_t psGeometryAddress;  // PS b2: pipeline row + kPipelineRowGeometryPS
		std::uint64_t permutationAddress;  // b4, both stages: pipeline row + kPipelineRowPermutation
		std::uint64_t psMaterialAddress;  // PS b1: material row + kMaterialRowPS
		std::uint64_t indexBufferAddress;  // GeometryBuffers::indexAddress
		std::uint32_t indexBufferSize;
		std::uint32_t indexFormat;  // DXGI_FORMAT_R16_UINT
		std::uint32_t indexCount;
		std::uint32_t instanceCount;
		std::uint32_t firstIndex;
		std::int32_t vertexOffset;
		std::uint32_t firstInstance;
	};
#pragma pack(pop)
	static_assert(sizeof(DrawSequence) == 136);
	static_assert(offsetof(DrawSequence, pipelineRowAddress) == 4);
	static_assert(offsetof(DrawSequence, materialRowAddress) == 12);
	static_assert(offsetof(DrawSequence, objectIndex) == 20);
	static_assert(offsetof(DrawSequence, vertexBufferRef) == 24);
	static_assert(offsetof(DrawSequence, streamBufferRef) == 40);
	static_assert(offsetof(DrawSequence, blockPad) == 56);
	static_assert(offsetof(DrawSequence, psTechniqueAddress) == 60);
	static_assert(offsetof(DrawSequence, vsGeometryAddress) == 68);
	static_assert(offsetof(DrawSequence, psGeometryAddress) == 76);
	static_assert(offsetof(DrawSequence, permutationAddress) == 84);
	static_assert(offsetof(DrawSequence, psMaterialAddress) == 92);
	static_assert(offsetof(DrawSequence, indexBufferAddress) == 100);
	static_assert(offsetof(DrawSequence, indexCount) == 116);
	static_assert(offsetof(DrawSequence, instanceCount) == 120 && offsetof(DrawSequence, vertexOffset) == 128 && offsetof(DrawSequence, firstInstance) == 132);
}
