#pragma once

// FrameGlobals: the engine globals the scene reads, captured once per game update by the frame start's game half and immutable after.
// Scene work reads the capture its frame carries. The render thread reads the one its frame's ticket carries.

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPortFrame.h"
#include "Snapshot/EngineReads.h"

namespace RE
{
	class NiAVObject;
}

struct ID3D11ShaderResourceView;

namespace DCLF::GeometryPort
{
	struct PipelineFrame;
}

namespace DCLF
{
	/** @brief What SetupTechnique reads of the engine, sampled at the frame's start so EvaluateTechnique depends only on its inputs. */
	struct TechniqueInputs
	{
		std::array<float, 4> highDetailRange{};  // LodHighDetailRange (0x2033094), less the margin 0x1ad28d0 in z and w
		ID3D11ShaderResourceView* shadowMask = nullptr;  // kSHADOW_MASK's view
		std::uint32_t shadowMaskFilter = 0;              // t14's filter mode (iShadowMaskQuarter)
		bool shadowMaskSized = false;                    // the texture exists, so shadowMaskInverseSize holds 1/width, 1/height
		std::array<float, 2> shadowMaskInverseSize{};
		bool fog = false;  // the scene graph's fog property (FUN_1414dfad0)
		float fogNear = 0.0f, fogFar = 0.0f, fogPower = 0.0f, fogClamp = 0.0f;
		std::array<float, 3> fogNearColor{}, fogFarColor{};
		float invFrameBufferRange = 0.0f;
		std::array<float, 3> colourClamp{ 1.0f, 1.0f, 1.0f };  // fLightingOutputColourClampPost{Lit,Env,Spec}
	};

	/**
	 * @brief One node of a root view's subtree, such as first person's. The key is only dereferenced right after a live walk found it.
	 * Holds its hidden bit now, and its bit at the engine's last culls of the view when an event changed it since.
	 */
	struct RootSubtreeNode
	{
		const RE::NiAVObject* key = nullptr;
		bool geometry = false, hidden = false, culled = false, saved = false;
	};

	/**
	 * @brief The engine's frame globals the scene reads, captured by the frame start's game half once per game update, after the engine's
	 * update and before the culls. Immutable after. The render half adopts its ticket's.
	 * Scene work reads no engine memory, only the capture its frame carries. Reading it from any other thread without a scope is a defect,
	 * which is counted and logged once.
	 */
	struct FrameGlobals
	{
		bool loading = false;
		bool interior = false;
		std::array<std::uint32_t, 4> decalBias{};  // per decal group, 0 unused
		// [LightingShader] specular and envmap LOD fade thresholds.
		float specularStart = 0.09f, specularEnd = 0.10f, envmapStart = 0.09f, envmapEnd = 0.10f;
		float refractionStart = 0.025f, refractionEnd = 0.03f;  // fRefractionLODFade*
		// SetupTechniqueDescriptor's bytes at 0x2032fdb and 0x2035500.
		std::uint8_t techniqueByte12 = 0, techniqueByte7 = 0;
		// GetRenderPasses_ShadowMapOrMask's global (0x2033498): 1 means no caster without kCastShadows, 2 the volumetric copy only.
		std::uint8_t shadowGlobal = 0;
		// StaticShadowBits: the main accumulator's deferred-shadow byte at +0x178, and whether it exists.
		bool accumulator = false;
		std::uint8_t accumulatorDeferredShadow = 0, noSunShadowDir = 0, screenDoorFades = 0;
		// The fade update's constants.
		std::uint8_t fadesOn = 0, fadeLodUpdates = 0;
		float fadeSpecialA = 0.0f, fadeSpecialB = 0.0f;  // 0x332a254, the metric override, and 0x1769578
		float fadeDistanceMult = 0.0f, fadeOutThreshold = 0.0f, fadeDefaultScale = 0.0f, metricScale = 0.0f;
		std::array<float, 16> fadeTypeDivisors{};
		float lodRadiusBase = 0.0f, lodExponentScale = 0.0f, lodPowBase = 0.0f;  // FUN_14147a430's scale: 0x2032e40, 0x2032e54, 0x2032e44
		std::array<float, 3> eye{};
		float lodAdjust = 0.0f;
		// Native: the pipelines' PerGeometry frame (GeometryPort::SamplePipelineFrame) taken here; its eye is zero where native's Prepass
		// runs (inside RenderWorld). Null in guest, whose Prepass samples it with the accumulator's eye.
		std::shared_ptr<const GeometryPort::PipelineFrame> pipeline;
		// ExtrasFrame's game inputs (CaptureExtrasInputs): the land blend position and ProjectedUVParams3.
		std::array<float, 2> landBlend{};
		std::array<float, 4> projectedGlobals{};
		// The features' frame-globals contributions.
		std::array<float, Extensions::kFrameGlobalsFloats> features{};
		// Roots whose hidden bit the culls change around the scene's views, as each view sees it. Sorted.
		std::vector<std::pair<const RE::NiAVObject*, bool>> cullHidden;
		// The sequence just before cullHidden was read. A hidden event numbered after it may be newer than the captured bit,
		// so it is announced again at a newer capture.
		std::uint64_t cullHiddenSequence = 0;
		// Geometry keys under the first-person root as the frame's start walked them. Sorted. Used by a diagnostic.
		std::vector<const RE::NiAVObject*> firstPersonGeometry;
		std::uint32_t membershipWitness = 0;
		TechniqueInputs technique;
		// A reference on technique.shadowMask. The technique rows name it and the scene work requests its binding, so it must outlive the request.
		winrt::com_ptr<ID3D11ShaderResourceView> shadowMaskHeld;
		MaterialPort::MaterialFrame material;

		/** @brief Game half of the frame start (FrameDriver::GameFrameStart), once per game update. The update's capture. */
		static std::shared_ptr<const FrameGlobals> Capture();
		/** @brief Render half of the frame start. The ticket's capture, which Current() hands the render owner from now on. */
		static void Adopt(std::shared_ptr<const FrameGlobals> a_globals);
		/**
		 * @brief Render thread, at RenderDepth's first-person draw. Records every node's hidden bit under a_root as the engine's culls saw them.
		 * The next frame's capture uses these instead of the frame start's bits.
		 */
		static void NoteFirstPersonCulled(const RE::NiAVObject* a_root);
		/**
		 * @brief Native, game thread: binds the update's capture to the rest of the driver's game half (the light walk, the shadow
		 * casters, Light Limit Fix's lights), from DCLF's frame start until EndGameHalf. A repeated render binds its ticket's again.
		 */
		static void BeginGameHalf(std::shared_ptr<const FrameGlobals> a_globals);
		/** @brief Native, game thread: the end of the driver's game half. */
		static void EndGameHalf();
		/** @brief The capture this thread reads: its Scope's, the game half's on the game thread, or the latest adopted on the thread that owns rendering. */
		static const FrameGlobals& Current();
		/** @brief Reads since the last call from a thread with neither a scope nor render ownership. The live-read detector's FrameGlobals site holds the count. */
		static std::uint64_t TakeUnscopedReads();

		/** @brief Binds a capture to this thread for the scene work's frame. Engine reads inside it count as captures (Snapshot/EngineReads.h). */
		class Scope
		{
		public:
			explicit Scope(std::shared_ptr<const FrameGlobals> a_globals);
			~Scope();
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

		private:
			std::shared_ptr<const FrameGlobals> held;
			const FrameGlobals* previous = nullptr;
			// The live-read detector counts reads under a bound capture as captures.
			Snapshot::EngineReads::CaptureScope capture;
		};
	};
}
