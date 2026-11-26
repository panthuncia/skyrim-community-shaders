#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <array>
#include <cstdint>
#include <string>

namespace RenderGraph::Engine::ImageSpace
{
	/**
	* @brief The game data the native post chain computes its ImageSpace effects from: the blended ImageSpace data, the effect
	* selection the game decides (the end-of-frame slots), and the settings the formulas read. One function fills it
	* (ReadGameInputs in NativePost.inl): native mode calls it in the snapshot capture, guest mode at today's read points.
	*/
	struct GameInputs
	{
		bool read = false;  // the ImageSpace manager exists
		// ImageSpaceManager +0xC8 to +0x15C: the blended ImageSpace data (HDR, cinematic, tint, fade, blurs, depth of field).
		std::array<float, 37> data{};
		// HDR and adaptation settings (:Display), with the world map forms already picked.
		bool filmic = false, worldMap = false, adaptOff = false, stopAt2 = false, menu = false;
		float reinhardWhite = 0, filmicWhite = 0, saturationBoost = 0, contrastBoost = 0, brightnessBoost = 0, adaptSpeed = 0, adaptStrength = 0, bloomBoost = 0;
		float frameTime = 0, menuTimer = 0, range = 1;
		// The bloom's blur effects (0x17 + r): their radius (+0x90) and bright pass flag (+0x94).
		std::array<std::int32_t, 7> blurRadius{};
		std::array<bool, 7> blurBright{};
		bool hdr = false, eof = false, usesLdr = false, sil = false, iblf = false;
		float taaK = 1, taaThreshold = 0;
		std::array<float, 4> taaBlend{};
		// The end-of-frame slots the game turns on (bit per slot). Refraction (bit 1) is not here: its geometry turns it on.
		std::uint8_t gameSlots = 0;
		bool screenshot = false;  // a save asks for a frame (manager +0x211)
		std::uint32_t frameCounter = 0;
		// The slot effects' inputs.
		std::array<float, 4> refractionTint{};  // manager +0x98
		bool dofCompute = false;
		std::int32_t underwater = 0;
		bool underwaterData = false;  // manager +0xB8, the underwater depth of field
		std::array<float, 4> underwaterDof{};
		std::array<float, 7> dofIni{}, dofCustom{};
		std::int32_t radialEffect = -1;  // the radial blur's shader (by quality), a kEffects index
		bool dofDynamic = false, radialDynamic = false;
		std::array<std::int32_t, 7> slotBlurRadius{};
		std::array<bool, 7> slotBlurDynamic{};
		bool fullBlurDynamic = false, fullBlurLegacy = false, getHitDynamic = false;
		float getHitScale = 0, getHitAmount = 0, timer = 0;

		/** @brief The ImageSpace data at the manager offset a_offset (+0xC8 to +0x158). */
		float At(std::uintptr_t a_offset) const { return data[(a_offset - 0xC8) / 4]; }

		bool operator==(const GameInputs&) const = default;
	};

	/** @brief The reflections pass's and RenderSSAO's settings (INI values the engine keeps in globals). */
	struct WorldSettings
	{
		bool ssrBlur = false, ssrDownsample = false;
		std::int32_t ssrDivider = 0;
		std::array<float, 3> ssrRay{};
		float saoPower = 0, saoBias = 0, saoRadius = 0, saoRow3x = 0, saoRow3z = 0, saoContrast = 0, saoCameraZw = 0;
		bool saoHalf = false, saoContrastOn = false, saoFog = false, saoTemporal = false, improvedSnow = false;
		bool blurCameraGate = false;  // the full-screen blur follows the world camera's movement (414598)
		std::uint32_t saoLevels = 0;
		bool sparkleToggle = false, sparkleOnly = false, sparkleNoAo = false, sparkleNoise = false;
		std::array<float, 6> sparkle{};
		float sparkleMaxDistance = 0, sparkleShadow = 0;
		std::int32_t sparkleColourMode = 0;
		std::array<float, 3> sparkleColour{};
		float ssrCompositeA = 0, ssrCompositeB = 0;
		// The SAO composite's sun: the sun shadow light's direction (zero without one) and the sun light's diffuse.
		std::array<float, 3> sunDirection{}, sunColour{};

		bool operator==(const WorldSettings&) const = default;
	};

	/** @brief The ImageSpace block of a snapshot. Native mode's post chain reads it; guest reads the same values at its hooks. */
	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::ImageSpace;

		GameInputs inputs{};
		WorldSettings world{};
		// Adaptation resets the game asked for so far (Sky::Update sets the engine's flag; the capture takes it). The render side
		// resets once when the count moves, so a repeated snapshot does not reset again and a dropped one is not lost.
		std::uint32_t adaptResets = 0;
		// TAA history resets the game asked for so far (the loading menu sets the engine's flag as it opens and closes; the capture
		// takes it). The first native world render after the count moves resets the histories (State::ShouldResetHistory).
		std::uint32_t taaResets = 0;
		// Save screenshots asked for so far: the capture takes the manager's request (+0x211) as a count. The render side copies the
		// frame into kSAVE_GAME_SCREENSHOT once the count moves and reports the count back; the next capture completes the request.
		std::uint32_t saveShots = 0;
		// Vanilla screenshots asked for so far (PrintScreen, the capture mode, Papyrus's TakeScreenshot), and the newest files, request
		// n at n % kScreenshotRing. The render side hands each new one to the Screenshot feature.
		static constexpr std::uint32_t kScreenshotRing = 4;
		std::uint32_t screenshots = 0;
		std::array<std::string, kScreenshotRing> screenshotPaths{};

		bool operator==(const Block&) const = default;
	};
}
