// The water material port: BSWaterShaderProperty with BSWaterShaderMaterial, modelled on Features/Effects' port.
// capture, derive, evaluate and shading call the phase 0 functions in Features/Water/Port and never compute a value themselves.
//
// - capture: the water flags and the stencil index into the record's feature word, the plane and the cell and flow offsets
//   into its values. kUnderwater is left out, since GetRenderPasses writes it from the camera every frame.
// - derive: Port::DeriveRenderPasses with a frame-free input, so a classification never depends on the camera. The key's
//   descriptors are the surface pass's: raw, and as CS's ModifyShaderLookup leaves them. The stencil index rides in the key's
//   spare bits (WaterScene.h). Phase 1 claims nothing: plain water returns WaterUnclaimed, the rest WaterVariant.
// - evaluate: Port::EvaluateMaterial with a neutral frame, and SetupMaterial's textures. The frame's values are in the water
//   material pool (WaterSceneRows.cpp), which the shader reads instead.
// - shading: water writes no per-object shading. The pool row comes from the WaterMaterial shading field.

#include "Features/DrawcallLimitFix/Features/Water/Scene/WaterScene.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"
#include "RenderGraph/Fatal.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string_view>
#include <vector>

namespace DCLF::Features::Water::WaterScene
{
	namespace
	{
		static_assert(ProgramFamilySlotOf(Api::ProgramFamily::Water).spareShift == kStencilIndexShift,
			"the water family's spare bits are not where the stencil index is read");
		static_assert((kStencilIndexMask >> kStencilIndexShift) == kProgramFamilySpareBits, "the stencil index is not the family's spare bits");

		constexpr std::uint32_t kVsVariables = static_cast<std::uint32_t>(Port::VsVariable::Count);
		constexpr std::uint32_t kPsVariables = static_cast<std::uint32_t>(Port::PsVariable::Count);

		// BSWaterShaderMaterial's texture fields, and the slots SetupMaterial (SE 100604, 0x1412f6a70) binds them to.
		constexpr std::uint16_t kStaticReflection = 0x38;  // t3, unless the material's +0x17D byte picks the global cube
		constexpr std::uint16_t kNormal1 = 0x40;           // t4
		constexpr std::uint16_t kNormal2 = 0x48;           // t5
		constexpr std::uint16_t kNormal3 = 0x50;           // t6
		constexpr std::uint16_t kFlowNormals = 0x58;       // t9, with the Flowmap bit
		constexpr std::uint16_t kGlobalCubeByte = 0x17D;
		constexpr std::uint16_t kTextureFields[] = { kStaticReflection, kNormal1, kNormal2, kNormal3, kFlowNormals };
		constexpr std::uint32_t kWrap = 3;         // SetupMaterial's address mode for the normal maps
		constexpr std::uint32_t kAnisotropic = 3;  // and filter mode
		constexpr std::uint32_t kBilinear = 1;     // the cube's filter mode, with address mode 0

		/** @brief One stage's space from the engine's variable order and Water.hlsl's packoffsets. Built once at install. */
		struct StageSpace
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> sized;
			std::vector<std::string_view> names;
			std::uint64_t groups[3]{};
			std::uint32_t first[3]{ ~0u, ~0u, ~0u };
			std::uint8_t firstRow[3]{ 0xFF, 0xFF, 0xFF };
		};

