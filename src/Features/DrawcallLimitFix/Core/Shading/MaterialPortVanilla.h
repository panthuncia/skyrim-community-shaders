#pragma once

#include <array>
#include <cstdint>

struct ID3D11ShaderResourceView;

namespace DCLF::MaterialPort
{
	/**
	 * @brief Render target slots SetupMaterial can index, SRVs at 0x143289230 + 0x30 * index: 116 in AE, 114 in SE.
	 * kVanillaRenderTargetCapacity sizes the arrays. VanillaRenderTargets() is the running runtime's count.
	 */
	inline constexpr std::uint32_t kVanillaRenderTargetCapacity = 116;
	inline std::uint32_t VanillaRenderTargets() { return REL::Module::IsSE() ? 114u : 116u; }

	/**
	 * @brief The per-frame sources vanilla SetupMaterial reads, sampled on the render thread at the frame's start.
	 * Floats are the engine's bits, copied and never computed.
	 */
	struct VanillaFrame
	{
		bool sampled = false;

		// SetupTechnique's remap, so EvaluateVanilla sees the +0x94 the draw has: 0x12 -> 9 unless 0x142032fdb is set, 7 -> 0 unless 0x142035500 is set.
		std::uint8_t techniqueByte12 = 0;
		std::uint8_t techniqueByte7 = 0;

		// SparkleParams for a plain material at MultiIndexSparkle: the Snow class's default, or zeros.
		bool plainSnowSparkle = true;

		// The texture-transform buffer the frame reads, 0x142033180, which Main::Update flips.
		std::uint32_t textureTransformBuffer = 0;

		// LODTexParams.z: 1 when the byte at 0x142032fda is set, else 0.
		std::uint8_t lodTexParamsZ = 0;
		// LandscapeTexture5to6IsSnow: .z is 1 when 0x142035548 is set, .w is 1 / (float)(int at 0x1420355f0).
		std::uint8_t landSnowZ = 0;
		std::int32_t landSnowDivisor = 0;

		// PS 6 AmbientSpecular: 16 bytes at 0x14203315c. Not written when SSE Engine Fixes NOPs out this 0x20-byte write
		// at SetupMaterial +0x8CF in AE or +0x713 in SE, because the fix writes PS 6 in SetupGeometry instead.
		std::array<float, 4> ambientSpecular{};
		bool ambientSpecularWritten = true;
		// SnowRimLightParameters (PS 34): 0x142035590, 0x1420355a8, 0x1420355c0, with .w 1 when 0x1420355d8 is set.
		std::array<float, 3> snowRim{};
		std::uint8_t snowRimW = 0;
		// CharacterLightParams (PS 35): 16 bytes at 0x14203316c while the full-bright byte 0x1420330a6 is set, else zero.
		std::array<float, 4> characterLight{};
		std::uint8_t fullBright = 0;

		// IBLParams (PS 29): x is shader +0xcc, yzw shader +0xd0 if the byte at +0xf0 is set, else +0xe0.
		// False while the lighting shader is not known yet, which fails EvaluateVanilla.
		bool shaderKnown = false;
		std::array<float, 4> ibl{};

		// The character light's t11: the render target index at 0x142033db0, bound only while not negative, -1 while a cell loads.
		// It alternates frame to frame.
		std::int32_t characterLightTarget = -1;

		// The view t5 takes when an envmap material has no mask: the default texture global at 0x14328ccb8.
		ID3D11ShaderResourceView* envMaskDefaultView = nullptr;

		// Every render target's SRV, for t0 and t11.
		std::array<ID3D11ShaderResourceView*, kVanillaRenderTargetCapacity> renderTargetViews{};
	};

	/** @brief Render thread, at the frame's start. */
	VanillaFrame SampleVanillaFrame();
}
