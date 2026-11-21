#pragma once

// Core's side of material ports, section 4 of Api/ObjectContributors.h. Kind 0 is Core's Lighting and features' ports take kinds from 1.
// Resolved once at install. Per-object paths read the table as data and never call a port per object per frame.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderProperty;
}

namespace DCLF
{
	struct LightingDescriptors;
	struct MaterialRecord;
	struct BindlessShading;
	enum class Ineligible : std::uint8_t;
	namespace SceneCapture
	{
		struct PropertyRecord;
		struct LeafView;
	}
	namespace MaterialPort
	{
		struct MaterialSnapshot;
		struct MaterialFrame;
	}

	/** @brief Where each variable of a stage lives in a ConstantBlock, in floats. Matrices take 12, unread variables share scratch, the rest 4. */
	struct StageLayout
	{
		std::uint32_t count;
		std::array<std::uint8_t, 64> offset;
		std::array<std::uint8_t, 64> size;
	};
	/** @brief The material kind for Core's Lighting. */
	inline constexpr std::uint8_t kMaterialKindLighting = 0;

	/** @brief Pure. Lays out a_count variables, 4 floats each except a_sized, with a_scratch in the shared region. */
	StageLayout MakeStageLayout(std::uint32_t a_count, std::span<const std::pair<std::uint32_t, std::uint32_t>> a_sized, std::span<const std::uint32_t> a_scratch);
}

namespace DCLF::Api
{
	/** @brief What a port's capture gets: the property, and the record fields its derive and shading read. */
	class MaterialPortCapture
	{
	public:
		const RE::BSShaderProperty& property;
		const void*& emissive;
		std::uint32_t& feature;
	};

	/** @brief What a port's derive gets. It writes out and reason, where None means drawn. For variants, out is null. */
	class MaterialPortDerivation
	{
	public:
		const SceneCapture::PropertyRecord& property;
		const SceneCapture::LeafView& leaf;
		LightingDescriptors* out;
		Ineligible reason;
	};

	/** @brief What a port's evaluate gets: the snapshot, the pass descriptor and the frame's sources, plus the record to write. */
	class MaterialPortEvaluation
	{
	public:
		const MaterialPort::MaterialSnapshot& snapshot;
		std::uint32_t passDescriptor;
		const MaterialPort::MaterialFrame& frame;
		MaterialRecord& out;

		// Snapshot reads. At returns zero past the snapshot's bytes.
		std::uint8_t Port() const;
		std::uint32_t Size() const;
		template <class T>
		T At(std::size_t a_offset) const
		{
			T value{};
			Read(a_offset, &value, sizeof(T));
			return value;
		}
		bool HasTexture(std::uint16_t a_offset) const { return At<const void*>(a_offset) != nullptr; }
		ID3D11ShaderResourceView* View(std::uint16_t a_offset) const;
		// The vanilla frame's sources.
		bool FrameSampled() const;
		std::uint32_t TextureTransformBuffer() const;
		// Record writes. Stage floats use StageLayout order.
		void ResetRecord(std::uint8_t a_kind);
		float* VSFloats();
		float* PSFloats();
		void SetFilterMode(std::uint32_t a_slot, std::uint32_t a_mode);
		void BindTexture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_addressMode);