		template <class Variable>
		StageSpace MakeSpace(std::uint32_t a_count, std::string_view (*a_name)(Variable))
		{
			StageSpace space;
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const auto variable = static_cast<Variable>(i);
				const auto place = Port::PlacementOf(variable);
				space.names.push_back(a_name(variable));
				if (place.rows > 1)
					space.sized.emplace_back(i, place.rows * 4u);
				if (!place.present || place.group > 2)
					continue;
				space.groups[place.group] |= 1ull << i;
				// A group's first variable is the one at its lowest row.
				if (place.row < space.firstRow[place.group]) {
					space.firstRow[place.group] = place.row;
					space.first[place.group] = i;
				}
			}
			for (auto& first : space.first)
				if (first == ~0u)
					first = 0;
			return space;
		}

		const StageSpace& VsSpace()
		{
			static const StageSpace space = MakeSpace<Port::VsVariable>(kVsVariables, &Port::VsName);
			return space;
		}

		const StageSpace& PsSpace()
		{
			static const StageSpace space = MakeSpace<Port::PsVariable>(kPsVariables, &Port::PsName);
			return space;
		}

		const StageLayout& VsLayout()
		{
			static const StageLayout layout = MakeStageLayout(kVsVariables, VsSpace().sized, {});
			return layout;
		}

		const StageLayout& PsLayout()
		{
			static const StageLayout layout = MakeStageLayout(kPsVariables, PsSpace().sized, {});
			return layout;
		}

		Api::ClassifyInputId lightLimitFix;  // install

		/** @brief The frame the derive and evaluate read: nothing of the camera, no fog, unit water lighting. */
		const Port::FrameInputs& NeutralFrame()
		{
			static const Port::FrameInputs frame = [] {
				Port::FrameInputs f;
				f.waterEnabled = true;
				f.invFrameBufferRange = 1.0f;
				f.waterLighting = { 1.0f, 1.0f, 1.0f, 1.0f };
				f.renderWidth = 1;
				f.renderHeight = 1;
				return f;
			}();
			return frame;
		}

		void CaptureWater(const Api::MaterialPortCapture& a_capture)
		{
			const auto& water = static_cast<const RE::BSWaterShaderProperty&>(a_capture.property);
			const std::uint32_t flags = water.waterFlags.underlying() & ~kWaterFlagUnderwater;
			a_capture.feature = kFeatureCaptured | (flags & kFeatureFlagsMask) | (StencilIndexOf(water.unk8C) << kFeatureStencilShift);
			a_capture.emissive = nullptr;
			auto& values = a_capture.values;
			values[kValuePlane + 0] = water.plane.normal.x;
			values[kValuePlane + 1] = water.plane.normal.y;
			values[kValuePlane + 2] = water.plane.normal.z;
			values[kValuePlane + 3] = water.plane.constant;
			values[kValueOffsets + 0] = std::bit_cast<float>(water.cellX);
			values[kValueOffsets + 1] = std::bit_cast<float>(water.cellY);
			values[kValueOffsets + 2] = std::bit_cast<float>(water.flowX);
			values[kValueOffsets + 3] = std::bit_cast<float>(water.flowY);
		}

		/** @brief Pure. The port's classification of a water leaf, and the passes GetRenderPasses would make for it. */
		Classification Classify(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, Port::RenderPasses& a_passes)
		{
			using enum Classification::Refusal;
			Classification c;
			if (!(a_property.feature & kFeatureCaptured)) {
				c.refusal = NotCaptured;
				return c;
			}
			const std::uint32_t index = (a_property.feature >> kFeatureStencilShift) & 7u;
			Port::ObjectInputs object;
			object.waterFlags = a_property.feature & kFeatureFlagsMask;
			object.stencilBit = index ? 1u << index : 0u;
			object.vertexDescLow = a_leaf.geometry ? static_cast<std::uint32_t>(a_leaf.geometry->shapeVertexDesc) : 0u;
			a_passes = Port::DeriveRenderPasses(object, 0, 0, NeutralFrame());
			c.waterFlags = object.waterFlags;
			c.stencilBit = object.stencilBit;
			c.surfaceDescriptor = a_passes.surfaceDescriptor;
			c.stencilPass = a_passes.stencil.passEnum;
			c.surfaceHint = a_passes.surface.accumulationHint;
			const std::uint32_t flags = object.waterFlags;
			c.attributes = ((flags & 0x2) ? Classification::kLodBit : 0u) | ((flags & 0x1) ? Classification::kWading : 0u) |
			               (c.surfaceHint == 13 ? Classification::kHint13 : 0u) |
			               ((c.stencilPass == 0x5C00006Eu || c.stencilPass == 0x5C000070u) ? Classification::kVertexColors : 0u) |
			               ((flags & 0x8000) ? Classification::kFlowmap : 0u) | (a_leaf.Skinned() ? Classification::kSkinned : 0u);
			if (a_leaf.Skinned())
				c.refusal = Skinned;
			else if (!index)
				c.refusal = Stencil;
			else if (c.attributes & (Classification::kWading | Classification::kHint13))
				c.refusal = Wading;
			else if (!Extensions::ClassifyInputOn(lightLimitFix))
				c.refusal = NoLightLimitFix;
			return c;
		}

		void DeriveWater(Api::MaterialPortDerivation& a_derivation)
		{
			Port::RenderPasses passes;
			const Classification c = Classify(a_derivation.property, a_derivation.leaf, passes);
			if (a_derivation.leaf.geometry)
				NoteClassified(a_derivation.leaf.geometry->key, c);
			if (c.refusal != Classification::Refusal::None) {
				a_derivation.reason = Ineligible::WaterVariant;
				return;
			}
			auto& out = *a_derivation.out;
			out = {};
			out.port = kMaterialKindWater;
			out.technique = 0;
			out.pass = passes.surfaceDescriptor;
			out.derivedPass = passes.surfaceDescriptor;
			out.rawVertex = passes.surfaceDescriptor;
			out.rawPixel = passes.surfaceDescriptor;
			out.vertex = passes.lookupDescriptor;
			out.pixel = passes.lookupDescriptor;
			out.familyKeyBits = static_cast<std::uint8_t>(StencilIndexOf(c.stencilBit));
			// Phase 1 claims no water. Phase 2 returns None here for plain water.
			a_derivation.reason = Ineligible::WaterUnclaimed;
		}

		std::uint32_t WaterVariants(const Api::MaterialPortDerivation& a_derivation)
		{
			return ClassifyLeaf(a_derivation.property, a_derivation.leaf).attributes;
		}

		constexpr std::string_view kVariantNames[] = { "lod", "wading", "hint-13", "vertex-colours", "flowmap", "skinned" };

		/** @brief Writes a material row's group-1 rows into the record's floats at their variables. */
		template <class Variable, std::size_t N>
		void WriteRows(const std::array<Port::Row, N>& a_rows, std::uint32_t a_count, const StageLayout& a_layout, float* a_out)
		{
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const auto place = Port::PlacementOf(static_cast<Variable>(i));
				if (!place.present || place.group != 1)
					continue;
				for (std::uint32_t r = 0; r < place.rows && place.row + r < N; ++r) {
					const auto& row = a_rows[place.row + r];
					for (std::uint32_t c = 0; c < 4; ++c)
						if (row.written & (1u << c))
							a_out[a_layout.offset[i] + r * 4 + c] = row.v[c];
				}
			}
		}

		bool EvaluateWater(Api::MaterialPortEvaluation& a_port)
		{
			if (a_port.Port() != kMaterialKindWater || a_port.Size() < Port::kMaterialBytes)
				return false;
			a_port.ResetRecord(kMaterialKindWater);
			const auto bytes = a_port.At<std::array<std::byte, Port::kMaterialBytes>>(0);
			const Port::MaterialRow rows = Port::EvaluateMaterial(Port::MaterialBytes(bytes), NeutralFrame());
			WriteRows<Port::VsVariable>(rows.vs, kVsVariables, VsLayout(), a_port.VSFloats());
			WriteRows<Port::PsVariable>(rows.ps, kPsVariables, PsLayout(), a_port.PSFloats());

			// SetupMaterial's textures. t1, t2, t7 and t8 are frame textures, t3's global cube is the frame's.
			const std::uint32_t pass = a_port.passDescriptor;
			if (!a_port.At<std::uint8_t>(kGlobalCubeByte) && a_port.HasTexture(kStaticReflection)) {
				a_port.BindTexture(3, a_port.View(kStaticReflection), 0);
				a_port.SetFilterMode(3, kBilinear);
			}
			const auto bindNormal = [&](std::uint32_t a_slot, std::uint16_t a_field) {
				if (a_port.HasTexture(a_field))
					a_port.BindTexture(a_slot, a_port.View(a_field), kWrap);
				a_port.SetFilterMode(a_slot, kAnisotropic);
			};
			const bool flowmap = (pass & Port::Flag::kFlowmap) != 0;
			if (flowmap) {
				bindNormal(9, kFlowNormals);
				bindNormal(4, kNormal1);
			}
			// A flowmap without BlendNormals binds only t4 for the surface, underwater and LOD techniques.
			const std::uint32_t technique = Port::TechniqueOf(pass);
			const bool onlyFirst = flowmap && !(pass & Port::Flag::kBlendNormals) && (technique == 0 || technique >= 8);
			if (!onlyFirst) {
				bindNormal(4, kNormal1);
				bindNormal(5, kNormal2);
				bindNormal(6, kNormal3);
			}
			return true;
		}

		void SampleWaterShading(Api::MaterialPortShading& a_shading)
		{
			// BSWaterShader writes no MaterialData or EmitColor. The material pool row is a shading field, which samples leave alone.
			auto& row = a_shading.out;
			row.shading = {};
			row.written = 0;
			row.padding = 0u;
			std::fill(std::begin(row.featureBlock), std::end(row.featureBlock), 0.0f);
		}
	}

	Classification ClassifyLeaf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
	{
		Port::RenderPasses passes;
		return Classify(a_property, a_leaf, passes);
	}

	void RegisterPort(Api::InstallContext& a_context)
	{
		lightLimitFix = a_context.NeedClassifyInput("light limit fix");
		const auto& vs = VsSpace();
		const auto& ps = PsSpace();
		static const Api::MaterialPortContribution port{
			.name = "water",
			.shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Water),
			.objectClass = Api::ObjectClass::Water,
			.propertyRtti = []() -> const void* { return REL::Relocation<const RE::NiRTTI*>{ RE::BSWaterShaderProperty::Ni_RTTI }.get(); },
			.materialVtable = []() -> const void* { return reinterpret_cast<const void*>(RE::VTABLE_BSWaterShaderMaterial[0].address()); },
			.materialBytes = static_cast<std::uint32_t>(Port::kMaterialBytes),
			.textureFields = kTextureFields,
			.vs = { .variables = kVsVariables, .sized = vs.sized, .groups = { vs.groups[0], vs.groups[1], vs.groups[2] },
				.first = { vs.first[0], vs.first[1], vs.first[2] }, .names = vs.names },
			.ps = { .variables = kPsVariables, .sized = ps.sized, .groups = { ps.groups[0], ps.groups[1], ps.groups[2] },
				.first = { ps.first[0], ps.first[1], ps.first[2] }, .names = ps.names },
			.rules = { .passEnumBase = Port::kSurfacePassBase, .techniqueRows = false, .rawPermutation = true, .signatureComponents = false },
			.capture = &CaptureWater,
			.derive = &DeriveWater,
			.evaluate = &EvaluateWater,
			.shading = &SampleWaterShading,
			.variants = &WaterVariants,
			.variantNames = kVariantNames,
		};
		if (a_context.RegisterMaterialPort(port) != kMaterialKindWater)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the water port did not take material kind 2 (Effects' port must register first, and no other before it)");
	}
}
