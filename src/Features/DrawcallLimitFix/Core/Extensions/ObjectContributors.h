#pragma once

// Core's side of the object contributor contracts in Api/ObjectContributors.h.
// Registered contributions are stored here and read as data by eligibility, the walk, captures and classification.
// Also holds the Lighting join rules and the calls captures, joins and derives make into them.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace RE
{
	class BSShaderProperty;
	class NiNode;
	class TESObjectREFR;
}

namespace DCLF::Api
{
	/** @brief What a shading contribution's derive gets: the sampled property and the zero-filled feature block to write. */
	class ShadingDerivation
	{
	public:
		const RE::BSShaderProperty& property;
		std::span<float, kShadingBlockWords> block;
	};

	/** @brief What a Lighting join rule reads of an object. Built per join and per derive. Read-only. */
	class LightingJoinView
	{
	public:
		const SceneCapture::PropertyRecord& property;
		const SceneCapture::LeafView& leaf;
		std::int64_t geometryExtra = SceneCapture::GeometryRecord::kNoExtra;  // DeclareGeometryExtra's value, or kNoExtra or kWrongExtra
		std::uint32_t referenceFacts = 0;  // bit per ReferenceFactId
		std::uint8_t contributorBits = 0;

		std::uint64_t PropertyFlags() const { return property.flags; }  // BSShaderProperty::EShaderPropertyFlag bits
		float PropertyAlpha() const { return property.alpha; }
		std::uint32_t MaterialFeature() const { return property.feature; }  // BSShaderMaterial::Feature
		bool HasMaterial() const { return property.material != nullptr; }
		bool Fact(ReferenceFactId a_fact) const { return a_fact.index < 32 && ((referenceFacts >> a_fact.index) & 1u); }
		bool Skinned() const { return leaf.Skinned(); }
	};
}

namespace DCLF::Extensions
{
	/** @brief Number of Lighting technique slots a contribution may name. */
	inline constexpr std::uint32_t kTechniqueSlots = 32;
	/** @brief Floats FrameGlobals holds for the features' contributions. Grow it before registering more. */
	inline constexpr std::uint32_t kFrameGlobalsFloats = 64;

	/** @brief A technique's contribution. */
	struct TechniqueEntry
	{
		bool taken = false;
		std::uint32_t objectFlags = 0;
		Api::ObjectExtras extras = Api::ObjectExtras::None;
	};
	using TechniqueEntries = std::array<TechniqueEntry, kTechniqueSlots>;

	/** @brief Any thread, after InstallFeatures. The technique contributions by technique. */
	const TechniqueEntries& RegisteredTechniques();

	/** @brief The record flags a_technique's objects carry, or 0. */
	inline std::uint32_t TechniqueObjectFlags(const TechniqueEntries& a_techniques, std::uint32_t a_technique)
	{
		return a_technique < a_techniques.size() ? a_techniques[a_technique].objectFlags : 0u;
	}

	/** @brief Whether a_technique's objects are DCLF's: a feature took the technique. */
	inline bool TechniqueOn(const TechniqueEntries& a_techniques, std::uint32_t a_technique)
	{
		return a_technique < a_techniques.size() && a_techniques[a_technique].taken;
	}

	/** @brief Core's extras rows a_technique's objects take. */
	inline Api::ObjectExtras TechniqueExtras(std::uint32_t a_technique)
	{
		const auto& techniques = RegisteredTechniques();
		return a_technique < techniques.size() ? techniques[a_technique].extras : Api::ObjectExtras::None;
	}

	/** @brief Number of geometry type slots a contribution may name. */
	inline constexpr std::uint32_t kGeometryTypeSlots = 16;
	using GeometryTypesPorted = std::array<bool, kGeometryTypeSlots>;
	/** @brief Any thread, after InstallFeatures. */
	const GeometryTypesPorted& RegisteredGeometryTypes();

	/** @brief Whether a_type's objects are DCLF's. */
	inline bool GeometryTypeOn(std::uint8_t a_type)
	{
		const auto& types = RegisteredGeometryTypes();
		return a_type < types.size() && types[a_type];
	}