	private:
		// Copies a_bytes at a_offset into a_out if the snapshot has them, otherwise leaves a_out as it is.
		void Read(std::size_t a_offset, void* a_out, std::size_t a_bytes) const;
	};

	/** @brief What a port's shading gets: the property, its pass descriptor and whether it is a member, plus the row to write. */
	class MaterialPortShading
	{
	public:
		const RE::BSShaderProperty& property;
		std::uint32_t pass;
		bool member;
		BindlessShading& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief Number of material kinds: Lighting plus one per port. Grow it before registering more. */
	inline constexpr std::uint8_t kMaterialKinds = 2;
	/** @brief Most variables a stage's space may have. */
	inline constexpr std::uint32_t kMaxStageVariables = 64;

	/** @brief One stage of a resolved space. */
	struct MaterialStageEntry
	{
		StageLayout layout{};
		std::uint64_t groups[3]{};
		std::uint32_t first[3]{};
		std::span<const std::string_view> names;
	};
	/** @brief A kind's space: both stages, plus the floats the frame owns, which record comparisons leave out. */
	struct MaterialSpace
	{
		MaterialStageEntry vs, ps;
		std::vector<std::uint32_t> frameVSFloats;
		std::vector<std::uint32_t> declaredVSFloats, declaredPSFloats;
	};
	/** @brief A material byte span [first, end). */
	using MaterialSpan = std::pair<std::uint16_t, std::uint16_t>;

	/** @brief A kind's entry with RTTI, vtable and space resolved. */
	struct MaterialPortEntry
	{
		std::string_view name;
		std::uint8_t shaderType = 0xFF;
		Api::ObjectClass objectClass = Api::ObjectClass::Count;
		const void* rtti = nullptr;
		const void* vtable = nullptr;
		std::uint32_t materialBytes = 0;
		std::span<const std::uint16_t> textureFields;
		MaterialSpace space;
		Api::MaterialPortRules rules;
		std::span<const Api::MaterialFrameVariable> frameVariables;
		// Material bytes a frame patch owns, sorted and disjoint: the texture-transform buffers at +0xc..+0x2c, then declared frame variable spans.
		std::vector<MaterialSpan> frameSpans;
		decltype(Api::MaterialPortContribution::capture) capture = nullptr;
		decltype(Api::MaterialPortContribution::derive) derive = nullptr;
		decltype(Api::MaterialPortContribution::evaluate) evaluate = nullptr;
		decltype(Api::MaterialPortContribution::shading) shading = nullptr;
		decltype(Api::MaterialPortContribution::variants) variants = nullptr;
		std::span<const std::string_view> variantNames;
		decltype(Api::MaterialPortContribution::emissiveOf) emissiveOf = nullptr;
	};
	using MaterialPortEntries = std::array<MaterialPortEntry, kMaterialKinds>;

	/** @brief Core's Lighting as kind 0: its constant space and rules, with no callbacks. */
	const Api::MaterialPortContribution& CoreLightingPort();
	/** @brief Main thread, at the end of InstallFeatures. Builds the entries by kind. */
	void ResolveMaterialPorts(std::span<const Api::MaterialPortContribution> a_byKind, std::size_t a_count);
	/** @brief Any thread, after InstallFeatures. */
	const MaterialPortEntries& MaterialPorts();

	/** @brief The port whose property has exactly a_rtti, or 0. */
	inline std::uint8_t PortOfRtti(const void* a_rtti)
	{
		const auto& ports = MaterialPorts();
		for (std::uint8_t kind = 1; kind < kMaterialKinds; ++kind)
			if (a_rtti && ports[kind].rtti == a_rtti)
				return kind;
		return 0;
	}
	/** @brief The port whose material has a_vtable, or 0. */
	inline std::uint8_t PortOfVtable(const void* a_vtable)
	{
		const auto& ports = MaterialPorts();
		for (std::uint8_t kind = 1; kind < kMaterialKinds; ++kind)
			if (a_vtable && ports[kind].vtable == a_vtable)
				return kind;
		return 0;
	}
	/** @brief Whether port a_kind's objects are DCLF's. */
	inline bool PortOn(std::uint8_t a_kind)
	{
		return a_kind != 0 && a_kind < kMaterialKinds && ObjectClassOn(MaterialPorts()[a_kind].objectClass);
	}
	/** @brief Whether port a_kind classifies a particle geometry's leaf. */
	inline bool PortTakesParticles(std::uint8_t a_kind)
	{
		return PortOn(a_kind) && MaterialPorts()[a_kind].rules.particleGeometry;
	}
	/**
	 * @brief Whether a leaf is a shape the billboards turn: turned shapes admitted, its port's rules allow it, and it is a skinless BSTriShape.
	 * Such a leaf under exactly one billboard node is DCLF's.
	 */
	inline bool BillboardTurnedLeaf(std::uint8_t a_kind, std::uint8_t a_type, bool a_skinned)
	{
		return ParentAdmits(Api::EngineParent::Billboard, Api::ParentLeaves::TurnedEffectShapes) && a_kind != 0 &&
		       MaterialPorts()[a_kind].rules.turnedByBillboards && a_type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape) && !a_skinned;
	}
	/**
	 * @brief Whether a Lighting leaf is a refraction shape the billboards turn, such as the heat haze cards over fires and forges.
	 * Its only draw is the refraction normals, whose vertex stage turns it.
	 */
	inline bool BillboardTurnedRefraction(std::uint8_t a_kind, std::uint64_t a_flags, std::uint8_t a_type, bool a_skinned)
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		constexpr std::uint64_t kRefractionFlags = static_cast<std::uint64_t>(Flag::kRefraction) | static_cast<std::uint64_t>(Flag::kTempRefraction);
		return ParentAdmits(Api::EngineParent::Billboard, Api::ParentLeaves::TurnedEffectShapes) && ObjectClassOn(Api::ObjectClass::Refraction) &&
		       a_kind == 0 && (a_flags & kRefractionFlags) != 0 && a_type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape) && !a_skinned;
	}
	inline const Api::MaterialPortRules& PortRules(std::uint8_t a_kind) { return MaterialPorts()[a_kind].rules; }
	/** @brief Kind a_kind's constant space. */
	inline const MaterialSpace& MaterialSpaceOf(std::uint8_t a_kind) { return MaterialPorts()[a_kind].space; }
	/** @brief Kind a_kind's frame variables. */
	inline std::span<const Api::MaterialFrameVariable> FrameVariablesOf(std::uint8_t a_kind)
	{
		return a_kind < kMaterialKinds ? MaterialPorts()[a_kind].frameVariables : std::span<const Api::MaterialFrameVariable>{};
	}
	/**
	 * @brief Pure. Whether two copies of a material's bytes [a_first, a_end) differ outside the frame-owned spans.
	 * The caller checks whether a feature actually patches those spans.
	 */
	bool DifferOutsideFrameSpans(std::uint8_t a_kind, const std::byte* a_left, const std::byte* a_right, std::size_t a_first, std::size_t a_end);
	/** @brief Pure. Whether they differ inside the frame-owned spans. */
	bool DifferInFrameSpans(std::uint8_t a_kind, const std::byte* a_left, const std::byte* a_right, std::size_t a_first, std::size_t a_end);

	void CapturePort(std::uint8_t a_kind, const RE::BSShaderProperty& a_property, const void*& a_emissive, std::uint32_t& a_feature);
	/** @brief Port a_kind's derive. Returns why the object stays native, or None. */
	Ineligible DerivePort(std::uint8_t a_kind, const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out);
	/** @brief Port a_kind's variant bits, for the census. */
	std::uint32_t PortVariants(std::uint8_t a_kind, const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf);
	/** @brief Port a_kind's evaluation of a_snapshot. Returns false if not covered. */
	bool EvaluatePort(std::uint8_t a_kind, const MaterialPort::MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialPort::MaterialFrame& a_frame,
		MaterialRecord& a_out);
	void SamplePortShading(std::uint8_t a_kind, const RE::BSShaderProperty& a_property, std::uint32_t a_pass, bool a_member, BindlessShading& a_out);
}
