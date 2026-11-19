#pragma once

// What a feature adds to the set of objects DCLF draws. There are five kinds, one section each:
// 1. Objects picked by an engine key, such as a Lighting technique or a geometry type.
// 2. Objects Core recognises but holds back until a feature takes their class.
// 3. Per-object values derived from a feature's property, and the engine globals they read.
// 4. Material ports for engine shaders other than Lighting.
// 5. Join rules for CS features that hook the Lighting shader.
// Core reads these registrations as data. None of the callbacks run per object per frame.
// Each enum value in section 2 belongs to one feature, and the install fails if a second one claims it.

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Api/EntryRanges.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace RE
{
	class BSShaderProperty;
	class NiNode;
	class TESObjectREFR;
}

namespace DCLF::Api
{
	// Defined in Core/Extensions/ObjectContributors.h.
	class ShadingDerivation;
	class LightingJoinView;
	// Defined in Core/Extensions/MaterialPorts.h.
	class MaterialPortCapture;
	class MaterialPortDerivation;
	class MaterialPortEvaluation;
	class MaterialPortShading;
	enum class EntryClass : std::uint8_t;
	enum class FrameValueItemClass : std::uint8_t;

	// ---- 1. Membership by an engine key.

	/** @brief Extra per-object vertex shader rows a technique's objects need beyond their record. */
	enum class ObjectExtras : std::uint8_t
	{
		None,
		LandBlend,
	};

	/**
	 * @brief An engine Lighting technique whose objects the feature ports.
	 *
	 * - propertyFlags, geometryType: objects whose property has any of these flags belong to the feature whatever technique they use.
	 *   They are DCLF's only while the toggle is on and the geometry matches, otherwise they stay native as LOD.
	 */
	struct TechniqueContribution
	{
		std::uint32_t technique = ~0u;
		ToggleId toggle;
		std::uint32_t objectFlags = 0;
		std::uint64_t propertyFlags = 0;
		std::uint8_t geometryType = 0xFF;
		ObjectExtras extras = ObjectExtras::None;
	};

	/**
	 * @brief While a classify input holds, a contributed technique also needs a second toggle.
	 * Used when another CS feature draws those objects its own way, such as Terrain Blending.
	 */
	struct TechniqueGate
	{
		std::uint32_t technique = ~0u;
		ClassifyInputId whileOn;
		ToggleId needs;
	};

	/** @brief Mirrored node kind bit for a BSFaceGenNiNode, tested by an entry class rule. */
	inline constexpr std::uint32_t kMirroredKindFaceGen = 1u << 7;

	/**
	 * @brief An engine geometry type whose objects the feature ports while its toggle is on.
	 *
	 * - toggle: may be empty only when the contribution sets just the entry rule.
	 * - propertyFlags: limits the objects to Lighting properties with any of these flags. 0 means every object of the type.
	 * - itemClass: the FrameValues item class for its skinless objects that have a palette block.
	 * - moves: its objects change every frame without events, such as a particle system's bounds, so they are placed every frame.
	 * - entry: the entry class its objects get when under a parent of entry.parentKind, whatever the toggle.
	 *   perFrame entries are classified every frame. holdsParent keeps the parent node alive as the entry's source.
	 * - ranges: fills the index ranges to draw. Called on the capturing thread in the engine-read window, once per record capture.
	 */
	struct GeometryTypeContribution
	{
		struct EntryRule
		{
			EntryClass entryClass{};
			std::uint32_t parentKind = 0;  // kMirroredKind* bits
			bool perFrame = false;
			bool holdsParent = false;
		};

		std::uint8_t geometryType = 0xFF;
		ToggleId toggle;
		std::uint64_t propertyFlags = 0;
		FrameValueItemClass itemClass{};
		bool moves = false;
		EntryRule entry;
		void (*ranges)(const void* a_geometry, std::vector<EntryRange>& a_out) = nullptr;
	};

	// ---- 2. Membership by a Core class.

	/** @brief An engine parent class whose leaves Core rejects unless a parent contribution admits them. */
	enum class EngineParent : std::uint8_t
	{
		Billboard,  // NiBillboardNode. Its OnVisible turns it to face each culling camera.
		Switch,     // NiSwitchNode. Its OnVisible culls all but the selected child.
		Actor,      // A node whose reference is an actor.
		Count,
	};

