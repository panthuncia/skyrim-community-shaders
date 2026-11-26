// The caster verdicts and the frame's shadow view list.

#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

#include "RE/B/BSShadowLight.h"

namespace DCLF
{
	namespace
	{
		// Address Library ids.
		constexpr Engine::Offset kShadowFreeMask{ { 391715 }, { 513748 } };  // RenderShadowmap's free target slices
		constexpr Engine::Offset kVolumetricCopy{ { 391108 }, { 513334 } };  // 2 when the cascades take a volumetric lighting copy
		constexpr Engine::Offset kFocusTargetBase{ { 255253 }, { 305513 } };  // the focus views' first slice in target 4

		constexpr std::uint64_t Bit(std::uint32_t a_bit)
		{
			return std::uint64_t(1) << a_bit;
		}

		/** @brief The global GetRenderPasses_ShadowMapOrMask tests when kCastShadows is clear (0x142033498), as the frame captured it. */
		std::uint8_t ShadowGlobal()
		{
			return FrameGlobals::Current().shadowGlobal;
		}
	}

	const char* ShadowRejectName(ShadowReject a_reason)
	{
		constexpr const char* kNames[] = { "eligible", "not-lighting", "decl-0", "faded", "refraction", "alpha-blended", "decal-no-zwrite", "no-cast-shadows", "volumetric-only", "decal-point-light", "layer", "lod" };
		static_assert(std::size(kNames) == static_cast<std::size_t>(ShadowReject::Count));
		const auto index = static_cast<std::size_t>(a_reason);
		return index < std::size(kNames) ? kNames[index] : "?";
	}

	std::uint32_t UtilityShaderDecl(std::uint64_t a_flags)
	{
		// Port of BSLightingShaderProperty::DetermineUtilityShaderDecl (vtable 0x3D, AE 0x1414adf00), since the scene work calls no engine code.
		// It depends on the property's flags alone and is never 0.
		std::uint32_t decl = (a_flags & 0x1000) ? 0x2u : 0x1Au;
		if (a_flags & 0x2)
			decl |= 0x4;
		if (a_flags & 0x20'0000'0000ull)
			decl |= 0x1;
		if (a_flags & (0x4000ull | 0x4000'0000'0000ull))
			decl |= 0x20;
		if (a_flags & 0x2'0000'0000ull)
			decl |= 0x100;
		if (a_flags & 0x2'0000ull)
			decl |= 0x40;
		if (a_flags & 0x2000'0000'0000'0000ull)
			decl |= 1u << 26;
		return decl;
	}

	ShadowReject ShadowCasterReject(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!lighting || !a_leaf.geometry)
			return ShadowReject::NotLighting;
		return ShadowCasterReject(lighting->flags, a_leaf.Type(), a_leaf.AlphaBlending(), lighting->materialAlpha, ShadowGlobal());
	}

	ShadowReject ShadowCasterReject(std::uint64_t a_flags, std::uint8_t a_type, bool a_blended, float a_materialAlpha, std::uint8_t a_shadowGlobal)
	{
		if (IsLodObject(a_flags, a_type) || IsLodLand(a_flags, a_type))
			return ShadowReject::Lod;
		const bool decal = (a_flags & (Bit(26) | Bit(27))) != 0;
		const bool decalLike = decal && (a_flags & Bit(18));
		if (decal && !(decalLike && (a_flags & Bit(32)) && a_blended))
			return decalLike ? ShadowReject::DecalNoZWrite : ShadowReject::DecalPointLight;
		// The fade node's share is the GPU's, since a shadow view drops a caster while its root fades.
		if (a_materialAlpha < 1.0f)
			return ShadowReject::Faded;
		if (a_flags & 0x8004ull)
			return ShadowReject::Refraction;
		if (a_blended && !decalLike)
			return ShadowReject::AlphaBlended;
		// With kCastShadows clear, global 1 registers no pass, global 2 only the volumetric copy's, and global 0 casts as usual.
		if (!(a_flags & Bit(9))) {
			if (a_shadowGlobal == 1)
				return ShadowReject::NoCastShadows;
			if (a_shadowGlobal == 2)
				return ShadowReject::VolumetricOnly;
		}
		if (UtilityShaderDecl(a_flags) == 0)
			return ShadowReject::DeclZero;
		return ShadowReject::None;
	}

	bool CastsNoShadow(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!lighting || !a_leaf.geometry)
			return false;
		const std::uint64_t flags = lighting->flags;
		if (IsLodObject(flags, a_leaf.Type()) || IsLodLand(flags, a_leaf.Type()))
			return true;  // Lod
		const bool blended = a_leaf.AlphaBlending();
		const bool decal = (flags & (Bit(26) | Bit(27))) != 0;
		const bool decalLike = decal && (flags & Bit(18));
		if (decal && !(decalLike && (flags & Bit(32)) && blended))
			return true;  // DecalNoZWrite, DecalPointLight
		if (flags & 0x8004ull)
			return true;  // Refraction
		if (blended && !decalLike)
			return true;  // AlphaBlended
		return UtilityShaderDecl(flags) == 0;
	}

