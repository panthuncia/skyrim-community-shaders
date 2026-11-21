#pragma once

#include "RenderGraph/BufferRef.h"

// Precipitation, in graph mode: the render graph draws the engine's rain and snow (BSParticleShader ENVCUBE techniques)
// in the engine's place.
// They draw through the grouped alphas (AE 107673 +0x1F3), mixed with every other blended draw, so there is no batch to hook.
// Each draw is latched at DrawTriShape and drawn at once as its own run at the EngineDraws anchor, with CS's Particle.hlsl.
// A draw the family cannot take is drawn by the engine, logged once per reason, with nobody vouching for it.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Features::Precipitation
{
	/** @brief Whether a particle technique is EnvCubeSnow or EnvCubeRain. */
	inline constexpr bool PrecipitationTechnique(std::uint32_t a_descriptor) { return a_descriptor == 4 || a_descriptor == 5; }
	/** @brief EnvCubeRain: the technique Rain Refraction draws its refractive rain with. */
	inline constexpr std::uint32_t kRainTechnique = 5;

	/**
	 * @brief The engine targets a draw may name. The water drawn earlier leaves its temporal mask bound, and the rain writes its
	 * normal there for the water's temporal blend. A draw naming another second target draws into kMAIN alone.
	 */
	inline constexpr std::array<std::int32_t, 6> kTargets{ 1, 4, 5, 7, 85, 86 };
	inline constexpr std::uint32_t kMaxColours = 2;

	/** @brief The material slots a draw samples: source, grayscale, occlusion map, underwater mask. */
	inline constexpr std::uint32_t kMaterialSlots = 4;
	/** @brief Whether an ENVCUBE draw samples slot a_slot. Those techniques do not read the grayscale (t1). */
	constexpr bool SlotRead(std::uint32_t a_slot) { return a_slot != 1; }

	/** @brief Rain Refraction's raindrop normals (Particle.hlsl's t80), which CS binds for refractive rain. */
	inline constexpr std::uint32_t kRaindropRegister = 80;

	/** @brief The rows of each engine constant group the latch takes: VS b0 (ScaleAdjust), VS b2 (ENVCUBE's 23), PS b2 (2). */
	inline constexpr std::uint32_t kVSTechniqueRows = 1, kVSGeometryRows = 23, kPSGeometryRows = 2;

	/** @brief ParticleOrgDraw in Particle.hlsl, 592 bytes. */
	struct DrawConstants
	{
		float vsTechnique[kVSTechniqueRows][4];
		float vsGeometry[kVSGeometryRows][4];
		float psGeometry[kPSGeometryRows][4];
		float projection[4];         // FrameBuffer: CameraProj[0][0], CameraProj[1][1], DynamicResolutionParams1.xy
		float bufferDim[4];          // SharedData::BufferDim
		float rainRefraction[3][4];  // FeatureData: RainRefractionSettings
		std::uint32_t descriptors[4];  // CS's permutation block: vertex, pixel, extra, extra feature
		float effectRadius;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t raindropNormals;  // t80's heap index
		std::uint32_t textures[kMaterialSlots];
		std::uint32_t samplers[kMaterialSlots];
		RenderGraph::BufferRef vertexRef;  // the vertex buffer view ref the vertex stage pulls through
		RenderGraph::BufferRef indexRef;   // the index buffer's
		std::uint32_t vertexLayout[2];
		std::uint32_t padding[2];
	};
	static_assert(sizeof(DrawConstants) == 592, "ParticleOrgDraw's layout");

	/** @brief One draw's latch region: values, strict lights, then the directional shadow record, as the water's. */
	struct DrawLatch
	{
		DrawConstants constants;
		std::byte strictLights[Water::kStrictLightBytes];
		std::byte directionalShadow[Water::kDirectionalShadowBytes];
	};
	static_assert(sizeof(DrawLatch) % 16 == 0, "a draw's latch region keeps 16-byte rows");

	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // the RasterProgram
		std::array<std::uint8_t, kMaxColours> colours{};  // indices into kTargets
		std::uint32_t colourCount = 0;
		std::uint32_t stencilRef = 0;
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawLatch latch{};
	};

	/** @brief A draw's run. Particle.hlsl's push block uses the water's header layout. */
	struct Run
	{
		std::vector<Draw> draws;
		std::array<std::uint32_t, Water::kHeaderRegisters.size()> headerIndices{};
		std::uint64_t revision = 0;
		Family::Owners owners;

		/** @brief What the recording depends on: everything but the latched values and counts (PrecipitationDraws.cpp). */
		std::uint64_t Hash() const;
	};

	Api::Feature Feature();

	/** @brief Whether the family is on, which is graph mode (read at install). */
	bool Enabled();

	void InstallHooks();
	void PreflightHooks();

	namespace PrecipitationDraws
	{
		/** @brief Whether the anchor and runtime are up. False for good once the client failed. */
		bool Available();
		/** @brief Makes the client and queues its programs. Main thread. False while the render graph has no host yet. */
		bool Setup();
		/** @brief Whether a draw is taken now. False is logged once. */
		bool CanTake();
		/** @brief How the client reads the published id a bound header or slot exports. */
		Family::InputResult ReadPublished(std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key, or null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a refused draw's imports and leases as long as a run's. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs the anchor with a_run's draws. False means they were not drawn, and that is logged. */
		bool Submit(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
