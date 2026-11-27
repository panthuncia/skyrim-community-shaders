#pragma once

#include "Snapshot/FrameSnapshot.h"
#include "Utils/ColorSpace.h"

#include <array>
#include <cstdint>

namespace Snapshot
{
	/**
	* @brief The core block: the game data the shared frame state reads (State::UpdateSharedData and the Util helpers).
	* Captured once per game update, before the feature captures, so they can read it through Builder::Built().
	*
	* Display size, dynamic resolution and the vanilla TAA flag are not here: they are render state CS owns
	* (the frame variables and Util's mirrors), not game data.
	*/
	struct CoreFrame
	{
		static constexpr BlockId kId = BlockId::CoreFrame;

		/**
		* @brief The main camera's near and far planes. Native: the world camera's frustum. Guest: the high actor culler's copy of it,
		* which the render half makes (one update late).
		*/
		float cameraNear = 0.0f;
		float cameraFar = 0.0f;
		/** @brief fDRClampOffset:Display and the inverse of fGamma:Display, for the frame camera's per-frame block. */
		float drClampOffset = 0.0f;
		float inverseGamma = 1.0f;
		/** @brief The world FOV in degrees (horizontal). */
		float fovDegrees = 0.0f;
		/** @brief Util::IsInterior: an interior cell, or a worldspace without sky or with fixed dimensions. */
		bool noSky = true;

		/** @brief The shadow scene's directional (sun) light. */
		RE::NiColor dirLightColor{};
		float dirLightFade = 0.0f;
		RE::NiPoint3 dirLightDirection{};
		/** @brief The colour is Sky Sync's working-space override (Linear Lighting does not convert it). */
		bool dirLightOverridden = false;
		/** @brief Linear Lighting's sunlight scale for the light (1 when overridden). */
		float sunlightScale = 1.0f;
		/** @brief The directional ambient (DALC) transform: rotate rows, then translate. */
		std::array<float, 12> dalc{};

		/** @brief Sky state. Directions are zero for a body the sky does not have. */
		bool hasSky = false;
		bool hideSky = false;
		float4 sunDirection{};
		float4 masserDirection{};
		float4 secundaDirection{};
		Util::ColorSpace::LightColor sun{};  // the disc's blend colour, or the default
		Util::ColorSpace::LightColor masser{};
		Util::ColorSpace::LightColor secunda{};

		/**
		* @brief Geometry the sky permutation compares draws with: cloud layer 28, Secunda's meshes. Held (EngineRef), so an address
		* stays the same object while a snapshot names it, and never dereferenced on the render side.
		*/
		EngineRef noScatteringCloud;
		EngineRef secundaMoonMesh;
		EngineRef secundaShadowMesh;

		/** @brief Water colour and relative height in the 5x5 cells around the main camera (State's WaterData). */
		std::array<float4, 25> water{};

		bool operator==(const CoreFrame&) const = default;
	};

	/** @brief Fills the core block. The snapshot builder calls it before the feature captures. Game thread. */
	void CaptureCore(Builder& a_builder);

	/** @brief The adopted snapshot's core block, or a default block before the first snapshot. Render owner. */
	const CoreFrame& Core();
}
