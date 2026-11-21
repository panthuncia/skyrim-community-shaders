#pragma once

// The eligibility rules: pure verdicts over the scene's records and a tracked entry's verdict. The coordinator classifies with
// them and the observers recompute with them. No state, and no engine read beyond the records passed in.

#include <cstdint>

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

namespace DCLF::Eligibility
{
	// A billboard turns to the camera in the main cull, after the walk read its transform. A switch draws one child at a time.
	// An ordered node orders its leaves' blended passes, and that order does not survive drawing outside the native loop.
	inline Ineligible ParentReason(RE::NiNode* a_node)
	{
		if (a_node->AsSwitchNode())
			return Ineligible::Switch;
		if (netimmerse_cast<RE::BSOrderedNode*>(a_node))
			return Ineligible::UnsupportedParent;
		if (netimmerse_cast<RE::NiBillboardNode*>(a_node))
			return Ineligible::Billboard;
		return Ineligible::None;
	}

	/** @brief ParentReason from a node's record instead of the live node. */
	inline Ineligible ParentReasonOf(const SceneCapture::NodeRecord& a_record)
	{
		return (a_record.kind & SceneCapture::kKindSwitch)    ? Ineligible::Switch :
		       (a_record.kind & SceneCapture::kKindOrdered)   ? Ineligible::UnsupportedParent :
		       (a_record.kind & SceneCapture::kKindBillboard) ? Ineligible::Billboard :
		                                                         Ineligible::None;
	}

	// A billboard outranks a switch, which outranks an ordered node. ClassifyFrame finds an ordered node under a switch by itself.
	inline Ineligible CombineParentReasons(Ineligible a_lhs, Ineligible a_rhs)
	{
		for (const auto reason : { Ineligible::Billboard, Ineligible::Switch, Ineligible::UnsupportedParent })
			if (a_lhs == reason || a_rhs == reason)
				return reason;
		return Ineligible::None;
	}

	/**
	 * @brief Whether a BSOrderedNode above a_leaf orders its draws. The engine gives the ordered group only to blended passes
	 * (FUN_1414b2330), so an opaque leaf draws in the normal opaque path and DCLF can take it.
	 */
	inline bool OrderedLeaf(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		return a_leaf.AlphaBlending() || (lighting && lighting->materialAlpha < 1.0f);
	}

	/**
	 * @brief The decal rule is the only one. A decal's group comes from its accumulated pass's hint, which does not exist before the
	 * shadow maps, so the accumulate phase takes the verdict again.
	 */
	inline bool DeferredToAccumulate(Ineligible a_reason)
	{
		return a_reason == Ineligible::Decal;
	}

	/**
	 * @brief Whether an object the main pass cannot take is still a shadow caster DCLF can draw. Billboards are not, because
	 * NiBillboardNode turns to the culling camera, which for a shadow view is the light's.
	 */
	inline bool ShadowOnlyReason(Ineligible a_reason)
	{
		return a_reason == Ineligible::Technique || a_reason == Ineligible::UnsupportedParent || a_reason == Ineligible::MultiIndex;
	}

	inline bool ShadowOnlyCaster(Ineligible a_reason, const SceneCapture::LeafView& a_leaf)
	{
		if (!ShadowOnlyReason(a_reason))
			return false;
		const auto reject = ShadowCasterReject(a_leaf);
		return reject == ShadowReject::None || reject == ShadowReject::VolumetricOnly;
	}

	bool Cacheable(Ineligible a_reason);
	/** @brief Static eligibility of a geometry, without the per-frame checks. */
	Ineligible Static(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated = nullptr);
	/** @brief Static eligibility of a geometry's main-pass layer, drawn as decal group 3. */
	Ineligible Layer(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated);

	struct EntryVerdict
	{
		bool record = false;      // it holds an object slot
		bool classified = false;  // a classification ran (Tracked::candidateFrame != 0)
		Ineligible reason = Ineligible::None;  // that classification's (Tracked::candidateReason)
	};

	inline constexpr std::uint8_t kPrimaryEngine = 0, kPrimaryMember = 1, kPrimaryHiddenStatic = 2;

	/** @brief A hidden leaf whose hidden bit changes only by events. An actor's parts change it through the behaviour graph instead. */
	inline bool HiddenStatic(const SceneCapture::LeafView& a_leaf) { return !(a_leaf.node && Extensions::OwnerClassOf(*a_leaf.node).everyFrame); }

	/** @brief Whether a tracked geometry lets its sun entry leave the cascade culls. a_leaf() is called only where a rule reads it. */
	template <class Leaf>
	bool Sun(const EntryVerdict& a_entry, bool a_switchNodes, Leaf&& a_leaf)
	{
		// A table object's shadow belongs to the shadow epoch.
		if (a_entry.record)
			return true;
		if (!a_entry.classified)
			return false;
		// The exclusion is a promise about culls a frame or two later. A fading non-member blocks it, since its fade advances only in those
		// culls. So does an actor's hidden part, since the behaviour graph can show it during Main::Draw, as a shield every 150-360 ms.
		switch (a_entry.reason) {
		case Ineligible::NotLightingShader:  // no class but Lighting casts into the cascades (measured)
		case Ineligible::AlphaBlend:         // the caster rule: no pass for an alpha-blended property
			return true;
		case Ineligible::Hidden:  // the cull skips app-culled nodes
			return HiddenStatic(a_leaf());
		case Ineligible::Fading:
			return false;
		case Ineligible::Switch:  // an unselected child: the switch node culls only the selected one
			return a_switchNodes;
		default:
			// A property no shadow view takes, such as refraction.
			return CastsNoShadow(a_leaf());
		}
	}

	/**
	 * @brief Whether a refusal is permanent, leaving the shadow to the engine, whose draws graph mode skips. Reasons that settle within a
	 * few frames, or that the engine's shadows skip anyway, are not counted.
	 */
	inline bool EngineCasterReason(Ineligible a_reason)
	{
		switch (a_reason) {
		case Ineligible::None:
		case Ineligible::Fading:
		case Ineligible::Hidden:
		case Ineligible::NotLightingShader:
		case Ineligible::AlphaBlend:
		case Ineligible::Switch:
			return false;
		default:
			return true;
		}
	}

	/**
	 * @brief Whether a tracked geometry lets its light entry leave the point lights' culls. A non-record must give the light's
	 * registration no pass. LocalLightCull owns the light's bit in activeLightMask.
	 */
	template <class Leaf>
	bool Light(const EntryVerdict& a_entry, bool a_switchNodes, Leaf&& a_leaf)
	{
		return Sun(a_entry, a_switchNodes, a_leaf) || (a_entry.classified && !SceneCapture::LeafView::Lighting(a_leaf().property));
	}

	/**
	 * @brief Whether a tracked geometry lets its entry leave the primary cull. Only a main-pass table object that is not a decal and not
	 * blended, whose main pass the synthetic pass reproduces.
	 */
	bool Primary(const EntryVerdict& a_entry, const SceneCapture::LeafView& a_leaf);

	/** @brief A hidden static's unhiding is an event that changes this verdict, so its entry is planned again. */
	template <class Leaf>
	std::uint8_t PrimaryVerdict(const EntryVerdict& a_entry, Leaf&& a_leaf)
	{
		if (a_entry.record && a_entry.classified && a_entry.reason == Ineligible::None && Primary(a_entry, a_leaf()))
			return kPrimaryMember;
		return a_entry.classified && a_entry.reason == Ineligible::Hidden && HiddenStatic(a_leaf()) ? kPrimaryHiddenStatic : kPrimaryEngine;
	}
}
