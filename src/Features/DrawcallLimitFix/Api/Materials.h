#pragma once

// Material contributors: a CS feature's hook on the Lighting shader's SetupMaterial, ported as data plus per-event callbacks.
// The hook chain has two stages. Replace contributors wrap vanilla SetupMaterial, and the first active one that takes a pass evaluates it.
// After contributors each write on top once the pass is covered.
// Core assigns the declared texture registers, in ascending order across all contributors, to the frozen feature texture slots.
// Callbacks run per material and pass evaluation, never per object per frame.

#include <cstdint>
#include <span>
#include <string_view>

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF::Api
{
	// Defined in Core/Extensions/Materials.h.
	class MaterialContributorFrame;
	class MaterialContributorEvaluation;

	/** @brief Where a contributor sits in SetupMaterial's hook chain. */
	enum class MaterialStage : std::uint8_t
	{
		Replace,  // wraps vanilla SetupMaterial
		After,    // runs after vanilla and the replacement
	};

	/** @brief What a contributor's evaluate made of the pass. */
	enum class MaterialOutcome : std::uint8_t
	{
		Made,       // the record is written
		Uncovered,  // the material stays native
		HandBack,   // Replace only. Nothing written, and vanilla evaluates it instead.
	};

	/**
	 * @brief A contributor's own material class, captured like a Lighting material. All three callbacks or none.
	 *
	 * - bytes: at most MaterialPort::kMaxMaterialBytes, or the material is uncovered.
	 * - textureFields: byte offsets of its NiSourceTexture fields. Sets a_count rather than adding to it.
	 * - vanillaAsBase: when vanilla evaluates it, treat it as BSLightingShaderMaterialBase. False leaves it uncovered.
	 */
	struct MaterialClass
	{
		bool (*owns)(const void* a_vtable) = nullptr;
		std::uint32_t (*bytes)(const RE::BSShaderMaterial& a_material) = nullptr;
		void (*textureFields)(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count) = nullptr;
		bool vanillaAsBase = true;
	};

	/**
	 * @brief A material contributor. Core orders contributors by stage, then order, then registration.
	 *
	 * - textureRegisters: strictly ascending PS registers in static storage. Local index n is the register at position n.
	 * - signatureBits: pass descriptor bits that takesPass, covers or evaluate branch on.
	 * - sample: render thread at the frame's start. Must be cheap and must not write to the engine.
	 * - takesPass: Replace only. Pure.
	 * - covers: After only. Pure. False means an input is not ready, so the material is retried with a later frame.
	 * - evaluate: pure, any thread. An After contributor writes on top of the covered record and should return Made.
	 */
	struct MaterialContributor
	{
		std::string_view name;
		MaterialStage stage = MaterialStage::After;
		std::uint8_t order = 0;
		std::span<const std::uint32_t> textureRegisters;
		std::uint32_t witnessWords = 0;
		std::uint32_t signatureBits = 0;
		MaterialClass materialClass;
		void (*sample)(MaterialContributorFrame& a_frame) = nullptr;
		bool (*takesPass)(const MaterialContributorEvaluation& a_evaluation) = nullptr;
		bool (*covers)(const MaterialContributorEvaluation& a_evaluation) = nullptr;
		MaterialOutcome (*evaluate)(MaterialContributorEvaluation& a_evaluation) = nullptr;
	};
}
