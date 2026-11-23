#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
	class BSShaderProperty;
	class BSShadowLight;
	class BSShaderAccumulator;
}

namespace DCLF::SceneCapture
{
	struct LeafView;
}

namespace DCLF
{
	/**
	 * @brief Why the engine would draw no shadow-map pass for an object, from GetRenderPasses_ShadowMapOrMask. None means a caster.
	 * It must never reject a caster the engine registers.
	 */
	enum class ShadowReject : std::uint8_t
	{
		None,
		NotLighting,  // the rule is that property's
		DeclZero,
		Faded,  // material->materialAlpha < 1. The fade node's share is the GPU's
		Refraction,  // kTempRefraction (2) or kRefraction (15)
		AlphaBlended,  // outside the decal exception
		// A decal (kDecal 26, kDynamicDecal 27) outside the one exception: kZBufferWrite (32), bit 18 and alpha blending.
		// The sun's and spot lights' accumulators drop it in FUN_1414b2a60 before asking for a pass.
		DecalNoZWrite,
		NoCastShadows,  // kCastShadows clear while the shadow global demands it
		// kCastShadows clear while the shadow global is 2. The engine gives it a pass only in the volumetric lighting copy, never the
		// sun's cascades (batch group 15, hint 8). DCLF draws it in the copy's views alone.
		VolumetricOnly,
		// A DecalNoZWrite decal without bit 18. By FUN_1414b2a60 a paraboloid light would cast it, but it was never seen to.
		// Features/Shadows/Local reports one if a scene ever registers it.
		DecalPointLight,
		// A multi-index shape's layer. The shadow modes register the main property's passes alone (FUN_1414b2a60).
		Layer,
		// Object and terrain LOD. No shadow cull reaches the LOD root, and no shadow mode registers either.
		Lod,
		Count
	};
	const char* ShadowRejectName(ShadowReject a_reason);

