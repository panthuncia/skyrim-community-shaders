#pragma once

// Core's side of Api/Materials.h: the material contributors in resolved order, what their callbacks receive,
// the feature texture slots assigned to them, and the calls the Lighting port makes per event.
// Per-frame and per-object paths read only resolved data and never call a contributor.

#include "Features/DrawcallLimitFix/Api/Materials.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	struct MaterialRecord;
	namespace MaterialPort
	{
		struct MaterialSnapshot;
		struct HookFrame;
	}
}

namespace DCLF::Extensions
{
	/** @brief Most material contributors. Grow it before registering more. */
	inline constexpr std::size_t kMaterialContributorSlots = 4;
	/**
	 * @brief Number of MaterialRecord::featureTextures slots, frozen by DCLFObjects.hlsli's DCLFMaterialTextureIndex decode.
	 * Contributors' registers fill them in ascending order from slot 0.
	 */
	inline constexpr std::uint32_t kFeatureTextureSlots = 14;
	/** @brief Total frame witness words across all contributors. */
	inline constexpr std::uint32_t kMaterialContributorWitnessWords = 4;
	/** @brief A snapshot not of any contributor's class. A class is its contributor's resolved index plus 1. */
	inline constexpr std::uint8_t kNoMaterialClass = 0;
	/** @brief Marks a slot with no register. */
	inline constexpr std::uint32_t kNoFeatureTextureRegister = ~0u;

	/** @brief One contributor's frame sample. */
	struct MaterialContributorSample
	{
		bool active = false;
		std::shared_ptr<const void> held;  // shared between frames while unchanged
	};
	/** @brief Every contributor's frame sample by resolved index, plus the witness words. Sampled at the frame's start with the material frame. */
	struct MaterialContributorSamples
	{
		std::array<MaterialContributorSample, kMaterialContributorSlots> contributors{};
		std::array<std::uint32_t, kMaterialContributorWitnessWords> words{};

		std::uint32_t ActiveBits() const
		{
			std::uint32_t bits = 0;
			for (std::size_t c = 0; c < contributors.size(); ++c)
				bits |= contributors[c].active ? 1u << c : 0u;
			return bits;
		}
	};

	/** @brief What one evaluation hands the contributors: the material snapshot and class, the pass, and the frame's hook frame and samples. */
	struct MaterialContributorInputs
	{
		const MaterialPort::MaterialSnapshot& snapshot;
		std::uint8_t materialClass = kNoMaterialClass;
		std::uint32_t pass = 0;
		const MaterialPort::HookFrame& hook;
		const MaterialContributorSamples& samples;
	};
}

namespace DCLF::Api
{
	/**
	 * @brief What a contributor's sample gets on the render thread at the frame's start.
	 * An inactive contributor is not called this frame. Equal witness words must mean equal records.
	 */
	class MaterialContributorFrame
	{
	public:
		MaterialContributorFrame(const MaterialPort::HookFrame& a_hook, Extensions::MaterialContributorSample& a_sample, std::span<std::uint32_t> a_words) :
			hook(a_hook), sample(a_sample), words(a_words) {}

		const MaterialPort::HookFrame& hook;

		void SetActive(bool a_active) { sample.active = a_active; }
		void Hold(std::shared_ptr<const void> a_held) { sample.held = std::move(a_held); }
		void Word(std::uint32_t a_index, std::uint32_t a_value)
		{
			if (a_index < words.size())
				words[a_index] = a_value;
		}

	private:
		Extensions::MaterialContributorSample& sample;
		std::span<std::uint32_t> words;
	};

	/**
	 * @brief What takesPass, covers and evaluate get: the snapshot, the pass, the hook frame and the contributor's own sample.
	 * Only evaluate has a record to write.
	 */
	class MaterialContributorEvaluation
	{
	public:
		MaterialContributorEvaluation(const Extensions::MaterialContributorInputs& a_inputs, std::size_t a_contributor, MaterialRecord* a_out);

		const MaterialPort::MaterialSnapshot& snapshot;
		const MaterialPort::HookFrame& hook;

		// The pass descriptor, as BSLightingShader+0x94 would hold it. Held returns null if the held table has a different type.
		std::uint32_t Pass() const { return inputs.pass; }
		bool Active() const;
		template <class T>
		const T* Held() const
		{
			return static_cast<const T*>(HeldPointer());
		}
		// Snapshot reads. At returns zero past the snapshot's bytes.
		std::uint32_t Feature() const;
		std::uint32_t Size() const;
		std::uint8_t MaterialClass() const { return inputs.materialClass; }
		bool OwnClass() const;
		template <class T>
		T At(std::size_t a_offset) const
		{
			T value{};
			Read(a_offset, &value, sizeof(T));
			return value;
		}
		bool HasTexture(std::uint16_t a_offset) const { return At<const void*>(a_offset) != nullptr; }
		ID3D11ShaderResourceView* View(std::uint16_t a_offset) const;

		// Record writes, evaluate only. Stage floats use Lighting's StageLayout order. Feature textures go by local index, and extras are dropped.
		void ResetRecord();
		float* VSFloats();
		float* PSFloats();
		void SetFilterMode(std::uint32_t a_slot, std::uint32_t a_mode);
		void BindTexture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_addressMode);
		void FeatureTexture(std::uint32_t a_local, ID3D11ShaderResourceView* a_view);