	/** @brief Which leaves under such a parent a parent contribution takes. */
	enum class ParentLeaves : std::uint8_t
	{
		// Skinless effect tri-shapes under exactly one billboard. The billboard's face mode goes in the key so the vertex stage can turn them.
		TurnedEffectShapes,
		ParticleSystems,
		// Every leaf, drawn only in the frames where each switch selects its branch.
		SelectedLeaves,
		AllLeaves,
		Count,
	};

	/** @brief Lets the feature port the leaves under an engine parent class that Core would otherwise reject. */
	struct ParentContribution
	{
		EngineParent parent = EngineParent::Billboard;
		ParentLeaves leaves = ParentLeaves::TurnedEffectShapes;
		ToggleId toggle;
	};

	/** @brief An object class Core can derive but leaves native until a feature takes it. */
	enum class ObjectClass : std::uint8_t
	{
		// Decals (accumulation hints 2 and 3) and the opaque effects drawn through the opaque decal group.
		Decal,
		// The layers of a multi-index shape and the MultiIndexSparkle technique.
		Layer,
		// Object LOD sub-index shapes under TES::lodLandRoot, drawn by their visible segments.
		LodObjects,
		// Terrain LOD land block shapes under TES::lodLandRoot.
		LodTerrain,
		// Tree LOD instance groups, drawn from mirrored instance records as the TreeInstances draw family.
		TreeLod,
		// BSEffectShaderProperty objects, drawn by the Effect port.
		Effect,
		// Opaque effects drawn through the opaque decal group without its bias.
		// Read as taken whatever its toggle, because the Effect port only runs while Effect is on.
		OpaqueEffect,
		// Shapes with a single-partition NiSkinInstance.
		Skinned,
		// Skins with several partitions, or a BSDismemberSkinInstance. Needs Skinned's toggle too.
		SkinPartitions,
		// Blended objects. This class gates the whole forward epoch.
		Forward,
		// Refraction objects, drawn into the refraction normals. Needs Forward's toggle too.
		Refraction,
		// Grass instance shapes, drawn from mirrored instance records as the Grass draw family.
		Grass,
		Count,
	};

	/**
	 * @brief Makes an object class DCLF's while the toggle is on.
	 *
	 * - categoryRoot: the engine root the class's objects live under, or null for none yet. Called once per frame on the render thread.
	 *   Two classes may share a root.
	 */
	struct ObjectClassContribution
	{
		ObjectClass objectClass = ObjectClass::Count;
		ToggleId toggle;
		RE::NiNode* (*categoryRoot)() = nullptr;
	};

	/** @brief Receives the roots listed by UnheldRootsContribution::gather. */
	class UnheldRootSink
	{
	public:
		/** @brief Adds one root. It must stay alive until leave is called. */
		virtual void Add(RE::NiNode* a_root) = 0;

	protected:
		~UnheldRootSink() = default;
	};

	/**
	 * @brief Parentless roots owned by an engine array, such as the particle manager's master particle systems.
	 * The engine frees them on a low reference count, so DCLF never holds a reference. Every removal must arrive as an event.
	 *
	 * - owner: the node whose child edits add or remove roots. An edit triggers a fresh gather and diff.
	 * - enter, leave: bracket gather and the mirror pushes, so no removal lands between listing a root and capturing it.
	 * At most one feature contributes this.
	 */
	struct UnheldRootsContribution
	{
		RE::NiNode* (*owner)() = nullptr;
		void (*enter)() = nullptr;
		void (*leave)() = nullptr;
		void (*gather)(UnheldRootSink& a_sink) = nullptr;
	};

	/**
	 * @brief A class of tracked entries whose records wait on a coordinator component.
	 * The objects are DCLF's only while the component that follows the class takes it.
	 */
	enum class EntryClass : std::uint8_t
	{
		None,
		// A dynamic shape under a BSFaceGenNiNode. Its positions come from the engine's face morphing, not its buffers.
		FaceShape,
		// An entry owned by an actor, including carried items. An entry can be both this and FaceShape.
		ActorOwned,
		Count,
	};

	/**
	 * @brief An owner class: the tracked entries whose leaf's reference has form type ownerFormType.
	 *
	 * - name: what the walk's timing report calls these entries.
	 * - everyFrame: the records change without events, such as an actor's equipment, so they are rebuilt every frame.
	 * - ownMotion: the entries move with nothing on their chain to say so, such as an actor's skeleton. Needs everyFrame.
	 * - published: the members are listed each frame for shading fields that name the class.
	 */
	struct EntryClassContribution
	{
		EntryClass entryClass = EntryClass::None;
		std::string_view name;
		std::uint8_t ownerFormType = 0;
		bool everyFrame = false;
		bool ownMotion = false;
		bool published = false;
	};