	/** @brief Object LOD: a BSSubIndexTriShape whose Lighting property has the LOD object flags. */
	inline bool IsLodObject(std::uint64_t a_flags, std::uint8_t a_type)
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		return a_type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kSubIndexTriShape) &&
		       (a_flags & (static_cast<std::uint64_t>(Flag::kLODObjects) | static_cast<std::uint64_t>(Flag::kHDLODObjects)));
	}
	inline bool IsLodObject(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry)
	{
		return IsLodObject(a_property.flags.underlying(), static_cast<std::uint8_t>(const_cast<RE::BSGeometry&>(a_geometry).GetType().get()));
	}

	/**
	 * @brief The previous transform BSLightingShader::SetupGeometry (0x1414dd040) draws with. LOD techniques use the current one with
	 * the previous posAdjust. A placed terrain LOD chunk keeps the previousWorld it was loaded with, the origin, for good.
	 */
	inline const RE::NiTransform& DrawnPreviousWorld(const RE::BSGeometry& a_geometry);

	/** @brief Terrain LOD: a BSTriShape whose Lighting property has the LOD landscape flag. */
	inline bool IsLodLand(std::uint64_t a_flags, std::uint8_t a_type)
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		return a_type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape) && (a_flags & static_cast<std::uint64_t>(Flag::kLODLandscape));
	}
	inline bool IsLodLand(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry)
	{
		return IsLodLand(a_property.flags.underlying(), static_cast<std::uint8_t>(const_cast<RE::BSGeometry&>(a_geometry).GetType().get()));
	}

	inline const RE::NiTransform& DrawnPreviousWorld(const RE::BSGeometry& a_geometry)
	{
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
		return lighting && (IsLodObject(*lighting, a_geometry) || IsLodLand(*lighting, a_geometry)) ? a_geometry.world : a_geometry.previousWorld;
	}

	/**
	 * @brief The engine's verdict on whether an object casts into a shadow map. Faded reads the material's alpha alone, since the
	 * fade node's share is FadeStateCS's.
	 */
	ShadowReject ShadowCasterReject(const SceneCapture::LeafView& a_leaf);
	/**
	 * @brief Whether a Lighting property's own flags give it no shadow pass in any view, whatever the frame.
	 * Its entry can leave every shadow cull, since DCLF writes the activeLightMask bits it still needs.
	 */
	bool CastsNoShadow(const SceneCapture::LeafView& a_leaf);

	/** @brief The Utility technique the engine derives for a caster, without the view's mode bits. The registered pass is this plus 0x2B. */
	std::uint32_t ShadowUtilityTechnique(const SceneCapture::LeafView& a_leaf);
	/** @brief ShadowUtilityTechnique from a Lighting property's flags and whether its geometry alpha tests. */
	std::uint32_t ShadowUtilityTechnique(std::uint64_t a_flags, bool a_alphaTesting);
	/** @brief Every value ShadowUtilityTechnique can return, each once, for any flags. */
	std::vector<std::uint32_t> AllShadowUtilityTechniques();

	/** @brief The technique bits one accumulator render mode contributes, 0xC RenderDepth to 0xF Pb. */
	std::uint32_t ShadowModeBits(std::uint32_t a_renderMode);

	/**
	 * @brief The shadow light selection's slots, as the array GetShadowCasterLightArrayEntry indexes, plus the focus shadow hosts.
	 * Render thread. Invalidated at the selection's start and published at its end, so an early return leaves it invalid.
	 * The spans stay valid until the next selection.
	 */
	struct LightSlotFrame
	{
		std::uint32_t sceneFrame = ~0u;
		std::span<const RE::BSShadowLight* const> slots;
		std::span<const RE::BSShadowLight* const> focusHosts;
		std::uint32_t focusCount = 0;  // at most 4
		bool valid = false;
		bool published = false;  // once set, the views' renderers may be published light by light

		void Invalidate()
		{
			valid = false;
			slots = {};
			focusHosts = {};
			focusCount = 0;
		}
		void Publish(std::uint32_t a_sceneFrame, std::span<const RE::BSShadowLight* const> a_slots, std::span<const RE::BSShadowLight* const> a_focusHosts,
			std::uint32_t a_focusCount)
		{
			sceneFrame = a_sceneFrame;
			slots = a_slots;
			focusHosts = a_focusHosts;
			focusCount = a_focusCount;
			valid = true;
			published = true;
		}
		/** @brief This frame's slots, or null before the selection has run this frame. */
		const LightSlotFrame* Current(std::uint32_t a_sceneFrame) const { return valid && sceneFrame == a_sceneFrame ? this : nullptr; }
	};

	/**
	 * @brief The frame's shadow views, as the engine is about to draw them.
	 *
	 * One view per ShadowmapDescriptor: a sun cascade, a spot light frustum, a paraboloid hemisphere or a focus shadow. The list is
	 * rebuilt in the engine's own order when CalculateAndDrawShadowCasterLights returns. Before that, each light's views are covered
	 * as the engine keeps it, so its registrations are already withheld.
	 * Downstream a view is identified by its accumulator and its batch renderer.
	 * Render thread only, valid from the end of CalculateAndDrawShadowCasterLights to the end of the frame's shadow draws.
	 */
	class ShadowViews
	{
	public:
		enum class Kind : std::uint8_t
		{
			Directional,
			Frustum,
			Parabolic,  // point lights, two hemispheres in one slice
			Other,
			Count
		};
		static constexpr std::uint32_t kKindCount = static_cast<std::uint32_t>(Kind::Count);
		static const char* KindName(Kind a_kind);

		/** @brief A rectangle of an array slice, as NiRect holds it (left, right, top, bottom, y up). */
		struct Port
		{
			std::int32_t left = 0, right = 0, top = 0, bottom = 0;
		};

		struct View
		{
			const RE::BSShadowLight* light = nullptr;
			std::uint32_t lightIndex = 0;
			Kind kind = Kind::Other;
			std::uint32_t descriptor = 0;  // index in the light's descriptor array
			bool focus = false;
			const RE::BSShaderAccumulator* accumulator = nullptr;
			const RE::BSBatchRenderer* batch = nullptr;
			std::uint32_t renderTarget = 0;  // ~0u until the draw assigns one
			std::uint32_t slice = 0;
			Port port;
			bool clear = false;
			bool enabled = false;
			/**
			 * @brief The render mode the view was drawn with last frame (0xD plain, 0xE clamped, 0xF paraboloid), stable per descriptor.
			 * 0 before its first draw.
			 */
			std::uint32_t renderMode = 0;
			/** @brief Whether DCLF draws this view this frame. Only covered views are withheld and captured. */
			bool covered = false;
			/**
			 * @brief DCLF's prediction of the depth target and slice. A sun cascade with volumetricCopy is drawn twice, the copy first.
			 */
			std::uint32_t drawTarget = ~0u;
			std::uint32_t drawSlice = 0;
			bool volumetricCopy = false;
		};

		static ShadowViews& Get();
		/** @brief Render thread. The selection's slots the list is rebuilt from. */
		static LightSlotFrame& LightSlots();

		/** @brief How many shadow views DCLF's buffers can hold this frame. Call before BeginCasters. */
		void SetViewCapacity(std::uint32_t a_views) { viewCapacity = a_views; }
		/** @brief The frame's shadow-mode views DCLF would draw given enough slots. Its slots grow to this. */
		std::uint32_t Candidates() const { return candidates; }

		/** @brief Before the engine keeps its first shadow light. Drops the list and publishes no renderer. */
		void BeginCasters();
		/** @brief After BeginCasters. Whether the frame's views are DCLF's. When not, the engine draws every one and nothing is withheld. */
		void SetDrawable(bool a_drawable);
		/** @brief Whether DCLF draws this frame's shadow views. */
		bool DclfDraws() const { return drawable && viewCapacity != 0; }
		/** @brief The engine kept a_light (FUN_1414a3fb0) and accumulates it next. Covers its views and publishes their renderers first. */
		void AddCaster(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots);
		/**
		 * @brief The engine is about to register a_light's focus targets. Lists and covers those views and publishes their renderers.
		 * Returns whether any is covered.
		 */
		bool AddFocusHost(const RE::BSShadowLight* a_light);
		/** @brief Rebuilds the list from DCLF's selection and publishes its renderers, once the engine's selection is done. */
		void Rebuild();
		/** @brief Render thread, for logs. Which shadow light holds a_accumulator and whether this frame's list has it. */
		std::string DescribeAccumulator(const void* a_accumulator) const;
		/** @brief Drops the list. */
		void Clear();
		/** @brief The list has views DCLF would draw, but not this frame, so the engine draws them. */
		bool Unrecorded() const { return unrecorded; }

		std::span<const View> All() const { return views; }
		bool Valid() const { return valid; }

		/** @brief The view an accumulator belongs to, or ~0u. */
		std::uint32_t ViewOfAccumulator(const void* a_accumulator) const;

		const View* At(std::uint32_t a_id) const { return a_id < views.size() ? &views[a_id] : nullptr; }
		/** @brief The view's captures so far this frame, then counts this one. A cascade's first, with its copy, is the copy's. */
		std::uint32_t NextCapture(std::uint32_t a_id)
		{
			if (a_id >= captures.size())
				return 0;
			return captures[a_id]++;
		}

	private:
		std::vector<View> views;
		std::vector<std::uint32_t> captures;
		void PredictTargets();
		/** @brief Appends a light's descriptor views, then its focus views. */
		void AddLightViews(RE::BSShadowLight& a_light, std::uint32_t a_lightIndex, std::uint32_t a_focusCount, bool a_focusOnly = false);
		/** @brief Decides coverage from a_first on. Returns whether one is covered. */
		bool CoverFrom(std::size_t a_first);
		/** @brief Publishes the covered views' renderers to PassCapture, whole. */
		void PublishRenderers();
		ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint32_t> batchToView;
		ankerl::unordered_dense::map<const void*, std::uint32_t> accumulatorToView;
		bool valid = false;
		std::uint32_t viewCapacity = UINT32_MAX;
		std::uint32_t candidates = 0;
		bool drawable = true;
		bool unrecorded = false;  // a view would be covered but the frame's are not DCLF's
	};
}