	private:
		const Extensions::MaterialContributorInputs& inputs;
		std::size_t contributor;  // resolved index
		MaterialRecord* out;      // null for takesPass and covers

		const void* HeldPointer() const;
		// Copies a_bytes at a_offset into a_out if the snapshot has them, otherwise leaves a_out as it is.
		void Read(std::size_t a_offset, void* a_out, std::size_t a_bytes) const;
	};
}

namespace DCLF::Extensions
{
	/** @brief A contributor as registered and resolved, with its assigned feature texture slots, witness words and class. */
	struct MaterialContributorEntry
	{
		std::string name;
		Api::MaterialStage stage = Api::MaterialStage::After;
		std::uint8_t order = 0;
		std::span<const std::uint32_t> registers;
		std::uint32_t firstSlot = 0;
		std::uint32_t witnessWords = 0, firstWord = 0;
		std::uint32_t signatureBits = 0;
		Api::MaterialClass materialClass;
		std::uint8_t classIndex = kNoMaterialClass;
		decltype(Api::MaterialContributor::sample) sample = nullptr;
		decltype(Api::MaterialContributor::takesPass) takesPass = nullptr;
		decltype(Api::MaterialContributor::covers) covers = nullptr;
		decltype(Api::MaterialContributor::evaluate) evaluate = nullptr;
	};

	struct MaterialContributorTable
	{
		std::array<MaterialContributorEntry, kMaterialContributorSlots> entries{};  // by resolved index: stage, then order, then registration
		std::size_t count = 0;
		// Each slot's register, ascending. kNoFeatureTextureRegister past featureTextures.
		std::array<std::uint32_t, kFeatureTextureSlots> featureTextureRegisters = [] {
			std::array<std::uint32_t, kFeatureTextureSlots> registers{};
			registers.fill(kNoFeatureTextureRegister);
			return registers;
		}();
		std::uint32_t featureTextures = 0;
		std::uint32_t signatureBits = 0;
		std::uint32_t witnessWords = 0;
		bool classes = false;
	};

	/** @brief Main thread, at the end of InstallFeatures. Orders the contributors and assigns their slots, witness words and classes. */
	void ResolveMaterialContributors(std::span<const MaterialContributorEntry> a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const MaterialContributorTable& MaterialContributors();
	inline std::span<const MaterialContributorEntry> MaterialContributorList()
	{
		const auto& table = MaterialContributors();
		return std::span(table.entries).first(table.count);
	}

	// Resolved data, readable from any thread after InstallFeatures.
	/** @brief The number of feature texture slots in use. Later slots are never written. */
	inline std::uint32_t FeatureTextureCount() { return MaterialContributors().featureTextures; }
	/** @brief Slot a_slot's PS texture register. */
	inline std::uint32_t FeatureTextureRegister(std::uint32_t a_slot)
	{
		return a_slot < kFeatureTextureSlots ? MaterialContributors().featureTextureRegisters[a_slot] : kNoFeatureTextureRegister;
	}
	/** @brief The slot for PS texture register a_register, or -1. */
	inline int FeatureTextureSlotOf(std::uint32_t a_register)
	{
		const auto& table = MaterialContributors();
		for (std::uint32_t slot = 0; slot < table.featureTextures; ++slot)
			if (table.featureTextureRegisters[slot] == a_register)
				return static_cast<int>(slot);
		return -1;
	}
	/** @brief Pass bits the contributors branch on, ORed into the material signature. */
	inline std::uint32_t MaterialSignatureBits() { return MaterialContributors().signatureBits; }

	// Material capture. Any thread the material is alive on.
	/** @brief The class of a material with vtable a_vtable, or kNoMaterialClass. */
	std::uint8_t MaterialClassOf(const void* a_vtable);
	std::uint32_t MaterialClassBytes(std::uint8_t a_class, const RE::BSShaderMaterial& a_material);
	/** @brief Writes class a_class's texture fields of a_material and sets a_count. */
	void MaterialClassTextureFields(std::uint8_t a_class, const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count);
	bool MaterialClassVanillaAsBase(std::uint8_t a_class);

	/** @brief Render thread, at the frame's start. Resets and fills every contributor's sample in resolved order. */
	void SampleMaterialContributors(const MaterialPort::HookFrame& a_hook, MaterialContributorSamples& a_out);

	// SetupMaterial's chain. All pure, run on the scene lane or the join pool:
	//   o = EvaluateReplace(in, out); covered = o == Made || (o == HandBack && vanilla's evaluation);
	//   if (!covered || MaterialRetryable(in)) uncovered; else EvaluateAfter(in, out).
	/** @brief Evaluates with the first active Replace contributor that takes the pass. Returns HandBack when none does. */
	Api::MaterialOutcome EvaluateReplace(const MaterialContributorInputs& a_inputs, MaterialRecord& a_out);
	/** @brief Whether an active After contributor is not ready yet, so the material must be evaluated again later. */
	bool MaterialRetryable(const MaterialContributorInputs& a_inputs);
	/** @brief Runs every active After contributor's evaluate, in order. */
	void EvaluateAfter(const MaterialContributorInputs& a_inputs, MaterialRecord& a_out);
}
