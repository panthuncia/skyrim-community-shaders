// The eligibility rules that need more than a header. Pure functions of the records passed in, called on the scene lane and the render thread.

#include "Features/DrawcallLimitFix/Core/Scene/Model/Eligibility.h"

#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <mutex>
#include <unordered_set>

namespace DCLF::Eligibility
{
	bool Cacheable(Ineligible a_reason)
	{
		// Only verdicts from the geometry's own shape are cacheable, since the pointer witnesses cover exactly those.
		// Alpha and property flags can change behind an unchanged pointer. Community Shaders' features set flags in place.
		switch (a_reason) {
		case Ineligible::NotTriShape:
		case Ineligible::Skinned:
		case Ineligible::NoRendererData:
		case Ineligible::NotLightingShader:
			return true;
		default:
			return false;
		}
	}

	namespace
	{
		/** @brief None when the buffers are drawable. */
		Ineligible BufferVerdict(const SceneCapture::GeometryRecord& a_geometry)
		{
			const auto& g = a_geometry;
			if (g.skin) {
				if (!Extensions::ObjectClassOn(Api::ObjectClass::Skinned))
					return Ineligible::Skinned;
				// For a skinned shape each drawn partition is one draw of that partition's buffer, not rendererData, which is a different TriShape.
				// The native shader can index at most 240 palette rows.
				static const REL::Relocation<const RE::NiRTTI*> niSkinInstance{ RE::NiSkinInstance::Ni_RTTI };
				static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
				const bool dismember = g.skinRtti == dismemberSkinInstance.get();
				if (g.skinRtti != niSkinInstance.get() && !dismember)
					return Ineligible::SkinShape;
				const auto partitions = static_cast<std::uint32_t>(g.partitions.size());
				if (!g.skinPartition || !g.skinData || partitions == 0 || !SkinPalettes::Fits(g.boneCount * SkinPalettes::kRowsPerBone))
					return Ineligible::SkinShape;
				if ((partitions > 1 || dismember) && !Extensions::ObjectClassOn(Api::ObjectClass::SkinPartitions))
					return Ineligible::SkinShape;
				if (partitions > kMaxSkinPartitions)
					return Ineligible::SkinShape;
				// The engine indexes the dismember flags by partition, so a flag array of another length is not one it could have drawn from.
				if (dismember && !g.shown.empty() && g.shown.size() != partitions)
					return Ineligible::SkinShape;
				// Every partition draws with the first one's pipeline, so they must share its vertex layout. A LOD byte past 2 would index
				// the next row of the engine's table.
				const auto& first = g.partitions[0];
				for (const auto& p : g.partitions) {
					if (!p.vertexBuffer || !p.indexBuffer)
						return Ineligible::NoRendererData;
					if (p.vertexDesc != first.vertexDesc || p.lodByte > 2)
						return Ineligible::SkinShape;
				}
			} else if (!g.rendererData || !g.vertexBuffer || !g.indexBuffer) {
				return Ineligible::NoRendererData;
			}
			return Ineligible::None;
		}

		/** @brief Logs a refused skin once per geometry, a few times a session. The engine keeps such a shape, so its shadow is the engine's alone. */
		void NoteSkinShape(const SceneCapture::LeafView& a_leaf)
		{
			constexpr std::size_t kMax = 64;
			static std::mutex mutex;
			static std::unordered_set<const void*> seen;
			const auto& g = *a_leaf.geometry;
			{
				std::lock_guard lock(mutex);
				if (seen.size() >= kMax || !seen.insert(g.key).second)
					return;
			}
			std::unordered_set<std::uint64_t> layouts;
			std::uint32_t lod = 0;
			for (const auto& p : g.partitions) {
				layouts.insert(p.vertexDesc);
				lod = std::max<std::uint32_t>(lod, p.lodByte);
			}
			static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
			logger::info("[DCLF] skin shape left to the engine: '{}' under '{}': {} bones ({} palette rows fit: {}), {} partitions (max {}), {} vertex layouts, LOD byte "
						 "max {}, {} instance, {} shown flags",
				a_leaf.node->name ? a_leaf.node->name : "", a_leaf.parent && a_leaf.parent->name ? a_leaf.parent->name : "", g.boneCount,
				g.boneCount * SkinPalettes::kRowsPerBone, SkinPalettes::Fits(g.boneCount * SkinPalettes::kRowsPerBone), g.partitions.size(), kMaxSkinPartitions,
				layouts.size(), lod, g.skinRtti == dismemberSkinInstance.get() ? "dismember" : "other", g.shown.size());
		}
	}

