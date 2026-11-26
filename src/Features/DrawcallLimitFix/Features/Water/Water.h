#pragma once

#include <atomic>

#include "RenderGraph/BufferRef.h"

// Water, in graph mode: the render graph draws the main camera's BSWaterShader passes in the engine's place.
// That covers the water mask (Stencil technique) and the water itself. Details are in docs/development/engine/water.md.
// DCLF's scene path draws the water it claims in its water epochs. The rest (the wading displacement mesh, and water the scene path
// refuses) is computed at each water batch from the passes the engine registered and the water port's constants (WaterNative.cpp),
// and an ORG pass at the EngineDraws anchor draws it with CS's Water.hlsl (WaterDraws.cpp). Its passes are taken out of the batch,
// so the engine draws none of them. The water displacement simulation stays the engine's.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"
#include "Features/DrawcallLimitFix/Integrations/PhysicalSky/PhysicalSky.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Features::Water
{
	namespace Address
	{
		// FinishAccumulating's RenderBatches for the water mask (SE 99938, AE 106583).
		inline constexpr Engine::Offset kMaskBatch{ { 106583, 0x492 }, { 99938, 0x4A6 } };
		// FinishAccumulating's check that the batch renderer holds mask passes (0x5C00006D..70), which guards the whole mask block.
		inline constexpr Engine::Offset kMaskPassesCheck{ { 106583, 0x3B4 }, { 99938, 0x3C5 } };
		// FinishAccumulatingPostResolveDepth's RenderBatches for the water (SE 99939, AE 106584).
		inline constexpr Engine::Offset kSurfaceBatch{ { 106584, 0x161 }, { 99939, 0x164 } };
		// Geometry group 10 just before it (fades and LOD cross-fades). The surface view takes its water passes. The rest are Lighting
		// cross-fade copies, which DCLF draws or vouches for.
		inline constexpr Engine::Offset kGroup10Batch{ { 106584, 0x143 }, { 99939, 0x146 } };
	}

	enum class View : std::uint8_t
	{
		Mask,     // the Stencil technique: the water's normal mask (kNORMAL_TAAMASK_SSRMASK or its swap) and motion vectors
		Surface,  // the water techniques: kMAIN, or kRAW_WATER when ISWaterBlend composites the water after
		Count
	};

	/** @brief The engine targets a view's draws may name. A draw naming any other stays the engine's. */
	inline constexpr std::array<std::int32_t, 3> kMaskTargets{ 4, 5, 7 };       // kNORMAL_TAAMASK_SSRMASK, its swap, kMOTION_VECTOR
	inline constexpr std::array<std::int32_t, 3> kSurfaceTargets{ 1, 88, -1 };  // kMAIN, kRAW_WATER

	/** @brief How many colour targets a view's pixel stage writes. */
	inline constexpr std::uint32_t TargetsOf(View a_view) { return a_view == View::Mask ? 2u : 1u; }

	/**
	 * @brief The header registers Water.hlsl's pixel stage reads by handle, in its EXTERN order (WATER_ORG_RESOURCES).
	 * The directional shadow record (t98) is a latch region instead, between 77 and 102 in the push block.
	 */
	inline constexpr std::array<std::uint32_t, 18> kHeaderRegisters{ 17, 18, 19, 22, 25, 26, 30, 31, 35, 36, 37, 50, 60, 70, 76, 77, 102, 103 };

	using Kind = GpuTextures::ViewKind;

	/** @brief Each header register's declared dimension. A buffer is read only when published. */
	inline constexpr std::array<Kind, 18> kHeaderKinds{
		Kind::Texture2D,       // t17 DepthTexture
		Kind::Texture2D,       // t18 SharedShadowMap
		Kind::Texture3D,       // t19 the fog's near volume
		Kind::Texture3D,       // t22 the fog's far volume
		Kind::Cube,            // t25 CloudShadowsTexture
		Kind::Cube,            // t26 CloudSelfShadowTexture
		Kind::Cube,            // t30 EnvReflectionsTexture
		Kind::Cube,            // t31 EnvTexture
		Kind::Buffer,          // t35 lights
		Kind::Buffer,          // t36 lightList
		Kind::Buffer,          // t37 lightGrid
		Kind::Texture3D,       // t50 SkylightingProbeArray
		Kind::Texture2D,       // t60 ShadowHeightTexture
		Kind::Texture2D,       // t70 TexPrecipOcclusion
		Kind::Texture2D,       // t76 EnvIBLTexture
		Kind::Texture2D,       // t77 SkyIBLTexture
		Kind::Buffer,          // t102 LocalShadows
		Kind::Texture2DArray,  // t103 LocalShadowMaps
	};

	inline constexpr std::uint32_t kMaterialSlots = 12;

	/** @brief The material slots' declared dimension: the cubemap at t3, 2D textures elsewhere. */
	constexpr Kind MaterialKind(std::uint32_t a_slot) { return a_slot == 3 ? Kind::Cube : Kind::Texture2D; }

	/**
	 * @brief Whether a_view's pixel stage samples material slot a_slot. The mask samples none and the surface never samples t0.
	 * Slots not sampled are latched as null views, whatever the engine left bound.
	 */
	constexpr bool SlotRead(View a_view, std::uint32_t a_slot) { return a_view == View::Surface && a_slot != 0; }

	/** @brief Whether a_view's pixel stage reads the headers. The mask reads none. */
	constexpr bool HeadersRead(View a_view) { return a_view == View::Surface; }

	/** @brief The headers before the directional shadow record in the push block. */
	inline constexpr std::size_t kHeadersBeforeShadow = 16;
	/** @brief Deferred's directional shadow record (ShadowSampling.hlsli), written by the CPU and copied into each draw's latch. */
	inline constexpr std::uint32_t kDirectionalShadowRegister = 98;

	/** @brief The rows of each engine constant group the latch takes (Water.hlsl's packoffsets). */
	inline constexpr std::array<std::uint32_t, 6> kGroupRows{ 1, 6, 14, 5, 14, 22 };  // VS b0, b1, b2, PS b0, b1, b2

	/** @brief WaterOrgDraw in Water.hlsl, 1152 bytes. */
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
		std::uint32_t textures[kMaterialSlots];
		std::uint32_t samplers[kMaterialSlots];
		RenderGraph::BufferRef vertexRef;  // the vertex buffer view ref the vertex stage pulls through
		RenderGraph::BufferRef indexRef;   // the index buffer's
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
		Integrations::PhysicalSky::OrgHandles physSky;  // the run's, the same for every draw
	};
	static_assert(sizeof(DrawConstants) == 1248, "WaterOrgDraw's layout");

	/** @brief Light Limit Fix's PS b3 (StrictLightDataValues), 1232 bytes. */
	inline constexpr std::size_t kStrictLightBytes = 1232;

	/** @brief DirectionalShadowLightData (t98), 272 bytes. */
	inline constexpr std::size_t kDirectionalShadowBytes = 272;

	/** @brief One draw's latch region: its values, its strict lights, then the directional shadow record. */
	struct DrawLatch
	{
		DrawConstants constants;
		std::byte strictLights[kStrictLightBytes];
		std::byte directionalShadow[kDirectionalShadowBytes];
	};
	static_assert(sizeof(DrawLatch) == 2752 && sizeof(DrawLatch) % 16 == 0, "a draw's latch region keeps 16-byte rows");

	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;    // the RasterProgram (WaterDraws.cpp)
		std::array<std::uint8_t, 2> colours{};  // indices into the view's targets (kMaskTargets, kSurfaceTargets)
		std::uint32_t stencilRef = 0;
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawLatch latch{};
	};

	/**
	 * @brief What a view's client is made for. Targets and published inputs are fixed per view, so neither is part of it.
	 * Headers and slots are read by heap index, resolved per batch.
	 */
	struct Shape
	{
		View view = View::Surface;
		bool operator==(const Shape&) const = default;
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		View view = View::Surface;
		std::vector<Draw> draws;
		std::array<std::uint32_t, kHeaderRegisters.size()> headerIndices{};  // every header's heap index (an import, a published id's, a null view)
		Integrations::PhysicalSky::OrgHandles physSky{};                     // Physical Sky's resources and samplers, latched with every draw
		std::uint64_t revision = 0;                                          // what the recording depends on (WaterDraws.cpp, Run::Hash)
		Family::Owners owners;                                               // the textures' imports and the buffers' leases
		// A CPU-driven frame's DCLF water draws of the batch (Draws::TakeCpuRange), drawn before the latched ones into target set
		// cpuVariant. Null for none. Only the first run of a batch carries them.
		void* cpuWater = nullptr;
		std::uint32_t cpuVariant = 0;
		// Set by the first commit that hands cpuWater on. The site run belongs to that frame, so a later commit of the same run
		// (no new batch) must not hand it on again.
		mutable std::atomic<bool> cpuHanded{ false };

		/** @brief What the recording depends on: everything but the latched values and counts (WaterDraws.cpp). */
		std::uint64_t Hash() const;
	};

	Api::Feature Feature();

	/** @brief Whether the family is on, which is graph mode (read at install). */
	bool Enabled();

	/** @brief Installs the batch hooks. False when this runtime has no address for them, so nothing triggers DCLF's water epochs. */
	bool InstallHooks();
	void PreflightHooks();

	namespace WaterDraws
	{
		/** @brief Whether the anchor and runtime are up. False for good once a client failed. */
		bool Available();
		/** @brief Makes both views' clients and queues their programs. Main thread. False while the graph has no host. */
		bool Setup();
		/** @brief Whether a_view's batch is taken now. False is logged once, and the engine draws the batch. */
		bool CanTake(View a_view);
		/** @brief How a_view's client reads the published id a bound header or slot exports. */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key, or null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases alive when it ran nothing, so the next batches find them. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor. False means its draws were not drawn, and that is logged. */
		bool Submit(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