	std::uint32_t ShadowUtilityTechnique(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!lighting || !a_leaf.geometry)
			return 0;
		return ShadowUtilityTechnique(lighting->flags, a_leaf.AlphaTesting());
	}

	std::uint32_t ShadowUtilityTechnique(std::uint64_t a_flags, bool a_alphaTesting)
	{
		std::uint32_t technique = UtilityShaderDecl(a_flags);
		if (a_alphaTesting)
			technique |= 0x80;
		if (a_flags & (Bit(34) | Bit(63)))
			technique |= 0x8000000;
		if (a_flags & (Bit(26) | Bit(27)))
			technique |= 0x20080;
		return technique;
	}

	std::vector<std::uint32_t> AllShadowUtilityTechniques()
	{
		// One flag for each test in UtilityShaderDecl, ShadowUtilityTechnique and ShadowCasterReject. Where a test reads either of two
		// flags, one stands for both. Keep this list in step with those functions. kCastShadows is left out: with shadow global 0 it
		// changes nothing, and the other globals only reject.
		constexpr std::uint64_t kLodLandscape = Bit(33);
		constexpr std::uint64_t kTested[] = { 0x1000, 0x2, Bit(37), 0x4000'0000'0000ull, kLodLandscape, Bit(17), Bit(61), Bit(34), Bit(26), Bit(18), Bit(32),
			0x4, 0x8000 };
		constexpr std::uint32_t kCombinations = 1u << std::size(kTested);
		// The geometry types the LOD rules tell apart.
		constexpr std::uint8_t kTriShape = static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape);
		constexpr std::uint8_t kSubIndexTriShape = static_cast<std::uint8_t>(RE::BSGeometry::Type::kSubIndexTriShape);
		std::vector<std::uint32_t> techniques;
		for (std::uint32_t combination = 0; combination < kCombinations; ++combination) {
			std::uint64_t flags = 0;
			for (std::size_t i = 0; i < std::size(kTested); ++i)
				if (combination & (1u << i))
					flags |= kTested[i];
			for (const std::uint8_t type : { kTriShape, kSubIndexTriShape }) {
				// Only terrain LOD chunks carry the LOD landscape flag, and they are BSTriShapes. So no caster has it.
				if ((flags & kLodLandscape) && type != kTriShape)
					continue;
				for (const bool blended : { false, true }) {
					// A faded caster is rejected, so only full alpha casts. Shadow global 0 rejects nothing by itself.
					const auto reject = ShadowCasterReject(flags, type, blended, 1.0f, 0);
					if (reject != ShadowReject::None && reject != ShadowReject::VolumetricOnly)
						continue;
					for (const bool alphaTesting : { false, true })
						techniques.push_back(ShadowUtilityTechnique(flags, alphaTesting));
				}
			}
		}
		std::ranges::sort(techniques);
		const auto [first, last] = std::ranges::unique(techniques);
		techniques.erase(first, last);
		return techniques;
	}

	std::uint32_t ShadowModeBits(std::uint32_t a_renderMode)
	{
		switch (a_renderMode) {
		case 0xC: return 0x2000;  // RenderDepth, the main Z-prepass
		case 0xE: return 0xC000;
		case 0xF: return 0x14000;
		default: return 0x4000;
		}
	}

	static_assert(Extensions::kShadowViewKinds == ShadowViews::kKindCount && static_cast<std::size_t>(ShadowViews::Kind::Directional) == 0 &&
				  static_cast<std::size_t>(ShadowViews::Kind::Frustum) == 1 && static_cast<std::size_t>(ShadowViews::Kind::Parabolic) == 2 &&
				  static_cast<std::size_t>(ShadowViews::Kind::Other) == 3);

	const char* ShadowViews::KindName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::Directional: return "dir";
		case Kind::Frustum: return "spot";
		case Kind::Parabolic: return "point";
		default: return "other";
		}
	}

	ShadowViews& ShadowViews::Get()
	{
		static ShadowViews views;
		return views;
	}

	LightSlotFrame& ShadowViews::LightSlots()
	{
		static LightSlotFrame frame;
		return frame;
	}

	void ShadowViews::Clear()
	{
		views.clear();
		captures.clear();
		batchToView.clear();
		accumulatorToView.clear();
		valid = false;
		candidates = 0;
		unrecorded = false;
	}

	void ShadowViews::BeginCasters()
	{
		// Before the first light's Accumulate, so nothing registered from here is withheld by last frame's renderers.
		Clear();
		drawable = true;
		PublishRenderers();
		PassCapture::Get().ResetCoveredKept();
	}

	void ShadowViews::AddCaster(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
	{
		// The engine just kept a_light and is about to Accumulate it, so cover its views and publish their renderers first.
		// Not before the selection has published once, since without it Rebuild lists no view.
		auto* light = const_cast<RE::BSShadowLight*>(a_light);
		if (!light || !LightSlots().published)
			return;
		const std::size_t first = views.size();
		// A spot host's focus accumulation can register before the focus cameras set the count, so list every focus descriptor with an
		// accumulator. Listing one the engine does not render withholds nothing it draws.
		auto& lightData = light->GetRuntimeData();
		std::uint32_t focusCount = std::min<std::uint32_t>(Engine::Global<std::uint32_t>(Engine::Address::kFocusTargetCount), 4u);
		if (lightData.drawFocusShadows)
			for (std::uint32_t d = focusCount; d < std::size(lightData.focusShadowmapDescriptors); ++d)
				if (lightData.focusShadowmapDescriptors[d].shaderAccumulator)
					focusCount = d + 1;
		// Already listed by AddFocusHost.
		if (std::ranges::any_of(views, [&](const View& a_view) { return a_view.light == light && a_view.focus; }))
			focusCount = 0;
		for (std::uint32_t s = 0; s < a_slots; ++s)
			AddLightViews(*light, a_firstSlot + s, s == 0 && lightData.drawFocusShadows ? focusCount : 0u);
		if (CoverFrom(first))
			PublishRenderers();
	}

	bool ShadowViews::AddFocusHost(const RE::BSShadowLight* a_light)
	{
		auto* light = const_cast<RE::BSShadowLight*>(a_light);
		if (!light || !LightSlots().published)
			return false;
		bool listed = false, covered = false;
		for (const auto& view : views)
			if (view.light == light && view.focus) {
				listed = true;
				covered = covered || view.covered;
			}
		if (listed)
			return covered;
		const std::uint32_t count = std::min<std::uint32_t>(Engine::Global<std::uint32_t>(Engine::Address::kFocusTargetCount), 4u);
		if (!count)
			return false;
		// The light's slot is not known before it is kept. Rebuild lists the views again with their slots.
		const std::size_t first = views.size();
		AddLightViews(*light, 0, count, true);
		if (CoverFrom(first))
			PublishRenderers();
		for (std::size_t i = first; i < views.size(); ++i)
			covered = covered || views[i].covered;
		return covered;
	}

	void ShadowViews::AddLightViews(RE::BSShadowLight& a_light, std::uint32_t a_lightIndex, std::uint32_t a_focusCount, bool a_focusOnly)
	{
		auto* light = &a_light;
		const std::uint32_t lightIndex = a_lightIndex;
		const Kind kind = light->GetIsDirectionalLight() ? Kind::Directional :
		                  light->GetIsFrustumLight()     ? Kind::Frustum :
		                  light->GetIsParabolicLight()   ? Kind::Parabolic :
		                                                   Kind::Other;
		auto& data = light->GetRuntimeData();
		const auto add = [&](const RE::BSShadowLight::ShadowmapDescriptor& a_descriptor, std::uint32_t a_index, bool a_focus) {
			View view;
			view.light = light;
			view.lightIndex = lightIndex;
			view.kind = kind;
			view.descriptor = a_index;
			view.focus = a_focus;
			auto* accumulator = a_descriptor.shaderAccumulator.get();
			view.accumulator = accumulator;
			auto* accumulatorData = accumulator ? accumulator->GetRuntimeData() : nullptr;
			view.batch = accumulatorData ? accumulatorData->batchRenderer : nullptr;
			view.renderMode = accumulatorData ? static_cast<std::uint32_t>(accumulatorData->renderMode) : 0u;
			view.renderTarget = static_cast<std::uint32_t>(a_descriptor.renderTarget);
			view.slice = a_descriptor.shadowmapIndex;
			static_assert(sizeof(a_descriptor.port) == sizeof(Port));
			std::memcpy(&view.port, &a_descriptor.port, sizeof(view.port));
			view.clear = a_descriptor.clearRenderTarget;
			view.enabled = a_descriptor.isEnabled;
			const auto id = static_cast<std::uint32_t>(views.size());
			if (accumulator)
				accumulatorToView.try_emplace(accumulator, id);
			if (view.batch) {
				batchToView.try_emplace(view.batch, id);
				// A geometry group sorts its passes in a batch renderer of its own, so registrations into it belong to this view too.
				for (auto* group : view.batch->geometryGroups) {
					if (group && group->batchRenderer)
						batchToView.try_emplace(group->batchRenderer, id);
				}
			}
			views.push_back(view);
		};
		for (std::uint32_t d = 0; !a_focusOnly && d < data.shadowmapDescriptors.size(); ++d)
			add(data.shadowmapDescriptors[d], d, false);
		// Render draws focus descriptors 0..count-1.
		for (std::uint32_t d = 0; d < a_focusCount; ++d)
			add(data.focusShadowmapDescriptors[d], d, true);
	}

	bool ShadowViews::CoverFrom(std::size_t a_first)
	{
		// DCLF's views are its candidates within the slots its buffers hold. On a frame whose views are not DCLF's, none is covered.
		bool covered = false;
		const auto& shadowViews = Extensions::ShadowViewsSet();
		for (std::size_t i = a_first; i < views.size(); ++i) {
			auto& view = views[i];
			const auto source = shadowViews.sourceOf[static_cast<std::size_t>(view.kind)][view.focus ? 1 : 0];
			const bool candidate = view.renderMode >= PassCapture::kFirstShadowMode &&
			                       view.renderMode < PassCapture::kFirstShadowMode + PassCapture::kShadowModes &&
			                       (source == Api::ShadowSource::Count || shadowViews.sources[static_cast<std::size_t>(source)].registered);
			const bool wouldCover = candidate && candidates < viewCapacity;
			view.covered = wouldCover && drawable;
			unrecorded = unrecorded || (wouldCover && !drawable);
			covered = covered || view.covered;
			candidates += candidate ? 1u : 0u;
		}
		return covered;
	}

	void ShadowViews::Rebuild()
	{
		Clear();
		// The lights in the order the engine renders them a moment later. If no selection ran this frame there are no views and no renderer is published.
		const auto* selection = LightSlots().Current(Scene::FrameView::Get().SceneStep());
		if (!selection) {
			PublishRenderers();
			return;
		}
		std::uint32_t lightIndex = 0;
		std::vector<const RE::BSShadowLight*> focusAdded;
		for (const auto* selected : selection->slots) {
			auto* light = const_cast<RE::BSShadowLight*>(selected);
			if (!light) {
				++lightIndex;
				continue;
			}
			const bool focusHost = std::find(selection->focusHosts.begin(), selection->focusHosts.end(), light) != selection->focusHosts.end() &&
			                       std::find(focusAdded.begin(), focusAdded.end(), light) == focusAdded.end();
			if (focusHost)
				focusAdded.push_back(light);
			AddLightViews(*light, lightIndex, focusHost ? selection->focusCount : 0u);
			++lightIndex;
		}
		PredictTargets();
		captures.assign(views.size(), 0);
		valid = true;
		(void)CoverFrom(0);
		PublishRenderers();
	}

	void ShadowViews::PublishRenderers()
	{
		// Published whole, since the hook runs on the engine's registration threads.
		auto renderers = std::make_shared<PassCapture::ShadowRendererMap>();
		// For a group renderer the engine makes during a registration, which is in no set yet.
		auto accumulators = std::make_shared<PassCapture::ShadowAccumulatorMap>();
		for (const auto& view : views)
			if (view.covered && view.accumulator && view.renderMode >= PassCapture::kFirstShadowMode)
				accumulators->emplace(view.accumulator, static_cast<std::uint8_t>(view.renderMode - PassCapture::kFirstShadowMode));
		for (const auto& [batch, id] : batchToView) {
			const auto& view = views[id];
			if (!view.covered)
				continue;
			renderers->emplace(batch, static_cast<std::uint8_t>(view.renderMode - PassCapture::kFirstShadowMode));
		}
		PassCapture::Get().SetShadowBatchRenderers(std::move(renderers));
		PassCapture::Get().SetShadowAccumulators(std::move(accumulators));
	}

	void ShadowViews::PredictTargets()
	{
		// Predict the engine's targets. Cascades go to target 2 at their index, after the volumetric copy (target 3) when 0x142033498 is 2.
		// Focus views go to target 4. A descriptor with target -1 gets target 4 and the free mask's lowest bit (RenderShadowmap 0x1414f0cf0).
		// RenderShadowmap binds only for a descriptor that clears, so a point light's hemisphere 1 draws into hemisphere 0's slice.
		using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
		const auto base = REL::Module::get().base();
		std::uint32_t freeMask = *reinterpret_cast<const std::uint32_t*>(base + kShadowFreeMask);
		const bool volumetric = *reinterpret_cast<const std::uint32_t*>(base + kVolumetricCopy) == 2;
		const std::uint32_t focusBase = *reinterpret_cast<const std::uint32_t*>(base + kFocusTargetBase);
		ankerl::unordered_dense::map<const RE::BSShadowLight*, std::uint32_t> firstTarget;
		ankerl::unordered_dense::map<const void*, std::pair<std::uint32_t, std::uint32_t>> given;
		// The target and slice the last clearing draw bound.
		bool bound = false;
		std::uint32_t boundTarget = 0, boundSlice = 0;
		// A light's views repeat per slot it takes, but the engine renders them once, at the first.
		ankerl::unordered_dense::map<const RE::BSShadowLight*, std::uint32_t> firstSlot;
		bool repeat = false;
		const auto draw = [&](View& a_view, bool a_clear) {
			if (repeat)
				return;
			if (!a_clear && bound) {
				a_view.drawTarget = boundTarget;
				a_view.drawSlice = boundSlice;
			}
			bound = true;
			boundTarget = a_view.drawTarget;
			boundSlice = a_view.drawSlice;
		};
		for (auto& view : views) {
			repeat = firstSlot.try_emplace(view.light, view.lightIndex).first->second != view.lightIndex;
			auto& data = const_cast<RE::BSShadowLight*>(view.light)->GetRuntimeData();
			if (view.focus) {
				view.drawTarget = static_cast<std::uint32_t>(T::kSHADOWMAPS);
				view.drawSlice = focusBase + view.descriptor;
				draw(view, true);
				continue;
			}
			if (view.descriptor >= data.shadowmapDescriptors.size())
				continue;
			const auto& descriptor = data.shadowmapDescriptors[view.descriptor];
			if (view.kind == Kind::Directional) {
				view.drawTarget = static_cast<std::uint32_t>(T::kSHADOWMAPS_ESRAM);
				view.drawSlice = view.descriptor;
				view.volumetricCopy = volumetric;
				draw(view, descriptor.clearRenderTarget);
				continue;
			}
			if (view.kind == Kind::Parabolic && view.descriptor == 1) {
				const auto it = firstTarget.find(view.light);
				view.drawTarget = it != firstTarget.end() ? it->second : static_cast<std::uint32_t>(descriptor.renderTarget);
				view.drawSlice = descriptor.shadowmapIndex;
				draw(view, descriptor.clearRenderTarget);
				continue;
			}
			if (const auto it = given.find(&descriptor); it != given.end()) {
				view.drawTarget = it->second.first;
				view.drawSlice = it->second.second;
			} else if (static_cast<std::int32_t>(descriptor.renderTarget) != -1) {
				view.drawTarget = static_cast<std::uint32_t>(descriptor.renderTarget);
				view.drawSlice = descriptor.shadowmapIndex;
			} else {
				std::uint32_t slice = 0;
				if (freeMask) {
					slice = static_cast<std::uint32_t>(std::countr_zero(freeMask));
					freeMask &= ~(1u << slice);
				}
				view.drawTarget = static_cast<std::uint32_t>(T::kSHADOWMAPS);
				view.drawSlice = slice;
				given.emplace(&descriptor, std::make_pair(view.drawTarget, slice));
			}
			if (view.descriptor == 0)
				firstTarget.try_emplace(view.light, view.drawTarget);
			draw(view, descriptor.clearRenderTarget);
		}
	}

	std::string ShadowViews::DescribeAccumulator(const void* a_accumulator) const
	{
		std::string listed = "not in this frame's list";
		for (std::size_t i = 0; i < views.size(); ++i)
			if (views[i].accumulator == a_accumulator) {
				const auto& view = views[i];
				listed = fmt::format("in this frame's list as view {} ({}{} descriptor {}, render mode {:#x}, {})", i, KindName(view.kind), view.focus ? " focus" : "",
					view.descriptor, view.renderMode, view.covered ? "covered" : "not covered");
				break;
			}
		std::string owner;
		const auto check = [&](const RE::BSShadowLight* a_light, const char* a_list) {
			if (!a_light || !owner.empty())
				return;
			auto* light = const_cast<RE::BSShadowLight*>(a_light);
			auto& data = light->GetRuntimeData();
			const char* kind = light->GetIsDirectionalLight() ? "directional" : light->GetIsFrustumLight() ? "spot" : light->GetIsParabolicLight() ? "point" : "other";
			for (std::uint32_t d = 0; d < data.shadowmapDescriptors.size(); ++d)
				if (data.shadowmapDescriptors[d].shaderAccumulator.get() == a_accumulator) {
					owner = fmt::format("{} {} light {} descriptor {}", a_list, kind, static_cast<const void*>(light), d);
					return;
				}
			for (std::uint32_t d = 0; d < std::size(data.focusShadowmapDescriptors); ++d)
				if (data.focusShadowmapDescriptors[d].shaderAccumulator.get() == a_accumulator) {
					owner = fmt::format("{} {} light {} focus descriptor {} (drawFocusShadows {})", a_list, kind, static_cast<const void*>(light), d,
						data.drawFocusShadows);
					return;
				}
		};
		if (const auto* node = Engine::Global<RE::ShadowSceneNode*>(Engine::Address::kShadowSceneNode)) {
			const auto& scene = node->GetRuntimeData();
			check(scene.sunShadowDirLight, "the sun's");
			for (const auto& light : scene.activeShadowLights)
				check(light.get(), "an active");
			for (const auto* light : scene.shadowLightsAccum)
				check(light, "an accumulated");
		}
		return fmt::format("{}: {}; {}; the frame's list has {} views, the focus target count is {}", a_accumulator,
			owner.empty() ? std::string("no shadow light of the scene node holds it") : owner, listed, views.size(),
			Engine::Global<std::uint32_t>(Engine::Address::kFocusTargetCount));
	}

	void ShadowViews::SetDrawable(bool a_drawable)
	{
		// Decided before any light is in, so it covers every view AddCaster and Rebuild list.
		drawable = a_drawable;
	}

	std::uint32_t ShadowViews::ViewOfAccumulator(const void* a_accumulator) const
	{
		const auto it = accumulatorToView.find(a_accumulator);
		return it == accumulatorToView.end() ? ~0u : it->second;
	}
}