	Ineligible Static(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated)
	{
		if (!a_leaf)
			return Ineligible::NotTriShape;
		const auto& g = *a_leaf.geometry;
		// A taken entry class such as a face shape gets the same checks. Its positions come from its component, bound as the second stream.
		const auto type = static_cast<RE::BSGeometry::Type>(g.type);
		const auto entryClass = a_leaf.parent ? Extensions::EntryClassOf(g.type, a_leaf.parent->kind) : Api::EntryClass::None;
		const bool entryShape = entryClass != Api::EntryClass::None && Extensions::EntryClassTaken(entryClass);
		// A BSMultiIndexTriShape draws every pass as a tri-shape except its main-pass layer (hint 12), which draws from the second index list
		// as DCLF's layer object. Shadow modes register only the main property's passes (FUN_1414b2a60), so it casts as a tri-shape.
		const bool multiIndex = type == RE::BSGeometry::Type::kMultiIndexTriShape;
		// A geometry type a contribution takes by its property flags, such as object LOD's BSSubIndexTriShape drawn by visible segment runs.
		bool flaggedShape = false;
		if (const auto& flagged = Extensions::GeometryTypeContributionOf(g.type); flagged.propertyFlags) {
			const auto* flaggedProperty = SceneCapture::LeafView::Lighting(a_leaf.property);
			if (flaggedProperty && (flaggedProperty->flags & flagged.propertyFlags)) {
				if (!flagged.ported)
					return Ineligible::Lod;
				flaggedShape = true;
			}
		}
		if (type != RE::BSGeometry::Type::kTriShape && !multiIndex && !flaggedShape) {
			// A particle geometry with a material port's property is that port's to classify. It has no renderer data, its vertices come from ParticleSnapshots.
			if (ParticleGeometryType(g.type) && Extensions::GeometryTypeOn(g.type)) {
				const auto* port = SceneCapture::LeafView::Port(a_leaf.property);
				if (!port || !Extensions::PortTakesParticles(port->port))
					return Ineligible::NotLightingShader;
				LightingDescriptors descriptors;
				const Ineligible reason = Extensions::DerivePort(port->port, *port, a_leaf, descriptors);
				if (a_descriptors && reason == Ineligible::None)
					*a_descriptors = descriptors;
				return reason;
			}
			if (!entryShape)
				return Ineligible::NotTriShape;
		}

		if (const Ineligible buffers = BufferVerdict(g); buffers != Ineligible::None) {
			if (buffers == Ineligible::SkinShape)
				NoteSkinShape(a_leaf);
			return buffers;
		}

		const auto* property = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!property) {
			const auto* port = SceneCapture::LeafView::Port(a_leaf.property);
			if (!port || !Extensions::PortOn(port->port))
				return Ineligible::NotLightingShader;
			if (type != RE::BSGeometry::Type::kTriShape)
				return Ineligible::EffectVariant;
			LightingDescriptors descriptors;
			const Ineligible reason = Extensions::DerivePort(port->port, *port, a_leaf, descriptors);
			if (a_descriptors && reason == Ineligible::None)
				*a_descriptors = descriptors;
			return reason;
		}

		// GetRenderPasses treats material alpha below one as transparent.
		if (property->materialAlpha < 1.0f && !Extensions::ObjectClassOn(Api::ObjectClass::Forward))
			return Ineligible::AlphaBlend;

		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*property, a_leaf, a_accumulated, descriptors);
		if (a_descriptors) {
			if (reason == Ineligible::None) {
				*a_descriptors = descriptors;
			} else {
				// rejectedTechnique's default 0 is a supported technique, so it must be copied on rejection too.
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
			}
		}
		if (!multiIndex || reason != Ineligible::None)
			return reason;
		if (!g.layerProperty)
			return Ineligible::None;  // nothing but its own passes: a tri-shape
		return Extensions::ObjectClassOn(Api::ObjectClass::Layer) && Layer(a_leaf, nullptr, nullptr) == Ineligible::None ? Ineligible::None : Ineligible::MultiIndex;
	}

	Ineligible Layer(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated)
	{
		// The layer's passes are GetRenderPasses of the additional property (FUN_1414b2330), with the geometry's alpha and skin.
		const auto* layer = SceneCapture::LeafView::Lighting(a_leaf.layer);
		if (!layer)
			return Ineligible::NotLightingShader;
		if (a_leaf.Skinned())
			return Ineligible::Skinned;  // a skinned layer is not modelled
		if (layer->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;
		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*layer, a_leaf, a_accumulated, descriptors, true);
		if (a_descriptors) {
			if (reason == Ineligible::None)
				*a_descriptors = descriptors;
			else
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
		}
		return reason;
	}

	bool Primary(const EntryVerdict& a_entry, const SceneCapture::LeafView& a_leaf)
	{
		if (!a_entry.record || !a_entry.classified || a_entry.reason != Ineligible::None || !a_leaf.property)
			return false;
		const bool decal = (a_leaf.property->flags & 0xc000000ull) != 0;
		if (decal && !Extensions::ObjectClassOn(Api::ObjectClass::Decal))
			return false;
		if (Extensions::ObjectClassOn(Api::ObjectClass::Forward))
			return true;
		return decal || !a_leaf.AlphaBlending();
	}
}