	using GeometryTypeContributions = std::array<Api::GeometryTypeContribution, kGeometryTypeSlots>;
	/** @brief Any thread, after InstallFeatures. */
	const GeometryTypeContributions& RegisteredGeometryTypeContributions();

	/** @brief a_type's contribution, or an empty one past the slots. */
	inline const Api::GeometryTypeContribution& GeometryTypeContributionOf(std::uint8_t a_type)
	{
		static constexpr Api::GeometryTypeContribution kNone{};
		const auto& types = RegisteredGeometryTypeContributions();
		return a_type < types.size() ? types[a_type] : kNone;
	}

	/** @brief Whether a_type's objects are movers. */
	inline bool GeometryTypeMoves(std::uint8_t a_type)
	{
		const auto& type = GeometryTypeContributionOf(a_type);
		return type.moves && type.ported;
	}

	/** @brief a_type's range capture, or null when its objects draw whole. */
	inline auto GeometryRangesOf(std::uint8_t a_type)
	{
		return GeometryTypeContributionOf(a_type).ranges;
	}

	/** @brief Whether a_type's objects draw captured ranges. */
	inline bool GeometryTypeRanged(std::uint8_t a_type)
	{
		return GeometryRangesOf(a_type) != nullptr;
	}

	/** @brief The item class a_type's skinless objects get. */
	inline Api::FrameValueItemClass GeometryItemClassOn(std::uint8_t a_type)
	{
		const auto& type = GeometryTypeContributionOf(a_type);
		return type.ported ? type.itemClass : Api::FrameValueItemClass::None;
	}

	/**
	 * @brief The item class the coordinator plans a record as. The frame values parity tests the same rule.
	 * Only a skinless geometry with a palette block gets its contribution's class. Everything else is None.
	 */
	inline Api::FrameValueItemClass PlannedItemClass(bool a_paletteBlock, std::uint8_t a_type, bool a_skinned)
	{
		return a_paletteBlock && !a_skinned ? GeometryTypeContributionOf(a_type).itemClass : Api::FrameValueItemClass::None;
	}

	using EntryClassRules = std::array<Api::GeometryTypeContribution::EntryRule, static_cast<std::size_t>(Api::EntryClass::Count)>;
	/** @brief Any thread, after InstallFeatures. */
	const EntryClassRules& RegisteredEntryClassRules();

	/** @brief The entry class of a geometry of a_type under a parent of mirrored kind a_parentKind. */
	inline Api::EntryClass EntryClassOf(std::uint8_t a_type, std::uint32_t a_parentKind)
	{
		const auto& entry = GeometryTypeContributionOf(a_type).entry;
		return (entry.parentKind & a_parentKind) ? entry.entryClass : Api::EntryClass::None;
	}

	inline const Api::GeometryTypeContribution::EntryRule& EntryClassRuleOf(Api::EntryClass a_class)
	{
		return RegisteredEntryClassRules()[a_class < Api::EntryClass::Count ? static_cast<std::size_t>(a_class) : 0];
	}

	/** @brief An owner class as the walk keeps it. Tracked entries hold a copy, resolved once. */
	struct OwnerClassRule
	{
		Api::EntryClass ownerClass = Api::EntryClass::None;
		bool everyFrame = false;
		bool ownMotion = false;
		bool published = false;
	};
	/** @brief Owner classes indexed by RE::FormType. */
	using OwnerClasses = std::array<OwnerClassRule, 256>;
	/** @brief Any thread, after InstallFeatures. */
	const OwnerClasses& RegisteredOwnerClasses();

	inline const OwnerClassRule& OwnerClassOfForm(std::uint8_t a_formType)
	{
		return RegisteredOwnerClasses()[a_formType];
	}

	/** @brief The owner class of a mirrored node's reference. */
	inline const OwnerClassRule& OwnerClassOf(const SceneCapture::NodeRecord& a_record)
	{
		static constexpr OwnerClassRule kNone{};
		return a_record.userData ? OwnerClassOfForm(a_record.formType) : kNone;
	}

	/** @brief The timing report's label for owner class entries. "owned" while none is registered. */
	std::string_view OwnedEntriesLabel(bool a_withoutRecord);

