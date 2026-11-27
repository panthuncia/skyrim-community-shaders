#pragma once

// The shadow light set (render-thread roadmap, phase 7 item 2; notes in docs/development/plans/snap7-lights.md). What the shadow maps,
// the shadow masks and the lens flares read of the frame's kept shadow lights, as values. Native's caster selection makes it on the
// game side, right after it keeps the lights: it does the shadow maps' bookkeeping there (each descriptor's target, slice and light
// transform, which the light's Render did inside the shadow maps), copies what the render side needs, then gives the lights' maps back
// (the masks' loop did that after its draws). The render half adopts the set and reads only it: no shadow light, light camera or
// selection global is read while recording. The engine's shadow light objects stay the selection's working storage on the game side
// (its camera ports keep the sun's smoothing and the accumulators there). The set holds no references: a reference would change the
// engine's own lifetime tests (the queued lights' orphan sweep, the sun light's teardown at a cell change), so lights are keys only.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "RE/N/NiFrustum.h"
#include "RE/N/NiPoint3.h"

namespace RE
{
	class BSLensFlareRenderData;
	class BSShaderAccumulator;
	class BSShadowLight;
	class ShadowSceneNode;
}

namespace DCLF::Features::Shadows::Native
{
	enum class LightKind : std::uint8_t
	{
		Directional,
		Frustum,
		Parabolic,
	};

	/** @brief A descriptor's values once the shadow maps' bookkeeping ran: its depth target (-1 for none), slice and light transform. */
	struct MapValues
	{
		std::int32_t target = -1;
		std::uint32_t slice = 0;
		std::array<float, 16> lightTransform{};
		bool culls = false;  // a culling process and the descriptor enabled: DCLF culls its casters into the view
	};

	/** @brief One RenderShadowmap call of the light's Render, in its order: the view DCLF draws, and where it draws it. */
	struct MapDraw
	{
		std::uint32_t view = ~0u;         // DCLF's shadow view (ShadowViews)
		std::uint32_t mode = 0;           // the accumulator's render mode
		std::array<float, 2> parabola{};  // a paraboloid's radius and side
		std::int32_t target = -1;         // the depth target and slice the draw clears when clear is set
		std::uint32_t slice = 0;
		bool clear = false;
	};

	/** @brief What the light's last draw leaves for the world's later passes: SetCameraData's inputs, the accumulator and render mode. */
	struct LeftState
	{
		RE::NiPoint3 eye;
		std::array<float, 3> dir{}, up{}, right{};
		RE::NiFrustum frustum;
		const RE::BSShaderAccumulator* accumulator = nullptr;  // written into the current accumulator global, never read
		std::uint32_t mode = 0;
	};

	/** @brief A kept shadow light, as values. */
	struct ShadowLight
	{
		const RE::BSShadowLight* light = nullptr;  // a key: the render side never reads through it
		LightKind kind = LightKind::Frustum;
		bool omni = false;
		std::uint32_t maps = 0;  // shadowMapCount
		std::uint32_t maskIndex = 0xFF;
		bool drawFocusShadows = false;
		bool farFlag = false;  // +0x63: the mask's ShadowLightParam.z takes the far value
		bool affectsLand = false;
		float biasScale = 0.0f;
		float spotFalloff = 0.0f;  // a frustum light's (+0x568)
		// The NiLight's values.
		bool hasNiLight = false;
		bool hidden = false;
		std::array<float, 3> position{};
		std::array<float, 3> diffuse{};
		float radius = 0.0f;
		float fade = 0.0f;
		float lodDimmer = 0.0f;
		std::vector<MapValues> descriptors;  // shadowmapDescriptors, in order
		// The shadow maps: whether DCLF draws the light (0) or why not (ShadowMapsNative's reasons), its draws and the state they leave.
		std::uint8_t verdict = 0;
		std::vector<MapDraw> draws;
		LeftState left;
		const RE::BSLensFlareRenderData* lensFlare = nullptr;  // set when the light has a lens flare and lens flares are on
	};

	/** @brief The shadow settings the masks read. */
	struct MaskSettings
	{
		std::uint32_t filterMode = 0, filterBits = 0;
		bool sceneShadows = false;
		bool focusTargets = false;  // FUN_1414cec50: the scene's shadows on and a focus target listed
		float poisson = 0.0f, resolution = 1.0f, farParam = 0.0f, biasScale = 0.0f, sunBiasScale = 0.0f, secondBiasScale = 0.0f;
	};

	/** @brief The scene node's sun light (sunShadowDirLight): the cascades' count and split distances the cascaded mask reads. */
	struct SunSplits
	{
		const RE::BSShadowLight* light = nullptr;  // a key
		std::uint32_t maps = 0;
		std::array<float, 3> start{}, end{};
	};

	/** @brief The frame's kept shadow lights, in shadowLightsAccum's walk (each light once, in slot order), with the masks' settings. */
	struct LightSet
	{
		std::vector<ShadowLight> lights;
		SunSplits sun;
		MaskSettings mask;

		/** @brief The light a_light keys, or null. */
		const ShadowLight* Find(const RE::BSShadowLight* a_light) const;
	};

	/** @brief a_light's values (no maps' bookkeeping): for a scene drawn where it is read (a menu scene, guest). */
	ShadowLight ReadLight(const RE::BSShadowLight& a_light);

	/** @brief The lights of a_node's shadowLightsAccum walk, read where they are drawn (ReadLight). */
	std::vector<ShadowLight> ReadLights(const RE::ShadowSceneNode* a_node);

	/** @brief The masks' settings, read now. */
	MaskSettings ReadMaskSettings();

	/** @brief a_sun's cascade values, read now. Null gives none. */
	SunSplits ReadSun(const RE::BSShadowLight* a_sun);

	/**
	 * @brief Game side, native, once the caster selection kept its lights and DCLF listed their views: each light's shadow maps'
	 * bookkeeping (ShadowMapsNative), the set, then each light's maps given back (CS's port of ReturnShadowmaps, which the masks' loop
	 * ran after its draws). The render half takes the set with TakeSet.
	 */
	void CaptureSet();

	/** @brief Game side: CaptureSet's result, which TakeSet hands out until the next. */
	void MadeSet(std::shared_ptr<const LightSet> a_set);

	/** @brief The set CaptureSet made last, for the render half. Null before any. */
	std::shared_ptr<const LightSet> TakeSet();

	/** @brief Render side, at the render half's start: the set this frame draws. */
	void AdoptSet(std::shared_ptr<const LightSet> a_set);

	/** @brief Render side: the adopted set, or null (guest; before the first native world frame). */
	const LightSet* RenderSet();
}
