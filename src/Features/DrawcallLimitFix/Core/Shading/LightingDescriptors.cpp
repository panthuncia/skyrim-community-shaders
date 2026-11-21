// LightingDescriptors.h's ports: technique selection, pass and shader descriptors, shadow bits, LOD fades and particle acceptance.
// Pure functions of the records and FrameGlobals. The Sample* functions run on the render thread.

#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

#include "ShaderCache.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "State.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

namespace DCLF
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using LightingFlag = SIE::ShaderCache::LightingShaderFlags;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }
		constexpr std::uint32_t Bit(LightingFlag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		// A contributed technique is DCLF's once a feature took it.
		bool ContributedTechniqueOn(std::uint32_t a_technique)
		{
			static const auto& techniques = Extensions::RegisteredTechniques();
			return Extensions::TechniqueOn(techniques, a_technique);
		}

		// Techniques DCLF supports. The rest stay native.
		bool IsSupportedTechnique(std::uint32_t a_technique)
		{
			switch (static_cast<Technique>(a_technique)) {
			case Technique::None:
			case Technique::Envmap:
			case Technique::Glowmap:
			case Technique::Parallax:
			// SetupMaterial writes their constants and textures, which the material records take from the engine's evaluation.
			case Technique::ParallaxOcc:
			case Technique::MultilayerParallax:
				return true;
			// A multi-index shape's ice or snow layer. SparkleParams comes from SetupMaterial, and SetupGeometry's projected textures replace slot 10.
			case Technique::MultiIndexSparkle:
				return Extensions::ObjectClassOn(Api::ObjectClass::Layer);
			// Techniques the features contribute, such as actor skin, TreeAnim, MTLand and the LOD techniques, each only for its flags' geometry type.
			default:
				return ContributedTechniqueOn(a_technique);
			}
		}

		// FUN_14147c470: false when the feature has faded out, else the fade factor. a_metric is the fade node's current LOD metric at +0x144.
		// The previous one at +0x148 only feeds the engine's render-pass cache invalidation.
		bool LodFadeVisible(float a_metric, float a_start, float a_end, float& a_fade)
		{
			if (a_end < a_metric)
				return false;
			if (a_metric <= a_start) {
				a_fade = 1.0f;
			} else {
				a_fade = std::clamp((a_metric - a_end) / (a_start - a_end), 0.0f, 1.0f);
			}
			return true;
		}

		// BSLightingShader::SetupTechnique splits the pass descriptor.
		constexpr std::uint32_t VertexDescriptorFromPass(std::uint32_t d)
		{
			return (d & 0x48007u) | ((d & 0x20a00u) ? 0x200u : 0u) | (d & 0x3f000000u);
		}

		// The pixel split drops bits 3-8 except what the pass rules keep. TruePBR keeps bits 3-5, so only the shadow light count is dropped.
		std::uint32_t PixelDescriptorFromPass(std::uint32_t d)
		{
			std::uint32_t p = d & ~Extensions::LightingPixelDropped();
			if ((p & 0x4u) == 0)  // Skinned is kept only with ModelSpaceNormals
				p &= ~0x2u;
			return p | 1u;  // VC is always set
		}
	}

	std::uint8_t FadeStateOf(const SceneCapture::PropertyRecord* a_property)
	{
		if (!a_property)
			return 0;
		const std::uint64_t f = a_property->flags;
		if (!(f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
			return 0;
		return a_property->fadeNode ? 0 : kFadeNoNode;
	}

	RE::BSShaderProperty* LayerPropertyOf(const RE::BSGeometry& a_geometry)
	{
		if (const_cast<RE::BSGeometry&>(a_geometry).GetType().get() != RE::BSGeometry::Type::kMultiIndexTriShape)
			return nullptr;
		const auto& data = static_cast<const RE::BSMultiIndexTriShape&>(a_geometry).GetMultiIndexTrishapeRuntimeData();
		if (!data.altIndexBuffer || !*reinterpret_cast<ID3D11Buffer* const*>(data.altIndexBuffer) || data.altPrimCount == 0)
			return nullptr;
		return data.additionalShaderProperty.get();
	}

	std::uint32_t StaticShadowBits(const SceneCapture::LeafView& a_leaf, bool a_settled, const SceneCapture::PropertyRecord* a_property)
	{
		const auto& g = FrameGlobals::Current();
		const auto* lighting = SceneCapture::LeafView::Lighting(a_property ? a_property : a_leaf.property);
		if (!lighting || !g.accumulator)
			return 0;
		const std::uint64_t flags = lighting->flags;
		const auto* fadeNode = lighting->fadeNode ? a_leaf.FadeNodeOf(lighting) : nullptr;
		// a_settled means fully faded in whatever the node holds, because a member's fade is the GPU's.
		const float fade = fadeNode && !a_settled ? fadeNode->currentFade : 1.0f;
		const float alpha = lighting->materialAlpha * fade;  // 1 without a material
		const bool blended = a_leaf.AlphaBlending();
		// GetRenderPasses: local_164, local_167, local_165/local_168 and local_158.
		const bool translucent = alpha < 1.0f || blended;
		const bool screenDoor = g.screenDoorFades && fadeNode && fadeNode->fadeDoor && fade < 1.0f &&
		                        !(flags & (1ull << 19));
		constexpr std::uint64_t kOpaqueClasses = 0x800c000000ull;
		const bool eligible = !translucent || (screenDoor && !blended) || (flags & kOpaqueClasses);
		bool deferred = g.accumulatorDeferredShadow != 0;
		if (!screenDoor || blended)
			deferred = deferred && !((alpha < 1.0f || blended || (flags & (1ull << 33))) && !(flags & kOpaqueClasses));
		bool shadowDir = deferred && eligible && !g.noSunShadowDir;
		bool defShadow = deferred;
		if (!(flags & 0x800c000100ull) && !lighting->shadowPasses)
			shadowDir = defShadow = false;
		return (shadowDir ? 0x2000u : 0u) | (defShadow ? 0x4000u : 0u) | (defShadow && !shadowDir ? 0x40u : 0u);
	}

	bool BlendFading(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_property);
		if (!lighting)
			return false;
		const std::uint64_t flags = lighting->flags;
		// Decals (hints 2 and 3), refraction (hint 4) and bit 19 (hint 10, the stencil dither) take other passes.
		if (flags & (0xc000000ull | 0x8004ull | (1ull << 19)))
			return false;
		const auto* fadeNode = lighting->fadeNode ? a_leaf.FadeNodeOf(lighting) : nullptr;
		if (!fadeNode || !(fadeNode->currentFade < 1.0f))
			return false;
		// A screen-door fade without blending stays opaque (AdditionalAlphaMask).
		const bool screenDoor = FrameGlobals::Current().screenDoorFades && fadeNode->fadeDoor;
		return !screenDoor || a_leaf.AlphaBlending();
	}

	LodFadeFrame SampleLodFadeFrame()
	{
		const auto& g = FrameGlobals::Current();
		LodFadeFrame frame;
		frame.eye[0] = g.eye[0];
		frame.eye[1] = g.eye[1];
		frame.eye[2] = g.eye[2];
		frame.lodAdjust = g.lodAdjust;
		frame.specularStart = g.specularStart;
		frame.specularEnd = g.specularEnd;
		frame.envmapStart = g.envmapStart;
		frame.envmapEnd = g.envmapEnd;
		frame.metricScale = g.metricScale;
		frame.defaultScale = g.fadeDefaultScale;
		frame.metricOverride = g.fadeSpecialA;
		frame.overridden = g.fadeSpecialA != g.fadeSpecialB ? 1.0f : 0.0f;
		for (std::uint32_t type = 0; type < 16; ++type)
			frame.divisors[type] = g.fadeTypeDivisors[type];
		frame.fadesOn = g.fadesOn ? 1.0f : 0.0f;
		frame.refractionStart = g.refractionStart;
		frame.refractionEnd = g.refractionEnd;
		return frame;
	}

	std::array<float, 4> LodFadeNodeOf(const RE::BSShaderProperty* a_property)
	{
		if (!a_property || !a_property->fadeNode)
			return { 0.0f, 0.0f, 0.0f, -1.0f };
		const auto* node = a_property->fadeNode;
		// A refraction property's own fade is fade-sensitive too, unless kNoFade.
		const std::uint64_t flags = a_property->flags.underlying();
		const bool refractionFades = (flags & (Bit(Flag::kRefraction) | Bit(Flag::kTempRefraction))) && !(flags & Bit(Flag::kNoFade));
		const bool held = (!(flags & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))) && !refractionFades) ||
		                  node->GetFlags().any(RE::NiAVObject::Flag::kIgnoreFade);
		const auto& center = node->worldBound.center;
		return { center.x, center.y, center.z, static_cast<float>(Engine::At<std::uint8_t>(node, 0x153) & 0xF) + (held ? kLodFadeHeld : 0.0f) };
	}

	static_assert(kLightingPixelDeferred == static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred));

	void LightingShaderDescriptors(std::uint32_t a_pass, bool a_deferred, std::uint32_t& a_vertex, std::uint32_t& a_pixel)
	{
		uint vertex = VertexDescriptorFromPass(SetupTechniqueDescriptor(a_pass));
		uint pixel = PixelDescriptorFromPass(SetupTechniqueDescriptor(a_pass));
		// Forced, so the result does not depend on whether the deferred pass runs. The lookup only adds the bit.
		globals::state->ModifyShaderLookup(RE::BSShader::Type::Lighting, vertex, pixel, true);
		if (!a_deferred)
			pixel &= ~kLightingPixelDeferred;
		a_vertex = vertex;
		a_pixel = pixel;
	}

	std::uint32_t SetupTechniqueDescriptor(std::uint32_t a_pass)
	{
		const std::uint32_t technique = a_pass & 0x3f000000u;
		// The engine's bytes at 0x2032fdb and 0x2035500, as the frame captured them.
		const auto& g = FrameGlobals::Current();
		if (technique == 0x12000000u && !g.techniqueByte12)
			return (a_pass & 0xc9ffffffu) | 0x9000000u;
		if (technique == 0x7000000u && !g.techniqueByte7)
			return a_pass & 0xc0ffffffu;
		return a_pass;
	}

	std::uint32_t SelectLightingTechnique(std::uint64_t f)
	{
		// Same order as BSLightingShaderProperty::GetRenderPasses; later tests win.
		std::uint32_t t = (f & Bit(Flag::kEnvMap)) ? 1u : 0u;
		if (f & Bit(Flag::kGlowMap))
			t = 2;
		if ((f & (Bit(Flag::kParallax) | Bit(Flag::kParallaxOcclusion))) == Bit(Flag::kParallax))
			t = 3;
		if (f & Bit(Flag::kFace))
			t = 4;
		if (f & Bit(Flag::kFaceGenRGBTint))
			t = 5;
		if (f & Bit(Flag::kHairTint))
			t = 6;
		if (f & Bit(Flag::kParallaxOcclusion))
			t = 7;
		if (f & Bit(Flag::kMultiTextureLandscape))
			t = 8;
		if (f & Bit(Flag::kNoLODLandBlend))
			t = 19;
		if (f & Bit(Flag::kLODLandscape))
			t = 18;
		if (f & Bit(Flag::kLODObjects))
			t = 13;
		if (f & Bit(Flag::kHDLODObjects))
			t = 15;
		if (f & Bit(Flag::kMultiLayerParallax))
			t = 11;
		if (f & Bit(Flag::kTreeAnim))
			t = 12;
		if ((f & (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV))) == (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV)))
			t = 14;
		if (f & Bit(Flag::kEyeReflect))
			t = 16;
		return t;
	}

	std::uint32_t AlphaBlendModeOf(const SceneCapture::AlphaRecord& a_alpha)
	{
		// NiAlphaProperty::GetSrcBlendMode, GetDestBlendMode: the flags' bits 1-4 and 5-8.
		using Function = RE::NiAlphaProperty::AlphaFunction;
		const auto source = static_cast<Function>((a_alpha.flags >> 1) & 0xF);
		const auto dest = static_cast<Function>((a_alpha.flags >> 5) & 0xF);
		if (source == Function::kSrcAlpha && dest == Function::kInvSrcAlpha)
			return 1;
		if ((source == Function::kSrcAlpha && dest == Function::kOne) || (source == Function::kOne && dest == Function::kOne) ||
			(source == Function::kSrcAlpha && dest == Function::kInvDestAlpha))
			return 2;
		if ((source == Function::kZero && dest == Function::kSrcColor) || (source == Function::kDestColor && dest == Function::kZero))
			return 4;
		if (source == Function::kDestColor && dest == Function::kInvSrcAlpha)
			return 3;
		return 0;
	}

	namespace
	{
		// DeriveLightingDescriptors' stages, in order. Each returns why the object stays native, or None to go on.

		// The decal and layer groups.
		Ineligible DeriveDecalGroup(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, const AccumulatedPass* a_accumulated,
			LightingDescriptors& a_out, bool a_layer, std::uint64_t f, bool blending)
		{
			const auto* alpha = a_leaf.alpha;
			if (a_layer) {
				// A multi-index shape's layer: every pass goes in geometry group 2 with hint 12, and FUN_1414b3bb0 draws it after the opaque decals
				// with their state. That is DCLF's decal group 3, between opaque and blended.
				if (!Extensions::ObjectClassOn(Api::ObjectClass::Decal))
					return Ineligible::Decal;
				a_out.decalGroup = 3;
				a_out.decalBlendMode = 0;
				a_out.decalWriteMode = 10;
			} else if (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) {
				// Decals. Hint 2 is depth test and write, opaque bias, no blending, alpha property not applied.
				// Hint 3 is depth test only, blended bias, alpha property applied, write mode 1 with kZBufferWrite or else 11.
				// Neither occludes more than its host, so DCLF draws them in a second pass without depth writes.
				// Without an accumulated pass the hint is 2 + (material alpha < 1 or blending), as GetRenderPasses gives a settled object.
				if (!Extensions::ObjectClassOn(Api::ObjectClass::Decal) || !(f & Bit(Flag::kZBufferTest)))
					return Ineligible::Decal;
				const std::uint32_t hint = a_accumulated ? a_accumulated->hint : (a_property.materialAlpha < 1.0f || blending) ? 3u : 2u;
				if (hint == 2) {
					if (blending)
						return Ineligible::Decal;  // not measured in this group, so the state would be a guess
					a_out.decalGroup = 1;
					a_out.decalBlendMode = 0;
					a_out.decalWriteMode = 10;
				} else if (hint == 3) {
					// The fading case, alpha < 1 without blending, stays native.
					if (!blending)
						return Ineligible::Decal;
					const std::uint32_t blendMode = AlphaBlendModeOf(*alpha);
					if (blendMode == 0)
						return Ineligible::Decal;  // a blend pair the engine's setup leaves as it finds it
					a_out.decalGroup = 2;
					a_out.decalBlendMode = blendMode;
					a_out.decalWriteMode = (f & Bit(Flag::kZBufferWrite)) ? 1u : 11u;
				} else {
					return Ineligible::Decal;
				}
			}
			return Ineligible::None;
		}

		// The held group the object joins.
		Ineligible DeriveHeldGroup(LightingDescriptors& a_out, std::uint64_t f)
		{
			// Held groups a CS feature replays after the opaque pass, as Terrain Blending does. DCLF draws them in terrain groups 1 and 2,
			// after its opaque and decal groups. The first matching group takes the object.
			for (const auto& group : Extensions::HeldGroups()) {
				if (!(f & group.propertyFlags) || !Extensions::ClassifyInputOn(group.heldWhile))
					continue;
				if (a_out.decalGroup)
					return Ineligible::Held;
				a_out.terrainGroup = group.group;
				break;
			}
			return Ineligible::None;
		}

		// Refraction and the forward epoch's blended draws.
		Ineligible DeriveForward(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, const AccumulatedPass* a_accumulated,
			LightingDescriptors& a_out, bool a_layer, std::uint64_t f, bool blending)
		{
			const auto* alpha = a_leaf.alpha;
			// Refraction: one Utility RENDER_NORMAL pass (hint 4) drawn into the refraction normals after the world, none past its LOD fade's end.
			// The forward epoch draws it, fading as GetRenderPasses would. A material alpha of 0 registers nothing, so it stays native and undrawn.
			if (f & (Bit(Flag::kRefraction) | Bit(Flag::kTempRefraction))) {
				if (!Extensions::ObjectClassOn(Api::ObjectClass::Refraction) || !Extensions::ObjectClassOn(Api::ObjectClass::Forward) ||
				    a_out.decalGroup || a_layer || a_out.terrainGroup || a_property.materialAlpha <= 0.0f) {
					a_out.rejectedTechnique = kRefractionReject;
					return Ineligible::Technique;
				}
				a_out.refraction = true;
				a_out.refractionFalloff = (f & Bit(Flag::kRefractionFalloff)) != 0;
				a_out.refractionClamp = (f & Bit(Flag::kNonProjectiveShadows)) != 0;
				// The draw's state per FUN_1414b4860: depth tested and not written, no blending.
				a_out.forward = true;
				a_out.forwardBlendMode = 0;
				a_out.forwardDepthTest = true;
				a_out.forwardDepthWrite = false;
			}

			// Blended geometry draws after the deferred composite, as the engine's blended draw (FUN_1414b39d0) does, with write mode 11.
			// A blend pair FUN_14150bc80 leaves stale stays native, as does anything a held group takes.
			// A fading pass (hint 9) draws the same way. The engine tests depth but never writes it, so faded actors rely on draw order to hide
			// their own back parts. No fixed order hides both hair behind a head and a mouth inside it, so a fading part writes depth as its opaque pass would.
			const bool fading = a_accumulated && a_accumulated->hint == kFadingHint && !a_layer;
			const bool translucent = blending || a_property.materialAlpha < 1.0f || fading;
			if (a_out.refraction) {
				// The refraction draw's forward state. Alpha property not applied.
			} else if (translucent && a_out.decalGroup == 0 && !a_layer && a_out.terrainGroup == 0 &&
			           Extensions::ObjectClassOn(Api::ObjectClass::Forward)) {
				// SetupAlphaBlend (FUN_14150bc80) uses source-alpha blending for material fades even without NiAlphaProperty blending.
				// Mode 0 would make hidden outfit meshes with alpha 0 draw opaque.
				const std::uint32_t blendMode = blending ? AlphaBlendModeOf(*alpha) : 1u;
				if (blending && blendMode == 0)
					return Ineligible::AlphaBlend;
				a_out.forward = true;
				a_out.forwardBlendMode = blendMode;
				a_out.forwardDepthTest = (f & Bit(Flag::kZBufferTest)) != 0;
				a_out.forwardDepthWrite = (f & Bit(Flag::kZBufferWrite)) != 0;
			} else if (blending && a_out.decalGroup != 2 && !a_layer) {
				return Ineligible::AlphaBlend;
			}
			return Ineligible::None;
		}

		// The descriptor as GetRenderPasses builds it, or why the object stays native.
		Ineligible DeriveRenderPassDescriptor(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out,
			std::uint64_t f, std::uint32_t& a_derived)
		{
			// Without a fade node GetRenderPasses never computes the LOD fades, so the fields the draw reads may be stale. Stay native.
			if (!a_property.fadeNode && (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
				return Ineligible::Fading;

			const std::uint32_t technique = SelectLightingTechnique(f);
			if (!IsSupportedTechnique(technique)) {
				a_out.rejectedTechnique = technique;
				return Ineligible::Technique;
			}

			std::uint32_t d = technique << 24;
			if (f & Bit(Flag::kVertexColors))
				d |= Bit(LightingFlag::VC);
			// GetRenderPasses copies kSkinned into bit 1 even without a skin. Static fish and buckets carry it.
			if (f & Bit(Flag::kSkinned))
				d |= Bit(LightingFlag::Skinned);
			// ProjectedUV is kProjectedUV itself. The snow conditions only add bits 19 and 21 on top, which are not derived.
			if (f & Bit(Flag::kProjectedUV))
				d |= Bit(LightingFlag::ProjectedUV);
			if (f & Bit(Flag::kModelSpaceNormals))
				d |= Bit(LightingFlag::ModelSpaceNormals);
			if (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow)))
				d |= Bit(LightingFlag::Specular);
			if (f & Bit(Flag::kSoftLighting))
				d |= Bit(LightingFlag::SoftLighting);
			if (f & Bit(Flag::kRimLighting))
				d |= Bit(LightingFlag::RimLighting);
			if (f & Bit(Flag::kBackLighting))
				d |= Bit(LightingFlag::BackLighting);
			if (f & Bit(Flag::kAnisotropicLighting))
				d |= Bit(LightingFlag::AnisoLighting);
			// GetRenderPasses (1414adfb0) gives every decal without kMultiIndexSnow DoAlphaTest, whatever its alpha property.
			if (a_leaf.AlphaTesting() ||
				((f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) && !(f & Bit(Flag::kMultiIndexSnow))))
				d |= Bit(LightingFlag::DoAlphaTest);
			if (f & Bit(Flag::kCharacterLighting))
				d |= Bit(LightingFlag::CharacterLight);

			// The pass rules' GetRenderPasses hooks, such as TruePBR swapping Specular for TruePbr.
			const Api::LightingJoinView view{ a_property, a_leaf, a_leaf.geometry ? a_leaf.geometry->extraValue : SceneCapture::GeometryRecord::kNoExtra,
				a_leaf.node ? a_leaf.node->facts : Extensions::ReferenceFactsNone(), a_property.contributorBits };
			d = Extensions::DeriveLightingPass(d, view);
			a_derived = d;
			return Ineligible::None;
		}
	}

	Ineligible DeriveLightingDescriptors(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_layer)
	{
		std::uint64_t f = a_property.flags;

		// The techniques' property flag rules, such as terrain and object LOD. A flagged object counts only with its geometry type.
		for (const auto& rule : Extensions::PropertyFlagRules())
			if ((f & rule.propertyFlags) && a_leaf.Type() != rule.geometryType)
				return Ineligible::Lod;
		const bool blending = a_leaf.AlphaBlending();
		if (const Ineligible reason = DeriveDecalGroup(a_property, a_leaf, a_accumulated, a_out, a_layer, f, blending); reason != Ineligible::None)
			return reason;
		// kSkinned selects the SKINNED permutation, whose palette comes from the skin instance. Without one, stay native.
		if ((f & Bit(Flag::kSkinned)) && !(Extensions::ObjectClassOn(Api::ObjectClass::Skinned) && a_leaf.Skinned()))
			return Ineligible::Skinned;
		if (const Ineligible reason = DeriveHeldGroup(a_out, f); reason != Ineligible::None)
			return reason;
		if (const Ineligible reason = DeriveForward(a_property, a_leaf, a_accumulated, a_out, a_layer, f, blending); reason != Ineligible::None)
			return reason;

		// The descriptor for an opaque object whose LOD fades have not run out. The draw does the fades, so this is the same at every distance.
		std::uint32_t derived = 0;
		const Ineligible derivedReason = DeriveRenderPassDescriptor(a_property, a_leaf, a_out, f, derived);

		if (derivedReason != Ineligible::None)
			return derivedReason;
		a_out.derivedPass = derived;
		std::uint32_t d = derived;
		if (a_accumulated) {
			// A registered pass gives only kRegisteredPassBits. The rest is the derivation's, and the shadow bits StaticShadowBits'.
			// A layer's pass has no shadow bits, because the registrations write light masks on the main property alone (FUN_1414b2140).
			d = (derived & ~kRegisteredPassBits) | (a_accumulated->technique & kRegisteredPassBits) | (a_layer ? 0u : StaticShadowBits(a_leaf, false, &a_property));
			// Screen-door fade: Lighting.hlsl discards against a 4x4 pattern, so the object stays opaque and the Z-prepass dithers identically.
		}

		a_out.rawVertex = VertexDescriptorFromPass(SetupTechniqueDescriptor(d));
		a_out.rawPixel = PixelDescriptorFromPass(SetupTechniqueDescriptor(d));
		std::uint32_t vertex = 0, pixel = 0;
		// Forward epoch draws are forward-shaded, with no Deferred bit, like the engine's blended draws after the composite.
		LightingShaderDescriptors(d, !a_out.forward, vertex, pixel);

		a_out.technique = (d >> 24) & 0x3f;
		a_out.pass = d;
		a_out.projectedUV = (d & Bit(LightingFlag::ProjectedUV)) != 0;
		a_out.vertex = vertex;
		a_out.pixel = pixel;
		return Ineligible::None;
	}

	Api::FrameValueItemClass PaletteItemClassOf(const SceneCapture::LeafView& a_leaf)
	{
		if (!a_leaf.geometry || a_leaf.Skinned())
			return Api::FrameValueItemClass::None;
		const auto itemClass = Extensions::GeometryItemClassOn(a_leaf.Type());
		if (itemClass == Api::FrameValueItemClass::None)
			return itemClass;
		const auto& p = a_leaf.geometry->particles;
		// Aligned quads with the velocities FUN_140e51320 reads for them.
		return p.system && p.data && (!p.aligned || p.velocities) && p.capacity > 0 ? itemClass : Api::FrameValueItemClass::None;
	}
}