	/** @brief Objects whose property has any of propertyFlags are DCLF's only while the geometry type matches. */
	struct PropertyFlagRule
	{
		std::uint64_t propertyFlags = 0;
		std::uint8_t geometryType = 0xFF;
	};
	/** @brief Any thread, after InstallFeatures. Techniques that share a rule list it once. */
	std::span<const PropertyFlagRule> PropertyFlagRules();

	using ParentsTaken = std::array<std::array<bool, static_cast<std::size_t>(Api::ParentLeaves::Count)>, static_cast<std::size_t>(Api::EngineParent::Count)>;
	/** @brief Any thread, after InstallFeatures. */
	const ParentsTaken& RegisteredParents();

	/** @brief Whether a_leaves under a_parent are DCLF's: a feature took the pair. */
	inline bool ParentAdmits(Api::EngineParent a_parent, Api::ParentLeaves a_leaves)
	{
		return RegisteredParents()[static_cast<std::size_t>(a_parent)][static_cast<std::size_t>(a_leaves)];
	}

	using ObjectClassesTaken = std::array<bool, static_cast<std::size_t>(Api::ObjectClass::Count)>;
	/** @brief Any thread, after InstallFeatures. */
	const ObjectClassesTaken& RegisteredObjectClasses();

	/** @brief Whether a_class's objects are DCLF's: a feature took the class at install. */
	inline bool ObjectClassOn(Api::ObjectClass a_class)
	{
		return RegisteredObjectClasses()[static_cast<std::size_t>(a_class)];
	}

	using CategoryRoots = std::array<RE::NiNode* (*)(), static_cast<std::size_t>(Api::ObjectClass::Count)>;
	/** @brief Any thread, after InstallFeatures. */
	const CategoryRoots& RegisteredCategoryRoots();
	using CategoryRootGathers = std::array<void (*)(Api::CategoryRootSink&), static_cast<std::size_t>(Api::ObjectClass::Count)>;
	/** @brief Any thread, after InstallFeatures. A null entry means the class lists no roots this way. */
	const CategoryRootGathers& RegisteredCategoryRootGathers();
	/** @brief Any thread, after InstallFeatures. A null gather means none. */
	const Api::UnheldRootsContribution& RegisteredUnheldRoots();

	/** @brief Runs every shading contribution matching a_property's flags, in registration order, into a_block. */
	void DeriveShading(const RE::BSShaderProperty& a_property, std::span<float, Api::kShadingBlockWords> a_block);
	/** @brief The feature block's rule from the shading contribution that claims it. Empty when no feature does. */
	struct ShadingBlock
	{
		std::uint32_t objectFlags = 0;
		std::uint32_t blockWords = 0;
		std::span<const Api::ShadingVariable> variables;
		std::uint64_t vsMask = 0;
	};
	/** @brief Any thread, after InstallFeatures. */
	const ShadingBlock& ShadingBlockRule();
	/** @brief Render thread. Fills a_out, zeroed, with every frame-globals contribution's floats. */
	void CaptureFrameGlobals(std::array<float, kFrameGlobalsFloats>& a_out);

	// ---- The Lighting join rules.

	/** @brief Most reference facts, one bit each in the node record. Grow it before registering more. */
	inline constexpr std::size_t kReferenceFactSlots = 4;
	/** @brief Most property bits registrations. They share the property record's 8 contributor bits. */
	inline constexpr std::size_t kPropertyBitsSlots = 4;
	/** @brief Most Lighting object flags. Grow it before registering more. */
	inline constexpr std::size_t kLightingObjectFlagSlots = 2;
	/** @brief Most derivePass callbacks. */
	inline constexpr std::size_t kLightingPassDeriveSlots = 2;
	/** @brief Pass descriptor bits 3-8, the light counts, which the engine's GetPixelTechnique drops. */
	inline constexpr std::uint32_t kLightingPixelSplitBits = 0x1f8u;
	/** @brief Number of spare bits in a Lighting key. */
	inline constexpr std::uint32_t kLightingKeySpareBits = 3;

