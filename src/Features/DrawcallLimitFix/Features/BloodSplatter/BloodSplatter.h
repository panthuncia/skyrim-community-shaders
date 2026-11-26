#pragma once

#include "RenderGraph/BufferRef.h"

// Blood splatter: the engine's screen blood, drawn by the render graph in the engine's place, in both graph modes.
// The draws are made from ScreenSplatter's root and camera and a snapshot block; the engine's batch is not run.
// An ORG pass at the EngineDraws anchor draws them with BloodSplatter.hlsl.
// Programs are prebuilt and the client is made at setup, so the first hit never waits or rebuilds.
// See docs/development/engine/blood-splatter.md.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"
#include "Snapshot/FrameSnapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Features::BloodSplatter
{
	namespace Address
	{
		// Main::RenderWorld's light flare setup: LightLoc, the flare scale and the HDR target, from the world camera.
		inline constexpr Engine::Offset kFlareSetup{ { 106979 }, { 100269 } };
		// What it writes, and what ScreenSplatter::Update writes, for SetupGeometry and SetupTechnique.
		inline constexpr Engine::Offset kFlareScale{ { 414855 }, { 527908 } };
		inline constexpr Engine::Offset kCtrl{ { 414856 }, { 527909 } };
		inline constexpr Engine::Offset kHdrTarget{ { 414857 }, { 527910 } };
		inline constexpr Engine::Offset kLightLoc{ { 414858 }, { 527911 } };
		inline constexpr Engine::Offset kFlareAlphaScale{ { 391123 }, { 513344 } };
		inline constexpr Engine::Offset kAlphaScale{ { 391124 }, { 513345 } };
		// BSBloodSplatterShader's singleton, the shader of its passes (GetRenderPasses).
		inline constexpr Engine::Offset kShader{ { 414854 }, { 527907 } };
	}

	/** @brief The most draws a run holds. */
	inline constexpr std::uint32_t kMaxDraws = 32;

	/** @brief Must match BloodSplatterOrgDraw in BloodSplatter.hlsl. */
	struct DrawConstants
	{
		float worldViewProj[4][4];  // VS b2 c0-c3 (rows)
		float lightLoc[4];          // VS b2 c4
		float ctrl[4];              // VS b2 c5
		float alpha[4];             // PS b2 c0
		std::uint32_t textures[4];  // t0-t3
		std::uint32_t samplers[4];  // s0-s3
		RenderGraph::BufferRef vertexRef;  // the vertex stage pulls vertices through this
		RenderGraph::BufferRef indexRef;
		std::uint32_t vertexLayout[2];
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
	};
	static_assert(sizeof(DrawConstants) == 176, "BloodSplatterOrgDraw's layout");

	/** @brief One draw. */
	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // a RasterProgram
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	struct Run
	{
		std::vector<Draw> draws;
		std::uint64_t revision = 0;
		Family::Owners owners;

		/** @brief What the recording depends on: the draws' programs and viewports (BloodSplatterDraws.cpp). */
		std::uint64_t Hash() const;
	};

	/**
	 * @brief The screen blood's game inputs, captured on the game thread (Snapshot::BlockId::ScreenBlood). Main::RenderWorld's
	 * light flare setup (SE 100269, AE 106979) computes the first three from the world camera and the sun; ScreenSplatter::Update writes
	 * the others. SetupGeometry and SetupTechnique read them.
	 */
	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::ScreenBlood;
		std::array<float, 4> lightLoc{};  // VS LightLoc: the light's screen position, 0.1 in w
		float flareScale = 0.0f;          // how much the camera faces the light: the flare's alpha factor
		std::int32_t hdrTarget = 0;       // the render target the flare samples (t3)
		float ctrl = 0.0f;                // VS Ctrl: fBloodSplatterFlareOffsetScale
		float alphaScale = 0.0f;          // every draw's alpha factor
		float flareAlphaScale = 0.0f;     // the flare's further alpha factor
		bool operator==(const Block&) const = default;
	};

	/** @brief Game thread, world snapshots. Runs the light flare setup and copies the block. */
	void CaptureBlock(Snapshot::Builder& a_builder);

	/** @brief One draw as the engine would make it: its pass, geometry, constants, textures and samplers. */
	struct DrawInputs
	{
		bool flare = false;
		const Family::TriShape* shape = nullptr;
		std::uint32_t triangles = 0;
		float worldViewProj[4][4]{};
		float lightLoc[4]{};
		float ctrl = 0.0f;
		float alpha = 0.0f;
		std::array<ID3D11ShaderResourceView*, 4> views{};  // unwrapped; null where the technique samples nothing
		std::int32_t hdrTarget = 0;                        // the flare's t3 target
	};

	/**
	 * @brief Render thread. The draws Main::RenderScreenSplatter would make this frame, from ScreenSplatter's root and camera and
	 * the adopted snapshot's block, in RenderBatches' order: the flares, then the splatters. False, with a_why, when they cannot be read.
	 */
	bool CollectDraws(std::vector<DrawInputs>& a_out, std::string& a_why);

	Api::Feature Feature();

	/** @brief Read at install. */
	bool Enabled();

	/** @brief The game-side capture and the screen blood at Main::Draw's call of RenderScreenSplatter. */
	void InstallHooks();
	void PreflightHooks();

	// Render thread, except Setup.
	namespace BloodSplatterDraws
	{
		/** @brief False for good once the client failed. */
		bool Available();
		/**
		 * @brief Main thread. Starts the stage precompile, queues the programs and makes the client.
		 * True once done or refused for good. False while the render graph has no host yet.
		 */
		bool Setup();
		/** @brief False means the engine draws the batch. Logged once per reason. */
		bool CanTake();
		/** @brief Null when the program cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases alive as long as a run would, when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief False means the draws were not drawn. Logged. */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief How the client reads a published engine target (the flare's adapted luminance). */
		Family::InputResult ReadPublished(std::string_view a_id, std::uint32_t& a_index);
		std::string Status();
	}
}