	// ---- 3. Derivations.

	/** @brief Size of the feature block in the shading row and object record. Frozen by DCLFObjects.hlsli. */
	inline constexpr std::uint32_t kShadingBlockWords = 8;

	/**
	 * @brief A Lighting VS PerGeometry variable supplied from the feature block.
	 * Only the first `compared` floats are checked by capture parity, because the rest depend on native draw order.
	 */
	struct ShadingVariable
	{
		std::uint32_t variable = 0;
		std::uint8_t firstWord = 0, floats = 0, compared = 0;
	};

	/**
	 * @brief Per-object values SetupGeometry derives for the feature's property, such as tree parameters and wind timers.
	 *
	 * - derive: writes the zero-filled block. Called once per sampled property with the flags, never per object per frame.
	 * - objectFlags, blockWords, vsVariables: one contribution owns the feature block. Objects with any of objectFlags draw
	 *   vsVariables from it instead of the pipeline template. vsVariables must point to static storage.
	 */
	struct ShadingContribution
	{
		std::string_view name;
		std::uint64_t propertyFlags = 0;  // BSShaderProperty::EShaderPropertyFlag bits
		void (*derive)(const ShadingDerivation& a_derivation) = nullptr;
		std::uint32_t objectFlags = 0;
		std::uint32_t blockWords = 0;
		std::span<const ShadingVariable> vsVariables;
	};

	/** @brief Where a frame-globals contribution's first float sits in FrameGlobals::features. */
	struct FrameGlobalsSlot
	{
		std::uint32_t first = ~0u;
		explicit operator bool() const { return first != ~0u; }
	};

	/**
	 * @brief Engine globals a feature's derivations read, captured once per frame so workers never read the engine.
	 * capture runs on the render thread at the frame's start and fills a zeroed array of `floats`.
	 */
	struct FrameGlobalsContribution
	{
		std::string_view name;
		std::uint32_t floats = 0;
		void (*capture)(float* a_out) = nullptr;
	};

	// ---- 4. Material ports.

	/**
	 * @brief A port's constants for one stage, indexed as the engine shader's Setup functions write them.
	 * Each variable is 4 floats except the `sized` ones. `scratch` variables are never read and share one region.
	 */
	struct MaterialStageSpace
	{
		std::uint32_t variables = 0;
		std::span<const std::pair<std::uint32_t, std::uint32_t>> sized;
		std::span<const std::uint32_t> scratch;
		std::uint64_t groups[3]{};
		std::uint32_t first[3]{};
		std::span<const std::string_view> names;
	};

	/** @brief Per-port rules that Core's classification, joins and material records read as data. */
	struct MaterialPortRules
	{
		std::uint32_t passEnumBase = 0;       // Engine pass enum minus the pass descriptor. Gives the order key.
		std::uint32_t deferredPixelBit = 0;   // G-buffer bit in the pixel descriptor, dropped for a forward decal
		std::uint32_t projectedPassBit = 0;   // ProjectedUV bit in the pass descriptor, or 0
		std::uint32_t frameVSVariable = ~0u;  // VS variable a record takes from the frame
		bool techniqueRows = true;            // false: keys read technique row 0
		bool rawPermutation = false;          // permutation is the raw VS and PS bits with no feature bits
		bool signatureComponents = true;      // false: no signature-sourced components
		bool emissiveAlwaysListed = false;    // emissive colour is listed whatever the property flags
		bool turnedByBillboards = false;      // TurnedEffectShapes leaves belong to this port
		bool particleGeometry = false;        // particle geometry leaves with this property belong to this port
	};

	/** @brief Most frame variables a port may declare. A material row has this many places, so grow both together. */
	inline constexpr std::size_t kMaxMaterialFrameVariables = 4;

	/** @brief One component of a frame variable: the float at `source`, times the float at `scale` unless it is kNoScale. */
	struct MaterialFrameComponent
	{
		static constexpr std::uint16_t kNoScale = 0xFFFF;
		std::uint16_t source = 0;
		std::uint16_t scale = kNoScale;
	};

	/**
	 * @brief A record variable whose value comes from the live material each frame, like TexcoordOffset.
	 * Writes that touch only bytes [first, end) then need no record rewrite.
	 * The `alternate` components apply under a pass descriptor with any of alternateBits.
	 */
	struct MaterialFrameVariable
	{
		bool pixel = false;
		std::uint32_t variable = 0;
		std::uint16_t first = 0, end = 0;
		std::array<MaterialFrameComponent, 4> components{};
		std::uint32_t alternateBits = 0;
		std::array<MaterialFrameComponent, 4> alternate{};
		const std::array<MaterialFrameComponent, 4>& ComponentsFor(std::uint32_t a_passDescriptor) const
		{
			return (a_passDescriptor & alternateBits) ? alternate : components;
		}
	};