	struct ReferenceFactEntry
	{
		std::string name;
		bool none = true;
		decltype(Api::ReferenceFact::sample) sample = nullptr;
	};
	struct PropertyBitsEntry
	{
		std::string name;
		std::uint8_t bits = 0;
		decltype(Api::PropertyBits::capture) capture = nullptr;
	};
	/** @brief The pass rules' masks, ORed across registrations. */
	struct LightingPassMasks
	{
		std::uint32_t pixelKeep = 0;
		std::uint32_t rawWhen = 0, rawSet = 0, rawFlip = 0;
	};
	/** @brief Every Lighting join rule. Written only during InstallFeatures and read-only after. */
	struct LightingJoinRules
	{
		std::array<ReferenceFactEntry, kReferenceFactSlots> facts{};  // by ReferenceFactId::index
		std::uint32_t factCount = 0;
		std::uint32_t factsNone = 0;  // bit f is fact f's none value
		std::string geometryExtra;    // empty means none
		std::array<PropertyBitsEntry, kPropertyBitsSlots> propertyBits{};
		std::uint32_t propertyBitsCount = 0;
		std::uint8_t propertyBitsTaken = 0;
		std::array<Api::LightingObjectFlag, kLightingObjectFlagSlots> objectFlags{};
		std::uint32_t objectFlagCount = 0;
		Api::LightingKeyBits keyBits{};  // null keyBits means none
		LightingPassMasks passMasks{};
		std::array<decltype(Api::LightingPassRules::derivePass), kLightingPassDeriveSlots> derives{};
		std::uint32_t deriveCount = 0;
	};
	/** @brief Any thread, after InstallFeatures. */
	const LightingJoinRules& RegisteredLightingJoinRules();

	/** @brief Every reference fact of a_reference, one bit each. Runs in the engine-read window. A null reference gives each fact's none value. */
	std::uint32_t ReferenceFactsOf(const RE::TESObjectREFR* a_reference);
	/** @brief The facts of a node without a reference. */
	inline std::uint32_t ReferenceFactsNone() { return RegisteredLightingJoinRules().factsNone; }
	/** @brief The geometry extra data name the geometry capture reads, or empty for none. */
	inline std::string_view GeometryExtraName() { return RegisteredLightingJoinRules().geometryExtra; }
	/** @brief Every property bits capture's bits of a_property. */
	std::uint8_t CapturePropertyBits(const RE::BSShaderProperty& a_property);
	/** @brief Per join. The object flags whose applies holds for a_view. */
	std::uint32_t LightingObjectFlagsOf(const Api::LightingJoinView& a_view);
	/** @brief The record flags paired with a_objectFlags' Lighting object flags. */
	inline std::uint32_t LightingRecordFlagsOf(std::uint32_t a_objectFlags)
	{
		const auto& rules = RegisteredLightingJoinRules();
		std::uint32_t flags = 0;
		for (std::uint32_t i = 0; i < rules.objectFlagCount; ++i)
			flags |= (a_objectFlags & rules.objectFlags[i].objectFlag) ? rules.objectFlags[i].recordFlag : 0u;
		return flags;
	}
	/** @brief Per join. The Lighting key's spare bits for a_view, or 0 without a claimant. */
	std::uint32_t LightingKeyBitsOf(const Api::LightingJoinView& a_view);
	/** @brief Per new pipeline key. The extraFeatureDescriptor for a_keyBits, or 0 without a claimant. */
	std::uint32_t LightingKeyPermutation(std::uint32_t a_keyBits);
	inline const LightingPassMasks& LightingPassRule() { return RegisteredLightingJoinRules().passMasks; }
	/** @brief The pixel split bits PixelDescriptorFromPass clears. */
	inline std::uint32_t LightingPixelDropped() { return kLightingPixelSplitBits & ~LightingPassRule().pixelKeep; }
	/** @brief a_raw with the raw technique rule applied. */
	inline std::uint32_t LightingRawTechnique(std::uint32_t a_raw)
	{
		const auto& rule = LightingPassRule();
		if (a_raw & rule.rawWhen) {
			a_raw |= rule.rawSet;
			a_raw ^= rule.rawFlip;
		}
		return a_raw;
	}
	/** @brief Per derive. a_pass after every derivePass, in registration order. */
	std::uint32_t DeriveLightingPass(std::uint32_t a_pass, const Api::LightingJoinView& a_view);
}
