#pragma once

// Precipitation (CS_ORG_PRECIPITATION): the engine's rain and snow (BSParticleShader's ENVCUBE techniques, the sky's precipitation
// geometry, Blackreach's spores among them) drawn by the render graph in the engine's place.
//
// The engine draws them through the grouped alphas (AE 107673 +0x1F3, the geometry-type dispatcher, DrawTriShape). The engine's CPU code
// runs unchanged: its BeginPass, SetupTechnique and SetupGeometry (CS's and Rain Refraction's hooks with them) set the draw up, and the
// engine draw families' DrawTriShape detour (Core/Engine/Family) latches the draw instead of drawing it: the shadow state, the constants
// the Setup functions wrote (the engine constants capture), the four material slots and their samplers, the headers CS binds, the
// geometry. Each draw is then drawn at once by an ORG pass at the EngineDraws anchor, at its place in the grouped alphas, with CS's
// Particle.hlsl built for ORG (PrecipitationDraws.cpp). There is no batch to hook: the grouped alphas mix the precipitation with
// every other blended draw, so a draw is a run of its own.
//
// The family is the primary path for the precipitation (docs/development/p5/primary-path.md): its stages compile when CS loads the
// ENVCUBE descriptors, its programs are prebuilt from the manifest, and its client is made at setup with every published input its
// draws can read. A draw it cannot take is drawn by the engine, logged once per reason, and nobody vouches for it under the flush latch.

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
	/** @brief BSParticleShader's techniques the family draws (ShaderCache::ParticleShaderTechniques): EnvCubeSnow and EnvCubeRain. */
	inline constexpr bool PrecipitationTechnique(std::uint32_t a_descriptor) { return a_descriptor == 4 || a_descriptor == 5; }
	/** @brief EnvCubeRain: the technique Rain Refraction draws its refractive rain with. */
	inline constexpr std::uint32_t kRainTechnique = 5;

	/**
	 * @brief The engine targets a draw may name, imported once by the client: kMAIN, and the second targets the grouped alphas may leave
	 * bound (kNORMAL_TAAMASK_SSRMASK, its swap, kMOTION_VECTOR, and kTEMPORAL_AA_WATER_1 and _2: the water drawn before the rain in the
	 * same grouped alphas leaves its temporal mask bound, and the engine's rain then writes its normal output there, which the water's
	 * temporal blend reads). A draw naming another second target draws into kMAIN alone (PrecipitationCapture.cpp).
	 */
	inline constexpr std::array<std::int32_t, 6> kTargets{ 1, 4, 5, 7, 85, 86 };
	/** @brief The colour targets Particle.hlsl's pixel stage writes (PS_OUTPUT: Color, Normal). */
	inline constexpr std::uint32_t kMaxColours = 2;

	/** @brief The material slots a draw samples (Particle.hlsl's t0-t3): the source, the grayscale, the occlusion map, the underwater mask. */
	inline constexpr std::uint32_t kMaterialSlots = 4;
	/** @brief Whether an ENVCUBE draw samples slot a_slot: the grayscale (t1) is not read by those techniques. */
	constexpr bool SlotRead(std::uint32_t a_slot) { return a_slot != 1; }

	/** @brief Rain Refraction's raindrop normals (Particle.hlsl's t80), which CS binds for refractive rain. */
	inline constexpr std::uint32_t kRaindropRegister = 80;

	/** @brief The rows of each engine constant group the latch takes: VS b0 (ScaleAdjust), VS b2 (ENVCUBE's 23), PS b2 (2). */
	inline constexpr std::uint32_t kVSTechniqueRows = 1, kVSGeometryRows = 23, kPSGeometryRows = 2;

	/** @brief ParticleOrgDraw in Particle.hlsl: a draw's latched values, 592 bytes. */
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
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t padding[2];
	};
	static_assert(sizeof(DrawConstants) == 592, "ParticleOrgDraw's layout");

	/** @brief One draw's latch region: its values, Light Limit Fix's strict lights, then the directional shadow record (as the water's). */
	struct DrawLatch
	{
		DrawConstants constants;
		std::byte strictLights[Water::kStrictLightBytes];
		std::byte directionalShadow[Water::kDirectionalShadowBytes];
	};
	static_assert(sizeof(DrawLatch) % 16 == 0, "a draw's latch region keeps 16-byte rows");

	/** @brief One latched draw. */
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

	/**
	 * @brief A draw's run, as the anchor's pass records it. The headers are the water's (Particle.hlsl's push block is laid out as
	 * Water.hlsl's: Water::kHeaderRegisters), each a heap index resolved for the draw.
	 */
	struct Run
	{
		std::vector<Draw> draws;
		std::array<std::uint32_t, Water::kHeaderRegisters.size()> headerIndices{};
		std::uint64_t revision = 0;
		Family::Owners owners;
	};

	// PrecipitationFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief Whether CS_ORG_PRECIPITATION=1 turned the family on (read at install). */
	bool Enabled();

	// PrecipitationCapture.cpp: the engine hooks, from the feature's install.
	void InstallHooks();
	void PreflightHooks();

	// PrecipitationDraws.cpp: the render graph's side (render thread).
	namespace PrecipitationDraws
	{
		/** @brief Whether the anchor exists and the runtime is up; false for good once the client failed. */
		bool Available();
		/** @brief Reserves the EngineDraws anchor (the feature's install). */
		void Reserve();
		/**
		 * @brief The family's setup (Family::AddSetup, main thread): the stage precompile started, the manifest's programs queued, the client
		 * made and its build asked for. True once done (or refused for good, logged); false while the render graph has no host yet.
		 */
		bool Setup();
		/** @brief Whether a draw is taken: the client exists and a run of the anchor now would draw. False is logged once. */
		bool CanTake();
		/** @brief How the client reads a_id, the published id a bound header or slot exports (Family::PublishedInputs::Resolve). */
		Family::InputResult ReadPublished(std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key (Family::EngineDrawPrograms::Get): null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a refused draw's imports and leases as long as a run's. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs the anchor with a_run's draws. True when the run was submitted; otherwise its draws were not drawn (logged). */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
