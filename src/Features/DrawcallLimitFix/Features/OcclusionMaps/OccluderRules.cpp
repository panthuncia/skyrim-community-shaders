// The maps' occluder rules, Skylighting's and the precipitation mask's, from the walked object's records, and the maps' camera.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/Skylighting.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/PrecipitationOcclusion.h"

#include <cstring>

namespace DCLF::Features::OcclusionMaps
{
	using enum RE::BSShaderProperty::EShaderPropertyFlag;

	namespace
	{
		/** @brief Whether any of a_flags is in the object's property flags. */
		template <class... Flags>
		bool Any(const Api::OccluderLeaf& a_leaf, Flags... a_flags)
		{
			return (a_leaf.flags & (static_cast<std::uint64_t>(a_flags) | ...)) != 0;
		}

		/** @brief The depth technique both rules draw an occluder with. */
		std::uint32_t DepthTechnique(const Api::OccluderLeaf& a_leaf)
		{
			using enum RE::BSUtilityShader::Flags;
			stl::enumeration<RE::BSUtilityShader::Flags> technique;
			technique.set(RenderDepth);
			if (Any(a_leaf, kVertexColors))
				technique.set(Vc);
			if (a_leaf.alphaTesting) {
				technique.set(Texture);
				technique.set(AlphaTest);
			}
			if (Any(a_leaf, kLODObjects, kHDLODObjects))
				technique.set(LodObject);
			if (Any(a_leaf, kTreeAnim))
				technique.set(TreeAnim);
			return technique.underlying();
		}
	}

	std::uint32_t SkylightingTechnique(const Api::OccluderLeaf& a_leaf)
	{
		if (Any(a_leaf, kSkinned) && !Any(a_leaf, kTreeAnim))
			return 0;
		if (a_leaf.referenced) {
			// The nearest fade node's BSX flags.
			using Bsx = RE::BSXFlags::Flag;
			constexpr auto kRejected = static_cast<std::int32_t>(Bsx::kRagdoll) | static_cast<std::int32_t>(Bsx::kEditorMarker) | static_cast<std::int32_t>(Bsx::kDynamic) |
			                           static_cast<std::int32_t>(Bsx::kAddon) | static_cast<std::int32_t>(Bsx::kNeedsTransformUpdate) |
			                           static_cast<std::int32_t>(Bsx::kMagicShaderParticles) | static_cast<std::int32_t>(Bsx::kLights) |
			                           static_cast<std::int32_t>(Bsx::kBreakable) | static_cast<std::int32_t>(Bsx::kSearchedBreakable);
			const std::int32_t bsx = a_leaf.NearestFadeNodeBsx();
			if (bsx != -1 && (bsx & kRejected))
				return 0;
		}
		if (!Any(a_leaf, kZBufferWrite) || Any(a_leaf, kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal))
			return 0;
		return DepthTechnique(a_leaf);
	}

	std::uint32_t PrecipitationTechnique(const Api::OccluderLeaf& a_leaf)
	{
		if (Any(a_leaf, kSkinned))
			return 0;
		if (!Any(a_leaf, kZBufferWrite) ||
			Any(a_leaf, kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal))
			return 0;
		return DepthTechnique(a_leaf);
	}

	std::uint32_t EnginePrecipitationTechnique(const Api::OccluderLeaf& a_leaf)
	{
		// The engine's GetPrecipitationOcclusionMapRenderPasses (vfunc 0x2D, AE 0x1414afd50): no size test, and tree animation and
		// anisotropic lighting occlude nothing. It writes the same depth as DepthTechnique.
		if (!Any(a_leaf, kZBufferWrite) || Any(a_leaf, kSkinned, kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape,
												 kEyeReflect, kTreeAnim, kDecal, kDynamicDecal, kAnisotropicLighting))
			return 0;
		return DepthTechnique(a_leaf);
	}

	namespace
	{
		/** @brief Guest: the precipitation accumulator's camera, as the engine's ComputeProjection just left it. */
		void EngineCamera(Api::OcclusionCamera& a_camera)
		{
			auto* sky = CS_ENGINE_READ(Sky, RecordTime, globals::game::sky);
			auto* precip = sky ? sky->precip : nullptr;
			auto* camera = precip ? precip->occlusionData.camera.get() : nullptr;
			if (!camera)
				return;
			const auto& data = camera->GetRuntimeData2();
			a_camera.valid = true;
			a_camera.world = camera->world;
			a_camera.frustum = data.viewFrustum;
			static_assert(sizeof(data.port) == sizeof(a_camera.port));
			std::memcpy(a_camera.port.data(), &data.port, sizeof(a_camera.port));
		}

		void FromValues(const Snapshot::PrecipitationOcclusion::MapCamera& a_values, Api::OcclusionCamera& a_camera)
		{
			if (!a_values.valid)
				return;
			a_camera.valid = true;
			a_camera.world = a_values.world;
			a_camera.frustum = a_values.frustum;
			a_camera.port = a_values.port;
		}
	}

	void PrecipitationCamera(Api::OcclusionCamera& a_camera)
	{
		// Native: the capture's mask camera.
		if (!RenderGraph::Engine::Native::Active())
			return EngineCamera(a_camera);
		if (const auto* block = Snapshot::PrecipitationOcclusion::Current())
			FromValues(block->mask, a_camera);
	}

	void SkyCamera(Api::OcclusionCamera& a_camera)
	{
		// Native: the camera Skylighting computed for this frame's sky map.
		if (!RenderGraph::Engine::Native::Active())
			return EngineCamera(a_camera);
		FromValues(globals::features::skylighting.occlusionCamera, a_camera);
	}
}