	/**
	 * @brief Ports an engine shader other than Lighting. Core captures, classifies and draws its objects while its class is on.
	 *
	 * - textureFields: offsets of the NiSourceTexture fields in the material.
	 * - vs, ps: at most 64 variables per stage.
	 * - frameVariables: at most kMaxMaterialFrameVariables, in static storage.
	 * - capture: copies what derive and shading need. Runs in the engine-read window or under the engine call that holds the property.
	 * - derive, evaluate, shading: pure. evaluate returns false for a material the port does not cover.
	 * - name: the census label for this port's objects.
	 * - emissiveOf: the colour the emissive points at after SetExternalEmittance. Any thread. Null if it is never re-pointed.
	 */
	struct MaterialPortContribution
	{
		std::string_view name;
		std::uint8_t shaderType = 0xFF;
		ObjectClass objectClass = ObjectClass::Count;
		const void* (*propertyRtti)() = nullptr;
		const void* (*materialVtable)() = nullptr;
		std::uint32_t materialBytes = 0;
		std::span<const std::uint16_t> textureFields;
		MaterialStageSpace vs, ps;
		MaterialPortRules rules;
		std::span<const MaterialFrameVariable> frameVariables;
		void (*capture)(const MaterialPortCapture& a_capture) = nullptr;
		void (*derive)(MaterialPortDerivation& a_derivation) = nullptr;
		bool (*evaluate)(MaterialPortEvaluation& a_evaluation) = nullptr;
		void (*shading)(MaterialPortShading& a_shading) = nullptr;
		std::uint32_t (*variants)(const MaterialPortDerivation& a_derivation) = nullptr;
		std::span<const std::string_view> variantNames;
		const void* (*emissiveOf)(const RE::BSShaderProperty& a_property) = nullptr;
	};

	// ---- 5. Lighting join rules. These apply to Lighting objects only.

	/** @brief A registered reference fact's bit in the node record. */
	struct ReferenceFactId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A per-reference engine fact a join rule reads, captured with the node's record.
	 * sample runs per node capture in the engine-read window. `none` is the value for a node without a reference.
	 */
	struct ReferenceFact
	{
		std::string_view name;
		bool none = true;
		bool (*sample)(const RE::TESObjectREFR& a_reference) = nullptr;
	};

	/**
	 * @brief Per-property bits captured with a Lighting property's record, for pass and join rules to read.
	 * capture runs per property capture and must return only bits within `bits`.
	 */
	struct PropertyBits
	{
		std::string_view name;
		std::uint8_t bits = 0;
		std::uint8_t (*capture)(const RE::BSShaderProperty& a_property) = nullptr;
	};

	/**
	 * @brief A per-object flag a feature's Lighting hook sets, carried as an object flag and a record flag.
	 * Both bits are compile-time constants that Features/Manifest.cpp checks for clashes. applies runs per join.
	 */
	struct LightingObjectFlag
	{
		std::uint32_t objectFlag = 0;
		std::uint32_t recordFlag = 0;
		bool (*applies)(const LightingJoinView& a_view) = nullptr;
	};

	/**
	 * @brief The 3 spare bits of a Lighting key, which give the object a pipeline of its own. One feature may claim them.
	 * permutation maps those bits to the permutation's extraFeatureDescriptor.
	 */
	struct LightingKeyBits
	{
		std::uint32_t (*keyBits)(const LightingJoinView& a_view) = nullptr;
		std::uint32_t (*permutation)(std::uint32_t a_keyBits) = nullptr;
	};

	/**
	 * @brief What a feature's Lighting hooks change in the pass descriptor, as TruePBR's do.
	 *
	 * - pixelKeep: pixel split bits to keep. ORed across registrations.
	 * - rawWhen, rawSet, rawFlip: a raw technique with any rawWhen bit gains rawSet, then flips rawFlip. One registration only.
	 * - derivePass: optional and pure. Returns the pass descriptor as the feature's GetRenderPasses hook would leave it.
	 */
	struct LightingPassRules
	{
		std::uint32_t pixelKeep = 0;
		std::uint32_t rawWhen = 0, rawSet = 0, rawFlip = 0;
		std::uint32_t (*derivePass)(std::uint32_t a_pass, const LightingJoinView& a_view) = nullptr;
	};
}
