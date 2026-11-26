#pragma once

// The constant records DCLF evaluates in place of the engine's setup functions: ConstantBlock and the Lighting layouts,
// MaterialRecord for SetupMaterial, TechniqueConstants for SetupTechnique and GeometryConstants for SetupGeometry.

#include <array>
#include <cstdint>
#include <cstring>

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/** @brief Lighting shader constant variable counts. */
	inline constexpr std::uint32_t kLightingVSVariables = 17;
	inline constexpr std::uint32_t kLightingPSVariables = 51;
	inline constexpr std::uint32_t kPixelTextureSlots = 16;

	// A material contributor's own textures go to MaterialRecord::featureTextures, by the slot Core assigned them.
	static_assert(Extensions::kFeatureTextureSlots <= 32);  // one bit each in the build's row state

	/** @brief Bits for components the engine did not write. A quiet NaN no engine code produces. A signaling NaN would be quietened by float copies. */
	inline constexpr std::uint32_t kUnwrittenBits = 0x7fdadbadu;

	/** @brief Filter mode for a sampler slot nothing set, so the draw inherits the previous draw's. */
	inline constexpr std::uint32_t kUnwrittenFilterMode = 0xffffffffu;

	/** @brief Floats in a constant block: every offset a constant-table byte can express, plus a float4. */
	inline constexpr std::uint32_t kConstantBlockFloats = 256 + 4;

	/** @brief Where each Lighting variable lives in a ConstantBlock. The per-geometry point light arrays share the scratch region. */
	const StageLayout& LightingVSLayout();
	const StageLayout& LightingPSLayout();

	/** @brief One constant group's values in StageLayout order. Unwritten components hold kUnwrittenBits. */
	struct ConstantBlock
	{
		alignas(16) std::array<float, kConstantBlockFloats> floats;

		void Reset();
		bool Written(std::uint32_t a_float) const { return std::bit_cast<std::uint32_t>(floats[a_float]) != kUnwrittenBits; }
		/** @brief Bit equality, because kUnwrittenBits is a NaN and never compares equal as a float. */
		bool SameBits(const ConstantBlock& a_other) const { return std::memcmp(floats.data(), a_other.floats.data(), sizeof(floats)) == 0; }
		bool SameBits(const ConstantBlock& a_other, std::uint32_t a_float) const
		{
			return std::bit_cast<std::uint32_t>(floats[a_float]) == std::bit_cast<std::uint32_t>(a_other.floats[a_float]);
		}
	};

	/** @brief What BSLightingShader::SetupMaterial binds for one (material, pass descriptor) pair. */
	struct MaterialRecord
	{
		ConstantBlock vs;
		ConstantBlock ps;
		std::array<ID3D11ShaderResourceView*, kPixelTextureSlots> textures{};
		std::array<std::uint32_t, kPixelTextureSlots> addressModes{};
		std::array<std::uint32_t, kPixelTextureSlots> filterModes{};  // kUnwrittenFilterMode where SetupMaterial leaves it
		std::uint32_t textureWritten = 0;
		// The contributors' registers by slot. Null where the feature binds nothing, so the draw takes the frame's.
		std::array<ID3D11ShaderResourceView*, Extensions::kFeatureTextureSlots> featureTextures{};
		// Which shader's SetupMaterial made it, which sets the variable space of vs and ps.
		std::uint8_t kind = kMaterialKindLighting;

		/** @brief Member-wise equality. Not memcmp, because the 16-byte alignment leaves 12 bytes of padding that are never written. */
		bool operator==(const MaterialRecord& a_other) const
		{
			return vs.SameBits(a_other.vs) && ps.SameBits(a_other.ps) &&
			       textures == a_other.textures && addressModes == a_other.addressModes &&
			       filterModes == a_other.filterModes && textureWritten == a_other.textureWritten && featureTextures == a_other.featureTextures &&
			       kind == a_other.kind;
		}
	};

	/** @brief What BSLightingShader::SetupTechnique writes for one pass descriptor: fog, colour output clamp and sampler filter modes. */
	struct TechniqueConstants
	{
		ConstantBlock vs;
		ConstantBlock ps;
		std::array<std::uint32_t, kPixelTextureSlots> filterModes{};  // kUnwrittenFilterMode where SetupTechnique leaves it
		bool shadowMask = false;  // binds the shadow mask to t14 and sets VPOSOffset
		ID3D11ShaderResourceView* shadowMaskTexture = nullptr;
	};

	inline constexpr std::uint32_t kShadowMaskSlot = 14;

	/**
	 * @brief Terrain LOD's HighDetailRange as DCLF draws it: the loaded grid's centre in absolute terms, and its half extents less 15.
	 * DCLF's vertex shader subtracts the draw's eye itself.
	 */
	void LodHighDetailRange(float* a_out);

	/** @brief Render thread. What SetupTechnique reads of the engine now. */
	TechniqueInputs SampleTechniqueInputs();
	/** @brief The fog SetupTechnique reads (FUN_1414dfad0): a_sceneNode's fog property, none without one. */
	void SampleFog(const RE::ShadowSceneNode* a_sceneNode, TechniqueInputs& a_inputs);
	/** @brief Pure. SetupTechnique's writes for a pass descriptor from sampled inputs. */
	void EvaluateTechnique(std::uint32_t a_passDescriptor, const TechniqueInputs& a_inputs, TechniqueConstants& a_out);
	/** @brief EvaluateTechnique from the frame's sample. */
	inline void EvaluateTechnique(std::uint32_t a_passDescriptor, TechniqueConstants& a_out)
	{
		EvaluateTechnique(a_passDescriptor, FrameGlobals::Current().technique, a_out);
	}

	/** @brief What EvaluateTechnique reads of a pass descriptor. Descriptors with the same key evaluate the same within a frame. */
	inline std::uint32_t TechniqueKey(std::uint32_t a_passDescriptor)
	{
		const bool shadowLights = (a_passDescriptor & (1u << 13)) || (a_passDescriptor & 0x1c0u);
		const bool shadowMask = shadowLights && (a_passDescriptor & (1u << 14));
		return (((a_passDescriptor >> 24) & 0x3f) << 1) | (shadowMask ? 1u : 0u);
	}

	struct GeometryConstants
	{
		ConstantBlock vs;
		ConstantBlock ps;
	};
}
