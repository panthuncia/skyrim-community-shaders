#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Switches.h"

bool RenderGraph::Engine::ImageSpace::Requested()
{
	static const bool requested = Switches::On("CS_ORG_IMAGESPACE");
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#	include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"
#	include "Features/IBL/ORGIBL.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Constants/EngineConstants.h"
#	include "RenderGraph/Engine/Pipeline/Descriptions.h"
#	include "RenderGraph/Engine/Pipeline/StateConversion.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/ProgramCache.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "ShaderCache.h"
#	include "State.h"
#	include "Utils/D3D.h"
#	include "Utils/Logging.h"
#	include "Utils/SharedShaderStore.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <cctype>
#	include <cfloat>
#	include <cmath>
#	include <filesystem>
#	include <fstream>
#	include <map>
#	include <memory>
#	include <optional>
#	include <set>
#	include <span>
#	include <stdexcept>
#	include <string_view>
#	include <tuple>
#	include <unordered_map>
#	include <vector>

namespace RenderGraph::Engine::ImageSpace
{
	namespace
	{
		using FrameAnchors::Anchor;
		namespace Constants = RenderGraph::Engine::Constants;
		namespace Pipeline = RenderGraph::Engine::Pipeline;
		using Effects = RE::ImageSpaceManager;

		constexpr const char* kClientId = "cs.engine.imagespace";
		// Gate entries (one per pass, numbered by the shapes' place in the frame).
		constexpr std::uint32_t kGateEntries = 256;
		// SRV slots a shader file's ORG push block lists at most (t0..t(n-1); its samplers and UAVs follow): ISSAOComposite's seven.
		constexpr std::uint32_t kMaxSlots = 7;
		// The shape manifest: a shape not seen for this many sessions is dropped, and it is written at most this often.
		constexpr std::uint32_t kManifestSessions = 16;
		constexpr std::uint32_t kManifestWriteFrames = 300;
		constexpr std::uint32_t kManifestVersion = 1;

		/**
		 * @brief How a ShapeKey names an input: an engine render target (its RE::RENDER_TARGET), or with one of these bits an engine
		 * depth-stencil target's depth SRV (RE::RENDER_TARGETS_DEPTHSTENCIL) or a cubemap target (RE::RENDER_TARGETS_CUBEMAP).
		 */
		constexpr std::int32_t kDepthInput = 0x10000;
		constexpr std::int32_t kCubeInput = 0x20000;
		/**
		 * @brief Two names outside the engine's target tables: the water's flow map (an output: BGSAutoWater's copies write it, and
		 * BSWaterShader reads it), and a source read by the heap index the draw's latch holds (ConstantsAt::source: each flow copy
		 * reads another loaded texture).
		 */
		constexpr std::int32_t kFlowMapOutput = 0x40000;
		constexpr std::int32_t kLatchSourceInput = 0x80000;
		constexpr std::int32_t kKindBits = kDepthInput | kCubeInput | kFlowMapOutput | kLatchSourceInput;
		constexpr std::int32_t kInputIndexMask = 0xFFFF;
		/**
		 * @brief A render target that is one mip of another target's texture (the SAO camera-Z levels: an RTV and SRV of one mip, no
		 * texture of their own) is named by the texture's target with its mip + 1 in these bits. 0 there is the target's own view.
		 */
		constexpr std::int32_t kMipShift = 20;
		constexpr std::int32_t kMipBits = 0xF << kMipShift;
		constexpr std::uint32_t kMaxMip = 14;

		/** @brief The name of one mip of an engine render target. */
		constexpr std::int32_t WithMip(std::int32_t a_target, std::uint32_t a_mip)
		{
			return a_target | static_cast<std::int32_t>((a_mip + 1) << kMipShift);
		}

		/** @brief The engine target index of a name (without its kind and mip bits). */
		constexpr std::uint32_t IndexOfName(std::int32_t a_name) { return static_cast<std::uint32_t>(a_name & kInputIndexMask); }

		/** @brief The name without its mip bits: the texture it imports. */
		constexpr std::int32_t ImportOfName(std::int32_t a_name) { return a_name < 0 ? a_name : (a_name & ~kMipBits); }

		/** @brief The mip a name views, or AnchorPasses::kAllMips for the target's own view. */
		constexpr std::uint32_t MipOfName(std::int32_t a_name)
		{
			const auto bits = a_name < 0 ? 0u : static_cast<std::uint32_t>((a_name & kMipBits) >> kMipShift);
			return bits ? bits - 1 : AnchorPasses::kAllMips;
		}

		/** @brief One effect the port covers: the engine's effect, its CS source and the slots it reads. */
		struct EffectInfo
		{
			Effects::ImageSpaceEffectEnum id;  // its index in ImageSpaceManager::effects (ImageSpaceManager::GetCurrentIndex)
			std::string_view effectName;       // R4's enum name, also its name in the shape manifest
			std::string_view group;            // for CS_ORG_IMAGESPACE_EFFECTS
			const char* source;                // under Data/Shaders
			std::uint32_t slots;               // the source's SRV count in its ORG resource list
			std::uint32_t reads;               // bit per texture slot the shader reads with these defines
			std::uint32_t samplers;            // the source's sampler count (s0..s(n-1)): a read slot below it has its sampler
			std::uint32_t cubes = 0;           // bit per slot CS binds the reflection cubemap to itself (VanillaBinding.cpp)
			bool compute = false;              // a compute shader (BSImagespaceShader::DispatchComputeShader): one UAV, CS b0
			// Drawn outside every scope (ScopeKind::Effect): the effect's own call is its scope, and its batch runs at the call's end.
			bool ownScope = false;
			std::uint32_t targets = 1;  // render targets it writes (SV_Target0..): 2 for ISWaterBlend
			// The SAO composite: after its samplers, s9 (Exponential Height Fog's SampColorSampler) and the five feature handles its
			// headers read (IBL's environment and sky SH, the fog's near and far volumes, Dynamic Cubemaps' environment).
			bool features = false;
			// An effect that keeps its own viewport (ImageSpaceEffect::unk88): the viewport is what the effect before it left, so each
			// of its shapes implies the whole-target one (ISRefraction into kMAIN_COPY at the full size).
			bool keepsViewport = false;
			// Drawn from its own geometry, not the manager's quad (the ripple makers): its vertex stage is its source's, which pulls the
			// geometry by device address from the latch's PerFrame words (TakeDraw); the gate opens it with the draw's index count.
			bool geometry = false;
		};

		/** @brief The push words of an effect's feature handles (EffectInfo::features): s9, then the five headers' resources. */
		constexpr std::uint32_t kFeatureWords = 6;

		// The engine slots and their names are R4's (engine-d3d11-surface.md); the read masks follow each source's code for the
		// effect's defines (GetShaderMacros, p5/imagespace-shaders.md).
		constexpr auto kEffects = std::to_array<EffectInfo>({
			{ Effects::ISHDRDownSample16, "ISHDRDownSample16", "hdr", "ISHDR.hlsl", 3, 0b001, 3 },
			// Also drawn outside the HDR effect, into kLDR_DOWNSAMPLE0 (Render, 107732): a scope of its own there.
			{ Effects::ISHDRDownSample4, "ISHDRDownSample4", "hdr", "ISHDR.hlsl", 3, 0b001, 3, 0, false, true },
			{ Effects::ISHDRDownSample16Lum, "ISHDRDownSample16Lum", "hdr", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample4RGB2Lum, "ISHDRDownSample4RGB2Lum", "hdr", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample4LumClamp, "ISHDRDownSample4LumClamp", "hdr", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample16LumClamp, "ISHDRDownSample16LumClamp", "hdr", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample4LightAdapt, "ISHDRDownSample4LightAdapt", "hdr", "ISHDR.hlsl", 3, 0b011, 3 },
			{ Effects::ISHDRDownSample16LightAdapt, "ISHDRDownSample16LightAdapt", "hdr", "ISHDR.hlsl", 3, 0b011, 3 },
			{ Effects::ISBrightPassBlur3, "ISBrightPassBlur3", "bloom", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur5, "ISBrightPassBlur5", "bloom", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur7, "ISBrightPassBlur7", "bloom", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur9, "ISBrightPassBlur9", "bloom", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur11, "ISBrightPassBlur11", "bloom", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur13, "ISBrightPassBlur13", "bloom", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur15, "ISBrightPassBlur15", "bloom", "ISBlur.hlsl", 2, 0b11, 2 },
			// The blur composites' draws (the bright-pass composite's vertical draw into kHDR_BLOOM, the full-screen, depth-of-field and
			// get-hit blurs): t0 only, without BRIGHTPASS. One shader per radius, as the bright-pass blurs. Outside every scope (the
			// end-of-frame chain's composites) each draw is a scope of its own.
			{ Effects::ISBlur3, "ISBlur3", "blur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISBlur5, "ISBlur5", "blur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISBlur7, "ISBlur7", "blur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISBlur9, "ISBlur9", "blur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISBlur11, "ISBlur11", "blur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISBlur13, "ISBlur13", "blur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISBlur15, "ISBlur15", "blur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISNonHDRBlur3, "ISNonHDRBlur3", "nonhdrblur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISNonHDRBlur5, "ISNonHDRBlur5", "nonhdrblur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISNonHDRBlur7, "ISNonHDRBlur7", "nonhdrblur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISNonHDRBlur9, "ISNonHDRBlur9", "nonhdrblur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISNonHDRBlur11, "ISNonHDRBlur11", "nonhdrblur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISNonHDRBlur13, "ISNonHDRBlur13", "nonhdrblur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISNonHDRBlur15, "ISNonHDRBlur15", "nonhdrblur", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			// The HDR effect's tonemap (step 5 of its Render): t0 the bloom, t1 the scene, t2 the luminance. With Post Processing on it is
			// ISHDR's passthrough (POSTPROCESS); with Effects11 owning the tonemap the engine never calls it. The fade form when the
			// manager's fade is above 0.
			{ Effects::ISHDRTonemapBlendCinematic, "ISHDRTonemapBlendCinematic", "tonemap", "ISHDR.hlsl", 3, 0b111, 3 },
			{ Effects::ISHDRTonemapBlendCinematicFade, "ISHDRTonemapBlendCinematicFade", "tonemap", "ISHDR.hlsl", 3, 0b111, 3 },
			// The copies whose vertex shader passes the coordinates through (ISCopyScaleBias and ISCopyTextureMask move them in theirs,
			// which the graph's fullscreen triangle cannot): back to the chain's destination in the pre-tonemap composites, and the main
			// frame's, the menus' and the end-of-frame chain's copies outside every scope (each call a scope of its own).
			{ Effects::ISCopy, "ISCopy", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISCopyDynamicFetchDisabled, "ISCopyDynamicFetchDisabled", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISCopyCustomViewport, "ISCopyCustomViewport", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISCopyGrayScale, "ISCopyGrayScale", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			// The get-hit effect's last draw (end-of-frame chain slot 7): t0 the image, t1 its blurred copy (kGETHIT_BUFFER's blur).
			{ Effects::ISDoubleVision, "ISDoubleVision", "gethit", "ISDoubleVision.hlsl", 2, 0b11, 2, 0, false, true },
			// The water's displacement simulation (the WaterDisplacement effect, slot 31, from the water system's update: ScopeKind::Water)
			// into kWATER_DISPLACEMENT and its swap. The ripple makers draw their own geometry (one quad per ripple), pulled by its
			// vertex stage (EffectInfo::geometry).
			{ Effects::ISWaterDisplacementClearSimulation, "ISWaterDisplacementClearSimulation", "waterdisplacement", "ISWaterDisplacement.hlsl", 0, 0, 0 },
			{ Effects::ISWaterDisplacementTexOffset, "ISWaterDisplacementTexOffset", "waterdisplacement", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterWadingHeightmap, "ISWaterWadingHeightmap", "waterdisplacement", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterRainHeightmap, "ISWaterRainHeightmap", "waterdisplacement", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterBlendHeightmaps, "ISWaterBlendHeightmaps", "waterdisplacement", "ISWaterDisplacement.hlsl", 2, 0b11, 2 },
			{ Effects::ISWaterSmoothHeightmap, "ISWaterSmoothHeightmap", "waterdisplacement", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterDisplacementNormals, "ISWaterDisplacementNormals", "waterdisplacement", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterDisplacementWadingRipple, "ISWaterDisplacementWadingRipple", "waterdisplacement", "ISWaterDisplacement.hlsl", 0, 0, 0, 0, false, false, 1, false,
				false, true },
			{ Effects::ISWaterDisplacementRainRipple, "ISWaterDisplacementRainRipple", "waterdisplacement", "ISWaterDisplacement.hlsl", 0, 0, 0, 0, false, false, 1, false,
				false, true },
			{ Effects::ISCompositeVolumetricLighting, "ISCompositeVolumetricLighting", "composite", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b01, 2 },
			{ Effects::ISCompositeLensFlare, "ISCompositeLensFlare", "composite", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b10, 2 },
			{ Effects::ISCompositeLensFlareVolumetricLighting, "ISCompositeLensFlareVolumetricLighting", "composite", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b11, 2 },
			// RenderSSAO (100520 / 107245), through the explicit-target Render (100951 / 107733).
			{ Effects::ISSAOCameraZ, "ISSAOCameraZ", "sao", "ISSAOCameraZ.hlsl", 1, 0b1, 1 },
			{ Effects::ISMinify, "ISMinify", "sao", "ISSAOMinify.hlsl", 1, 0b1, 1 },
			{ Effects::ISMinifyContrast, "ISMinifyContrast", "sao", "ISSAOMinify.hlsl", 1, 0b1, 1 },
			{ Effects::ISSAORawAO, "ISSAORawAO", "sao", "ISSAORawAO.hlsl", 4, 0b1111, 4 },
			{ Effects::ISSAORawAONoTemporal, "ISSAORawAONoTemporal", "sao", "ISSAORawAO.hlsl", 4, 0b0011, 4 },
			{ Effects::ISSAOBlurH, "ISSAOBlurH", "sao", "ISSAOBlur.hlsl", 1, 0b1, 1 },
			{ Effects::ISSAOBlurV, "ISSAOBlurV", "sao", "ISSAOBlur.hlsl", 1, 0b1, 1 },
			// RenderSSAO's composite into the destination (kMAIN): t0 the source, t1 kSAO, t2 the depth, t3 the normals' SSR mask, t4 the
			// snow's specular alpha, t5 the shadow mask, t6 the reflections (all engine targets, SE 0x1412ebd70). CS's IBL and Exponential
			// Height Fog headers read their features' outputs too: by published id (EffectInfo::features).
			{ Effects::ISSAOCompositeSAO, "ISSAOCompositeSAO", "sao", "ISSAOComposite.hlsl", 7, 0b1111111, 7, 0, false, false, 1, true },
			{ Effects::ISSAOCompositeFog, "ISSAOCompositeFog", "sao", "ISSAOComposite.hlsl", 7, 0b1111111, 7, 0, false, false, 1, true },
			{ Effects::ISSAOCompositeSAOFog, "ISSAOCompositeSAOFog", "sao", "ISSAOComposite.hlsl", 7, 0b1111111, 7, 0, false, false, 1, true },
			// The screen-space reflections pass (100428 / 107146): the normal mask's downsample, the ray march (t4: the reflection
			// cubemap CS binds) and the separable compute blur (107207: horizontal, then vertical, each through 107734).
			{ Effects::ISDownsampleIgnoreBrightest, "ISDownsampleIgnoreBrightest", "ssr", "ISDownsample.hlsl", 3, 0b001, 3 },
			{ Effects::ISReflectionsRayTracing, "ISReflectionsRayTracing", "ssr", "ISReflectionsRayTracing.hlsl", 5, 0b11111, 4, 0b10000 },
			{ Effects::ISReflectionBlurHCS, "ISReflectionBlurHCS", "ssr", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true },
			{ Effects::ISReflectionBlurVCS, "ISReflectionBlurVCS", "ssr", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true },
			// The end-of-frame chain's effects (ImageSpaceManager 99023 / 105674): depth of field and the distant blur (t0 the image,
			// t1 the blurred image, t2 the depth, t3 the average depth unless MASKED, t4 the mask with MASKED), refraction (t1 the
			// refraction normals) and the radial blur. Each is a scope of its own. The chain's last effect draws into kFRAMEBUFFER.
			{ Effects::ISDepthOfField, "ISDepthOfField", "dof", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false, true },
			{ Effects::ISDepthOfFieldFogged, "ISDepthOfFieldFogged", "dof", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false, true },
			{ Effects::ISDepthOfFieldMaskedFogged, "ISDepthOfFieldMaskedFogged", "dof", "ISDepthOfField.hlsl", 5, 0b10111, 5, 0, false, true },
			{ Effects::ISDistantBlur, "ISDistantBlur", "dof", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false, true, 1, false, true },
			{ Effects::ISDistantBlurFogged, "ISDistantBlurFogged", "dof", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false, true, 1, false, true },
			{ Effects::ISDistantBlurMaskedFogged, "ISDistantBlurMaskedFogged", "dof", "ISDepthOfField.hlsl", 5, 0b10111, 5, 0, false, true, 1, false, true },
			{ Effects::ISRefraction, "ISRefraction", "refraction", "ISRefraction.hlsl", 2, 0b11, 2, 0, false, true, 1, false, true },
			{ Effects::ISRadialBlur, "ISRadialBlur", "radialblur", "ISRadialBlur.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISRadialBlurMedium, "ISRadialBlurMedium", "radialblur", "ISRadialBlur.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISRadialBlurHigh, "ISRadialBlurHigh", "radialblur", "ISRadialBlur.hlsl", 1, 0b1, 1, 0, false, true },
			// The world map's depth blur (the map menu's view, outside the frame's scopes: each call a scope of its own): t0 the image,
			// t1 the depth. The NoSkyBlur form leaves the sky sharp (NO_SKY).
			{ Effects::ISWorldMap, "ISWorldMap", "worldmap", "ISWorldMap.hlsl", 2, 0b11, 2, 0, false, true },
			{ Effects::ISWorldMapNoSkyBlur, "ISWorldMapNoSkyBlur", "worldmap", "ISWorldMap.hlsl", 2, 0b11, 2, 0, false, true },
			// The depth-of-field compute blur (107734 <- 107209 <- 107208, inside 99023): ISBlurCS's depth-of-field build, which reads
			// the depth at t1 (DEPTH_THRESHOLD). Horizontal, then vertical, each call a scope of its own.
			{ Effects::ISDepthOfFieldBlurHCS, "ISDepthOfFieldBlurHCS", "dof", "ISBlurCS.hlsl", 2, 0b11, 2, 0, true, true },
			{ Effects::ISDepthOfFieldBlurVCS, "ISDepthOfFieldBlurVCS", "dof", "ISBlurCS.hlsl", 2, 0b11, 2, 0, true, true },
			// The sub-rectangle copy, t0 to u0, no sampler: the snow pass's (107734 <- 107217 <- Main::RenderWorld, engine targets) and
			// Main::RenderWaterEffects' (35561 / 36560: each queued flow texture into the flow map, the source by heap index).
			{ Effects::ISCopySubRegionCS, "ISCopySubRegionCS", "copy", "ISCopySubRegionCS.hlsl", 1, 0b1, 0, 0, true, true },
			// The water's temporal blend (the explicit-target Render, outside the other scopes): t0 the image, t1 the water history,
			// t2 the motion vectors, t3 the depth, t4 the water mask; into kMAIN (SV_Target0) and the next history (SV_Target1).
			{ Effects::ISWaterBlend, "ISWaterBlend", "waterblend", "ISWaterBlend.hlsl", 5, 0b11111, 5, 0, false, true, 2 },
		});

		/**
		 * @brief The engine blend state a draw uses, by value: its four fields in the engine's blend table (SetDirtyStates' index:
		 * mode, alpha to coverage, write mode, extra), one byte each. kNoBlend for a dispatch.
		 */
		constexpr std::uint32_t kNoBlend = UINT32_MAX;

		constexpr std::uint32_t PackBlend(std::uint32_t a_mode, std::uint32_t a_alphaToCoverage, std::uint32_t a_writeMode, std::uint32_t a_extra)
		{
			return (a_mode & 0xFF) | (a_alphaToCoverage & 0xFF) << 8 | (a_writeMode & 0xFF) << 16 | (a_extra & 0xFF) << 24;
		}

		/** @brief The engine's blend state object of a packed blend (its table entry now), or null. */
		ID3D11BlendState* BlendStateOf(std::uint32_t a_blend)
		{
			if (a_blend == kNoBlend)
				return nullptr;
			return Pipeline::EngineBlendState(a_blend & 0xFF, (a_blend >> 8) & 0xFF, (a_blend >> 16) & 0xFF, (a_blend >> 24) & 0xFF);
		}

		/**
		 * @brief The description of a packed engine blend state: read from the engine's table when it has the entry (and kept), else
		 * the one kept from an earlier read this session or from the manifest. False when neither has it.
		 */
		bool BlendDescOf(std::uint32_t a_blend, D3D11_BLEND_DESC& a_out);

		/**
		 * @brief What one pass is made for, by value (the shape manifest stores it): the effect, its outputs and inputs, its samplers,
		 * its blend state and its viewport. A compute shape's output is the render target behind its UAV (u0); it has no blend state and
		 * no viewport.
		 */
		struct ShapeKey
		{
			std::uint32_t effect = 0;   // kEffects index
			std::int32_t output = -1;   // RE::RENDER_TARGET, with a mip (WithMip)
			std::int32_t output1 = -1;  // SV_Target1's render target (EffectInfo::targets 2), or -1
			std::array<std::int32_t, kMaxSlots> inputs{ -1, -1, -1, -1, -1, -1, -1 };  // kDepthInput / kCubeInput / kLatchSourceInput, or a render target (with a mip)
			std::array<std::uint32_t, kMaxSlots> address{};
			std::array<std::uint32_t, kMaxSlots> filter{};
			std::uint32_t blend = kNoBlend;  // PackBlend
			bool compute = false;
			std::array<std::uint32_t, 4> viewport{};  // x, y, width, height in the output's pixels (of its mip)

			auto Tie() const { return std::tie(effect, output, output1, inputs, address, filter, blend, compute, viewport); }
			bool operator<(const ShapeKey& a_other) const { return Tie() < a_other.Tie(); }
			bool operator==(const ShapeKey& a_other) const { return Tie() == a_other.Tie(); }
		};

		/** @brief One pass: its immutable spec, made at a registration. Its program is taken when the pass is made (a graph build). */
		struct Spec
		{
			ShapeKey key;
			std::uint32_t entry = 0;  // gate entry, latch region (entry * 32) and order key
			std::string name;
			std::shared_ptr<org::ExternalTextureResource> output;
			std::shared_ptr<org::ExternalTextureResource> output1;
			std::array<std::shared_ptr<org::ExternalTextureResource>, kMaxSlots> inputs;
			rhi::Format format = rhi::Format::Unknown;        // the outputs' render target view formats
			rhi::Format format1 = rhi::Format::Unknown;
		};

		/** @brief OrgEngineConstantsAt in Common/OrgEngineConstants.hlsli. */
		struct ConstantsAt
		{
			std::uint32_t srv = 0;  // PerGeometry (PS b2), or a dispatch's constants (CS b0)
			std::uint32_t offset = 0;
			std::uint32_t frameSrv = 0;  // PerFrame (PS b12), read as FrameBuffer; unused by a dispatch
			std::uint32_t frameOffset = 0;
			std::uint32_t size = 0;  // the bound bytes of each: rows past them read 0, as on D3D11
			std::uint32_t frameSize = 0;
			std::uint32_t source = UINT32_MAX;  // a kLatchSourceInput's heap index (ORG_ENGINE_NO_SOURCE: none)
			std::uint32_t padding = 0;
		};
		static_assert(sizeof(ConstantsAt) == 32);

		/** @brief A taken draw or dispatch waiting for its batch's run: its pass, where its constants are, and a dispatch's groups. */
		struct Pending
		{
			std::uint32_t entry = 0;
			ConstantsAt at;
			bool compute = false;
			std::array<std::uint32_t, 3> groups{};
			std::shared_ptr<const void> source;  // the descriptor and texture behind at.source, held until the run's GPU work retires
			std::uint32_t vertices = 3;          // a draw's vertex count: the fullscreen triangle's, or a geometry effect's indices
		};

		/** @brief The BSImagespaceShader::Render (or DispatchComputeShader) call in progress. */
		struct EffectScope
		{
			const RE::BSImagespaceShader* shader = nullptr;
			std::size_t effect = 0;
			bool covered = false;   // an effect the port covers and selected: its draws are the graph's or refused, never vouched for
			bool eligible = false;  // a covered effect whose draw may be taken
			bool drew = false;      // its draw reached DrawTriShape (its dispatch DispatchCSShader)
			bool compute = false;   // a DispatchComputeShader call
		};

		/**
		 * @brief The engine functions whose ImageSpace work is taken, each a scope: its draws are batched, and the batch runs at the
		 * scope's end at the latest (CS code around these calls may use D3D11 or run other anchors).
		 */
		enum class ScopeKind : std::uint32_t
		{
			Composites,   // ImageSpaceManager::RenderPreTonemapComposites (99024 / 105675)
			HDR,          // ImageSpaceEffectHDR::Render (100465 / 107183)
			SSAO,         // RenderSSAO (100520 / 107245)
			Reflections,  // the screen-space reflections pass (100428 / 107146)
			Effect,       // one call of an effect drawn outside every other scope (EffectInfo::ownScope)
			FlowMap,      // one of Main::RenderWaterEffects' copies into the water's flow map (an own-scope ISCopySubRegionCS call)
			Water,        // the water system's displacement update (31385 / 32176): the WaterDisplacement effect's simulation draws
			Count
		};

		/**
		 * @brief How many batches each kind of scope may start in a frame: its share of the anchor's runs a frame. Each kind has a
		 * share of its own, so the scopes early in the frame never use up the runs of the ones after them. The pre-tonemap
		 * composites, the SAO and reflection passes and the HDR effect have a second run for a batch cut short by a draw the graph
		 * does not take. Effect scopes have one run per call: the depth-of-field (or distant blur) draw, its two compute blurs,
		 * refraction, the radial blur, the snow copy, the water blend, the LDR downsample and the copies outside every scope (the
		 * main frame's, the menus' and the end-of-frame chain's), and there the blur composites' and the get-hit effect's draws. The
		 * water's flow copies have a share of their own (one run per copy: a cell load queues a few). The water's displacement update
		 * has two (a second for a batch cut short by a ripple, which the graph does not take).
		 * Run leases give an anchor as many runs as it makes, so these are bounds against a runaway frame, not budgets: each is
		 * several times what a frame uses (the blur composites, bright pass and adaptation chain take several runs in one scope).
		 */
		constexpr std::array<std::uint32_t, static_cast<std::size_t>(ScopeKind::Count)> kScopeRuns{ 16, 16, 16, 16, 64, 16, 8 };

		constexpr std::uint32_t ScopeRunsTotal()
		{
			std::uint32_t total = 0;
			for (const auto runs : kScopeRuns)
				total += runs;
			return total;
		}

		/** @brief A raster program's key: the effect, the outputs' formats and the packed blend state. */
		using ProgramKey = std::tuple<std::size_t, rhi::Format, rhi::Format, std::uint32_t>;

		/** @brief One shape of the manifest: where it is in the frame, and the last session that saw it. */
		struct ManifestEntry
		{
			std::uint32_t position = 0;  // scope << 16 | draw within the scope
			std::uint32_t lastSession = 0;
		};

		struct ImageSpaceState
		{
			bool installed = false;
			bool reserved = false;
			bool on = false;  // set up; off again for the session after a failure
			bool flushing = false;
			bool logShapes = false;
			// The draw DrawTriShape is about to make, for a geometry effect's TakeDraw: the shape's renderer data and its index range.
			const void* drawShape = nullptr;
			std::uint32_t drawStart = 0, drawTriangles = 0;
			std::array<bool, kEffects.size()> enabled{};
			std::unordered_map<const void*, std::int32_t> effectOf;  // engine shader -> kEffects index (filled from the manager's effects)
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> latch;
			std::map<std::pair<std::uint32_t, std::uint32_t>, org::OwnedDescriptorBinding> samplers;  // (address, filter)
			org::OwnedDescriptorBinding defaultSampler;  // D3D11's default sampler: the SAO composite's s9 (FeatureBinds)
			// A 1x1x1 volume of (0, 0, 0, 1), no in-scatter and full transmittance, by heap index: what the SAO composite reads for Exponential
			// Height Fog's volumes before the fog publishes them (D3D11 binds none then, and the header's sample of a null volume gives the
			// same). Made at the first build that needs it.
			winrt::com_ptr<ID3D11ShaderResourceView> noFogView;
			std::uint32_t noFogIndex = UINT32_MAX;
			std::shared_ptr<const void> noFogOwner;
			bool noFogTried = false;
			// By run lease index: the sources a run's latched draws read by heap index (Pending::source), held until the next run of
			// the index commits (the lease's last run retired before the index was opened again). Grown with the host's lease indices.
			std::vector<std::vector<std::shared_ptr<const void>>> heldSources;
			// The registration: every pass, by entry and by shape, and the engine objects its imports were made from (a resize check).
			std::vector<std::shared_ptr<const Spec>> specs;
			std::map<ShapeKey, const Spec*> specOf;
			std::map<std::int32_t, const void*> importedObjects;
			std::set<ShapeKey> refusedShapes;    // shapes no pass can be made for (logged once)
			std::set<ShapeKey> loggedConstants;  // shapes whose constant sizes were logged (CS_ORG_IMAGESPACE_LOG)
			std::uint32_t registrations = 0;
			// The shape manifest (Data/ShaderCache/v2/ImageSpaceShapes.json): read at Setup, written when a session sees new shapes.
			std::map<ShapeKey, ManifestEntry> manifest;
			std::set<ShapeKey> seen;  // shapes drawn or refused this session
			std::uint32_t session = 0;
			std::size_t seenAtWrite = 0;  // seen.size() when the manifest was last written
			// Each effect's program defines as the session that saw it composed them: a pass whose engine effect object does not exist yet
			// at the graph's build (the manager makes some late, ISCopySubRegionCS) builds its program from these.
			std::map<std::size_t, std::vector<std::pair<std::string, std::string>>> savedDefines;
			// A pass was left out at a build because an engine state table was not filled yet: the client's graph is built again once,
			// at a frame outside the world (the main menu), never in play.
			bool retryBuild = false;
			// By gate entry: the pass the last build made for it (MakePass). A shape whose pass was left out is refused, never withheld.
			std::array<bool, kGateEntries> passMade{};
			std::uint32_t manifestWritten = 0;
			std::set<const Spec*> programsQueued;
			// The frame and its batches.
			std::uint32_t scope = 0;                                   // the current scope (ScopeKind)
			std::uint32_t drawIndex = 0;                               // eligible draws and dispatches so far in the current scope
			std::array<std::uint32_t, kScopeRuns.size()> scopeRuns{};  // batches each kind of scope started this frame
			std::uint32_t frame = UINT32_MAX;
			std::uint32_t runsThisFrame = 0;
			std::uint32_t scopeDepth = 0;
			std::optional<EffectScope> current;
			std::vector<Pending> batch;
			std::vector<std::shared_ptr<const org::LatchBlock>> keepAlive;  // the batch's constant chunks, until its run
			std::uint32_t lastEntry = 0;
			bool executed = false;
			// The engine's ImageSpace calls in progress (Render, the explicit-target Render, DispatchComputeShader). No draw or dispatch
			// inside one is vouched for: one the port leaves to the engine (an effect it does not cover, a refusal, a failure) is skipped
			// by the flush latch and counted by the census. Those of effects it does not cover are named once in a while (LogEngineDraws).
			std::uint32_t engineCalls = 0;
			// The shaders of the engine calls in progress, innermost last: the effect a draw left to the engine belongs to.
			std::vector<const RE::BSImagespaceShader*> callShaders;
			// ImageSpace draws and dispatches left to D3D11, by shader, target and why, since the last summary (LogEngineDraws, every
			// kEngineDrawFrames frames).
			std::map<std::string, std::uint64_t> engineDraws;
			std::uint32_t engineDrawsSince = 0;
			// The last reason a covered effect's draw was refused or failed: the summary's why for that draw.
			std::string lastRefusal;
			// Each packed engine blend state's description, as last read from the engine's table (BlendDescOf) or the manifest: a pass
			// made at a build when the table entry is empty takes it from here.
			std::map<std::uint32_t, D3D11_BLEND_DESC> blendDescs;
			// Counters.
			std::uint64_t taken = 0, engineDrawn = 0, batches = 0, lost = 0;
			// Why each effect's draws stayed with the engine, logged once per reason and effect: refusals (cases the port does not
			// take) at info, failures (what should not happen on the primary path) at error.
			std::array<std::unique_ptr<Util::ReasonLog>, kEffects.size()> fallbacks;
			std::array<std::unique_ptr<Util::ReasonLog>, kEffects.size()> failures;
		};

		ImageSpaceState& IS()
		{
			static ImageSpaceState state;
			return state;
		}

		/** @brief The effects' raster programs, by value key. Queued at Setup; a pass made at a graph build takes its own with GetNow. */
		ProgramCache<RasterProgram, ProgramKey>& Programs()
		{
			static ProgramCache<RasterProgram, ProgramKey> programs{ "ImageSpace" };
			return programs;
		}

		/** @brief The compute effects' programs, by kEffects index. */
		ProgramCache<ComputeProgram, std::size_t>& ComputePrograms()
		{
			static ProgramCache<ComputeProgram, std::size_t> programs{ "ImageSpace compute" };
			return programs;
		}

		/** @brief A draw of a_effect stays with the engine: an inherent refusal, counted, logged the first time for that effect and reason. */
		void Refuse(std::string_view a_reason, std::size_t a_effect, std::string_view a_detail = {})
		{
			auto& state = IS();
			++state.engineDrawn;
			state.lastRefusal = a_reason;
			if (auto& log = state.fallbacks[a_effect])
				log->Report(a_reason, a_detail);
		}

		/** @brief As Refuse, for what should not happen on the primary path (constants the capture missed): logged as an error. */
		void Failure(std::string_view a_reason, std::size_t a_effect)
		{
			auto& state = IS();
			++state.engineDrawn;
			state.lastRefusal = fmt::format("failed: {}", a_reason);
			if (auto& log = state.failures[a_effect])
				log->Report(a_reason);
		}

		/** @brief A target name for the log: "rt<index>", "rt<index>.mip<n>", "depth<index>", "cube<index>", "flowmap" or "latched". */
		std::string NameForLog(std::int32_t a_name)
		{
			if (a_name < 0)
				return "none";
			if (a_name & kFlowMapOutput)
				return "flowmap";
			if (a_name & kLatchSourceInput)
				return "latched";
			const auto index = IndexOfName(a_name);
			if (a_name & kDepthInput)
				return fmt::format("depth{}", index);
			if (a_name & kCubeInput)
				return fmt::format("cube{}", index);
			const auto mip = MipOfName(a_name);
			return mip == AnchorPasses::kAllMips ? fmt::format("rt{}", index) : fmt::format("rt{}.mip{}", index, mip);
		}

		/** @brief Every effect to the engine for the session. */
		void Fail(std::string_view a_why)
		{
			auto& state = IS();
			if (!state.on)
				return;
			state.on = false;
			logger::error("[ORG] ImageSpace: {}; every ImageSpace effect returns to the engine", a_why);
			FrameAnchors::Unregister(kClientId);
		}

		/** @brief CS_ORG_IMAGESPACE_EFFECTS: effect names or groups, any case; unset, empty or "all" selects every effect. */
		void SelectEffects()
		{
			auto& state = IS();
			const auto lower = [](std::string_view a_text) {
				std::string out(a_text);
				std::ranges::transform(out, out.begin(), [](unsigned char a_char) { return static_cast<char>(std::tolower(a_char)); });
				return out;
			};
			const auto names = Switches::List("CS_ORG_IMAGESPACE_EFFECTS");
			if (names.empty() || (names.size() == 1 && lower(names.front()) == "all")) {
				state.enabled.fill(true);
				return;
			}
			for (const auto& item : names) {
				const auto name = lower(item);
				bool known = false;
				for (std::size_t i = 0; i < kEffects.size(); ++i) {
					const auto& effect = kEffects[i];
					// The engine's names drop the IS of the HDR and bloom effects (BSImagespaceShaderHDRDownSample16) and keep it for the
					// composites (BSImagespaceShaderISCompositeLensFlare).
					const auto engineName = lower(fmt::format("BSImagespaceShader{}", effect.effectName));
					const auto shortName = lower(fmt::format("BSImagespaceShader{}", effect.effectName.substr(2)));
					if (name == lower(effect.effectName) || name == engineName || name == shortName || name == lower(effect.group)) {
						state.enabled[i] = true;
						known = true;
					}
				}
				if (!known)
					logger::warn("[ORG] ImageSpace: CS_ORG_IMAGESPACE_EFFECTS names {}, which is not an effect this port covers", item);
			}
		}

		/** @brief The engine's shader object of a covered effect (ImageSpaceManager::effects), or null before the manager made it. */
		const RE::BSImagespaceShader* ShaderOf(std::size_t a_effect)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			const std::uint32_t index = RE::ImageSpaceManager::GetCurrentIndex(kEffects[a_effect].id);
			// NiTArray::size() counts the non-null entries; free_idx() is one past the highest set index (the engine's setter, AE
			// 0x141481a30). With the empty slots below it, size() cut off the last effects (ISWaterBlend at 155).
			if (!manager || index >= manager->effects.free_idx() || index >= manager->effects.capacity())
				return nullptr;
			return static_cast<const RE::BSImagespaceShader*>(manager->effects[static_cast<decltype(manager->effects)::size_type>(index)]);
		}

		std::int32_t EffectOf(const RE::BSImagespaceShader* a_shader)
		{
			auto& state = IS();
			// An entry is checked against the manager's effects now, and a miss scans them: the manager makes some effects after the
			// first lookups.
			if (const auto it = state.effectOf.find(a_shader); it != state.effectOf.end()) {
				if (ShaderOf(static_cast<std::size_t>(it->second)) == a_shader)
					return it->second;
				state.effectOf.erase(it);
			}
			for (std::size_t i = 0; i < kEffects.size(); ++i)
				if (ShaderOf(i) == a_shader) {
					state.effectOf.emplace(a_shader, static_cast<std::int32_t>(i));
					return static_cast<std::int32_t>(i);
				}
			return -1;
		}

		/** @brief A kEffects index by its name (the manifest's), or -1. */
		std::int32_t EffectNamed(std::string_view a_name)
		{
			for (std::size_t i = 0; i < kEffects.size(); ++i)
				if (kEffects[i].effectName == a_name)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/** @brief The engine render target whose own texture is a_resource, or -1. */
		std::int32_t TargetOfTexture(ID3D11Resource* a_resource)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_resource)
				return -1;
			auto* resource = Tripwire::Unwrap(a_resource);
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = RE::RENDER_TARGET::kMAIN; i < RE::RENDER_TARGET::kTOTAL; ++i)
				if (targets[i].texture && Tripwire::Unwrap(static_cast<ID3D11Resource*>(targets[i].texture)) == resource)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/**
		 * @brief The name of a target without a texture of its own whose SRV is one mip of another target's texture (the SAO camera-Z
		 * levels), or -1. The pass reads it through that texture's import, so the view must have the import's format.
		 */
		std::int32_t MipInputOfView(ID3D11ShaderResourceView* a_view)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || desc.Texture2D.MipLevels != 1 || desc.Texture2D.MostDetailedMip > kMaxMip)
				return -1;
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			const auto owner = TargetOfTexture(resource.get());
			if (owner < 0)
				return -1;
			auto* ownerView = globals::game::renderer->GetRuntimeData().renderTargets[owner].SRV;
			if (!ownerView)
				return -1;
			D3D11_SHADER_RESOURCE_VIEW_DESC ownerDesc{};
			ownerView->GetDesc(&ownerDesc);
			if (ownerDesc.Format != desc.Format)
				return -1;
			return WithMip(owner, desc.Texture2D.MostDetailedMip);
		}

		/**
		 * @brief The engine target a shader resource view is: a render target's index (with a mip for a target that is one mip of
		 * another's texture), a depth-stencil target's depth SRV as kDepthInput | index, or -1. kFRAMEBUFFER (an RTV only) is never
		 * one.
		 */
		std::int32_t InputOfView(ID3D11ShaderResourceView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return -1;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = RE::RENDER_TARGET::kMAIN; i < RE::RENDER_TARGET::kTOTAL; ++i)
				if (targets[i].SRV == a_view)
					return targets[i].texture ? static_cast<std::int32_t>(i) : MipInputOfView(a_view);
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			for (std::uint32_t i = 0; i < RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL; ++i)
				if (depths[i].depthSRV == a_view)
					return kDepthInput | static_cast<std::int32_t>(i);
			return -1;
		}

		/** @brief The engine render target an unordered access view is, or -1. */
		std::int32_t TargetOfUav(const ID3D11UnorderedAccessView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return -1;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = RE::RENDER_TARGET::kMAIN; i < RE::RENDER_TARGET::kTOTAL; ++i)
				if (targets[i].UAV == a_view)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/**
		 * @brief The water's flow map (SE 527694 / AE 414616: the NiSourceTexture "FlowMap"), BGSAutoWater's copies' target, which
		 * BSWaterShader::SetupGeometry binds. Null before it is made.
		 */
		RE::NiSourceTexture*& FlowMapSlot()
		{
			static REL::Relocation<RE::NiSourceTexture**> slot{ REL::RelocationID(527694, 414616) };
			return *slot.get();
		}

		/** @brief The flow map's renderer texture, or null. */
		RE::BSGraphics::Texture* FlowMap()
		{
			auto* texture = FlowMapSlot();
			return texture ? texture->rendererTexture : nullptr;
		}

		/**
		 * @brief Makes the water's flow map at Setup, as the engine makes it at the first cell with flowing water (SE 31234, inlined in AE
		 * 1405195d0): a size x size R8G8B8A8_UNORM texture of zeros with a UAV, size a setting's value (SE 501245 / AE 359676, the int
		 * the engine reads at +8 of its Setting) times 64, held by the NiSourceTexture "FlowMap". The engine makes it only while the
		 * slot is empty, so it uses this one; the copies' pass can then name it from the session's registration instead of a graph
		 * build in play. Main thread.
		 */
		void MakeFlowMap()
		{
			if (FlowMapSlot())
				return;
			static REL::Relocation<const std::int32_t*> sizeSetting{ REL::RelocationID(501245, 359676) };
			const auto cells = *sizeSetting;
			if (cells <= 0 || cells > 64) {
				logger::warn("[ORG] ImageSpace: the flow map's size setting is {}; the flow map is left for the engine to make", cells);
				return;
			}
			const auto size = static_cast<std::uint32_t>(cells) << 6;
			using CreateTexture2D = RE::BSGraphics::Texture*(RE::BSGraphics::Renderer*, std::uint32_t, std::uint32_t, const void*, D3D11_USAGE, DXGI_FORMAT, bool);
			using CreateUav = void(RE::BSGraphics::Renderer*, RE::BSGraphics::Texture*, DXGI_FORMAT);
			using MakeSourceTexture = RE::NiSourceTexture*(const RE::BSFixedString*);
			static REL::Relocation<CreateTexture2D*> createTexture{ REL::RelocationID(75511, 77303) };
			static REL::Relocation<CreateUav*> createUav{ REL::RelocationID(75533, 77330) };
			static REL::Relocation<MakeSourceTexture*> makeSourceTexture{ REL::RelocationID(69335, 70717) };
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return;
			const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(size) * size * 4, 0);
			auto* texture = createTexture(renderer, size, size, zeros.data(), D3D11_USAGE_DEFAULT, DXGI_FORMAT_R8G8B8A8_UNORM, true);
			if (!texture) {
				logger::warn("[ORG] ImageSpace: the flow map could not be made; it is left for the engine to make");
				return;
			}
			createUav(renderer, texture, DXGI_FORMAT_R8G8B8A8_UNORM);
			const RE::BSFixedString name{ "FlowMap" };
			auto* source = makeSourceTexture(&name);
			if (!source)
				return;
			source->IncRefCount();
			source->rendererTexture = texture;
			FlowMapSlot() = source;
			logger::info("[ORG] ImageSpace: made the water's flow map ({0}x{0}) for the flow copies' pass", size);
		}

		/** @brief The flow map's texture as a 2D texture, or null. */
		ID3D11Texture2D* FlowMapTexture()
		{
			auto* map = FlowMap();
			return map && map->texture ? static_cast<ID3D11Texture2D*>(map->texture) : nullptr;
		}

		/** @brief The graph's import of a target as ShapeKey names it (a render target, a depth target, a cubemap or the flow map; any mip is its texture's). */
		std::shared_ptr<org::ExternalTextureResource> ImportOf(std::int32_t a_name)
		{
			if (a_name < 0 || (a_name & kLatchSourceInput))
				return nullptr;
			if (a_name & kFlowMapOutput) {
				auto* map = FlowMap();
				return map ? FrameAnchors::D3D11Texture("engine.flow-map", FlowMapTexture(), map->resourceView, map->UAV) : nullptr;
			}
			const auto index = IndexOfName(a_name);
			if (a_name & kDepthInput)
				return FrameAnchors::EngineDepth(index);
			if (a_name & kCubeInput)
				return FrameAnchors::EngineCubemap(index);
			return FrameAnchors::EngineTarget(index);
		}

		/**
		 * @brief The engine object an import of a_name is made from now: the texture (kFRAMEBUFFER's RTV, which the engine keeps alone),
		 * read from the renderer's tables without a D3D11 call. A change means the engine made the target again (a resize).
		 */
		const void* EngineObjectOf(std::int32_t a_name)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || a_name < 0 || (a_name & kLatchSourceInput))
				return nullptr;
			if (a_name & kFlowMapOutput)
				return FlowMapTexture();
			const auto index = IndexOfName(a_name);
			if (a_name & kDepthInput)
				return index < RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL ? renderer->GetDepthStencilData().depthStencils[index].texture : nullptr;
			if (a_name & kCubeInput)
				return index < RE::RENDER_TARGETS_CUBEMAP::kTOTAL ? renderer->GetRendererData().cubemapRenderTargets[index].texture : nullptr;
			if (index >= RE::RENDER_TARGET::kTOTAL)
				return nullptr;
			const auto& target = renderer->GetRuntimeData().renderTargets[index];
			return target.texture ? static_cast<const void*>(target.texture) : static_cast<const void*>(target.RTV);
		}

		/** @brief The format of an output's render target view (the program's target format), from the target or the texture it is a mip of. */
		rhi::Format RtvFormatOf(std::int32_t a_name, const std::shared_ptr<org::ExternalTextureResource>& a_import)
		{
			const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			if (auto* rtv = targets[IndexOfName(a_name)].RTV) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				rtv->GetDesc(&view);
				return rhi::helpers::ToRHI(view.Format);
			}
			return a_import ? a_import->GetFormat() : rhi::Format::Unknown;
		}

		/**
		 * @brief Whether an input reads what the output writes: the same render target, unless both name different mips of it (the
		 * SAO camera-Z chain reads one level and writes the next).
		 */
		bool Overlaps(std::int32_t a_input, std::int32_t a_output)
		{
			if (a_input < 0 || a_output < 0 || (a_input & kKindBits) || (a_output & kKindBits) || IndexOfName(a_input) != IndexOfName(a_output))
				return false;
			const auto inputMip = MipOfName(a_input);
			const auto outputMip = MipOfName(a_output);
			return inputMip == AnchorPasses::kAllMips || outputMip == AnchorPasses::kAllMips || inputMip == outputMip;
		}

		/**
		 * @brief A shape's inputs from the shadow state's slots (the PS ones for a draw, the CS ones for a dispatch): false with a
		 * reason when one is not an engine target or is an output. A cube slot is the reflection cubemap CS binds itself.
		 */
		template <class AddressModes, class FilterModes>
		bool ReadInputs(const EffectInfo& a_effect, ID3D11ShaderResourceView* const* a_views, const AddressModes* a_address, const FilterModes* a_filter,
			ShapeKey& a_out, std::string_view& a_refusal)
		{
			for (std::uint32_t slot = 0; slot < a_effect.slots; ++slot) {
				if (!(a_effect.reads & (1u << slot)))
					continue;
				if (a_effect.cubes & (1u << slot)) {
					a_out.inputs[slot] = kCubeInput | static_cast<std::int32_t>(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS);
					continue;
				}
				const auto input = InputOfView(a_views[slot]);
				if (input < 0) {
					a_refusal = "an input that is not an engine target";
					return false;
				}
				if (Overlaps(input, a_out.output) || Overlaps(input, a_out.output1)) {
					a_refusal = "it reads its own output";
					return false;
				}
				a_out.inputs[slot] = input;
				if (slot < a_effect.samplers) {
					a_out.address[slot] = a_address[slot].underlying();
					a_out.filter[slot] = a_filter[slot].underlying();
				}
			}
			return true;
		}

		/** @brief Whether a_view is the flow map's UAV (either may be a tripwire handle). */
		bool IsFlowMapUav(const ID3D11UnorderedAccessView* a_view)
		{
			const auto* map = FlowMap();
			return a_view && map && map->UAV && Tripwire::Unwrap(a_view) == Tripwire::Unwrap(static_cast<const ID3D11UnorderedAccessView*>(map->UAV));
		}

		/** @brief Whether the shadow state's compute output is the flow map now (a flow copy is about to dispatch). */
		bool FlowCopyBound() { return IsFlowMapUav(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().CSUAV[0]); }

		/**
		 * @brief A dispatch's shape from the shadow state's compute slots, as SetDirtyStates(true) is about to apply them. A flow copy
		 * (ISCopySubRegionCS into the flow map) reads a loaded texture, a different one each call: its input is kLatchSourceInput, and
		 * a_source gets the view, whose heap index the dispatch's latch then holds.
		 */
		bool ReadComputeShape(std::size_t a_effect, ShapeKey& a_out, std::string_view& a_refusal, ID3D11ShaderResourceView*& a_source)
		{
			const auto& effect = kEffects[a_effect];
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			a_out.effect = static_cast<std::uint32_t>(a_effect);
			a_out.compute = true;
			a_source = nullptr;
			for (std::size_t i = 1; i < std::size(shadow.CSUAV); ++i)
				if (shadow.CSUAV[i]) {
					a_refusal = "more than one UAV";
					return false;
				}
			if (effect.id == Effects::ISCopySubRegionCS && IsFlowMapUav(shadow.CSUAV[0])) {
				a_source = shadow.CSTexture[0];
				if (!a_source) {
					a_refusal = "a flow copy without a source";
					return false;
				}
				a_out.output = kFlowMapOutput;
				a_out.inputs[0] = kLatchSourceInput;
				return true;
			}
			a_out.output = TargetOfUav(shadow.CSUAV[0]);
			if (a_out.output < 0) {
				a_refusal = "its UAV is not an engine render target's";
				return false;
			}
			return ReadInputs(effect, shadow.CSTexture, shadow.CSTextureAddressMode, shadow.CSTextureFilterMode, a_out, a_refusal);
		}

		/** @brief A draw's output: its name (with the mip it writes) and that mip's size. */
		struct OutputView
		{
			std::int32_t name = -1;
			std::uint32_t width = 0, height = 0;
		};

		/**
		 * @brief The output of a draw into engine render target a_target. A target with a texture is itself. kFRAMEBUFFER keeps only its
		 * RTV: it is itself, its size the RTV's texture's (FrameAnchors::EngineTarget imports that texture). A target with only an RTV
		 * of one mip of another target's texture (the SAO camera-Z levels) is that texture's target at that mip: the pass draws through
		 * that texture's import, so the owner must have an RTV, and the mip's view the import's formats.
		 */
		bool ResolveOutput(std::int32_t a_target, OutputView& a_out, std::string_view& a_refusal)
		{
			const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			const auto& target = targets[a_target];
			ID3D11Texture2D* texture = target.texture;
			winrt::com_ptr<ID3D11Texture2D> viewTexture;
			std::uint32_t mip = 0;
			a_out.name = a_target;
			if (!texture) {
				if (!target.RTV) {
					a_refusal = "its output has no texture";
					return false;
				}
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				target.RTV->GetDesc(&view);
				winrt::com_ptr<ID3D11Resource> resource;
				target.RTV->GetResource(resource.put());
				if (a_target == static_cast<std::int32_t>(RE::RENDER_TARGET::kFRAMEBUFFER)) {
					if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0 || !resource) {
						a_refusal = "its output is kFRAMEBUFFER through a view other than the whole back buffer";
						return false;
					}
					viewTexture = resource.try_as<ID3D11Texture2D>();
					texture = viewTexture.get();
					if (!texture) {
						a_refusal = "its output is kFRAMEBUFFER without a texture";
						return false;
					}
				} else {
					const auto owner = TargetOfTexture(resource.get());
					if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice > kMaxMip || owner < 0) {
						a_refusal = "its output has no texture and is no mip of an engine target's";
						return false;
					}
					const auto& ownerTarget = targets[owner];
					if (!ownerTarget.RTV) {
						a_refusal = "its output is a mip of a texture without a render target view";
						return false;
					}
					// The import's format is its SRV's (the program's target format), its RTVs the owner RTV's format.
					D3D11_RENDER_TARGET_VIEW_DESC ownerView{};
					ownerTarget.RTV->GetDesc(&ownerView);
					D3D11_TEXTURE2D_DESC ownerDesc{};
					ownerTarget.texture->GetDesc(&ownerDesc);
					DXGI_FORMAT importFormat = ownerDesc.Format;
					if (ownerTarget.SRV) {
						D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
						ownerTarget.SRV->GetDesc(&srv);
						importFormat = srv.Format;
					}
					if (view.Format != ownerView.Format || view.Format != importFormat) {
						a_refusal = "its output is a mip view in another format than its texture's";
						return false;
					}
					texture = ownerTarget.texture;
					mip = view.Texture2D.MipSlice;
					a_out.name = WithMip(owner, mip);
				}
			}
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			if (mip >= desc.MipLevels) {
				a_refusal = "its output is a mip its texture does not have";
				return false;
			}
			a_out.width = (std::max)(desc.Width >> mip, 1u);
			a_out.height = (std::max)(desc.Height >> mip, 1u);
			return true;
		}

		/**
		 * @brief The viewport the engine's draw uses, from the shadow state (RendererShadowState::viewPort, +0x70: what SetDirtyStates
		 * applies): the whole target (Render's UpdateViewPort), the caller's (the explicit-target Render), the one a custom-viewport
		 * effect keeps (ImageSpaceEffect::unk88: Render leaves the viewport as it is), or the dynamic-resolution part of the target. Never
		 * the D3D11 context's: under the flush latch it is stale. The pass draws it as its render area, so it must be whole pixels inside
		 * the output. False with a reason (and the viewport as a_detail).
		 */
		bool ReadViewport(const OutputView& a_output, ShapeKey& a_key, std::string_view& a_refusal, std::string& a_detail)
		{
			const auto& viewport = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().viewPort;
			a_detail = fmt::format("{}x{} at ({}, {}) in {}x{}, depth {} to {}", viewport.Width, viewport.Height, viewport.TopLeftX, viewport.TopLeftY, a_output.width,
				a_output.height, viewport.MinDepth, viewport.MaxDepth);
			// The dynamic-resolution viewport's depth range is a float product too (0 to 0.99998 seen). A taken draw has depth testing and
			// writes off (ReadShape), so a range within 1e-3 of 0 to 1 writes the same pixels.
			if (std::abs(viewport.MinDepth) > 1e-3f || std::abs(viewport.MaxDepth - 1.0f) > 1e-3f) {
				a_refusal = "a viewport depth range other than 0 to 1";
				return false;
			}
			// The dynamic-resolution sizes are float products. Within 1/64 of a whole pixel, the pixels covered are the same and
			// TEXCOORD0 differs by far less than a texel.
			const std::array values{ viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height };
			std::array<std::uint32_t, 4> rect{};
			for (std::size_t i = 0; i < values.size(); ++i) {
				const float rounded = std::round(values[i]);
				if (!(rounded >= 0.0f) || std::abs(values[i] - rounded) > 1.0f / 64.0f) {
					a_refusal = "a viewport that is not whole pixels";
					return false;
				}
				rect[i] = static_cast<std::uint32_t>(rounded);
			}
			if (rect[2] == 0 || rect[3] == 0) {
				a_refusal = "an empty viewport";
				return false;
			}
			if (rect[0] + rect[2] > a_output.width || rect[1] + rect[3] > a_output.height) {
				a_refusal = "a viewport outside its target";
				return false;
			}
			a_key.viewport = rect;
			return true;
		}

		/** @brief Whether a render target index is one of the engine's table (kFRAMEBUFFER included). */
		bool IsEngineTarget(std::int32_t a_target)
		{
			return a_target >= static_cast<std::int32_t>(RE::RENDER_TARGET::kFRAMEBUFFER) && a_target < static_cast<std::int32_t>(RE::RENDER_TARGET::kTOTAL);
		}

		/**
		 * @brief The draw's shape from the shadow state, as SetDirtyStates is about to apply it (all but the viewport: ReadViewport
		 * reads it once SetDirtyStates ran); false with a reason when no pass can draw it.
		 */
		bool ReadShape(std::size_t a_effect, ShapeKey& a_out, OutputView& a_output, std::string_view& a_refusal)
		{
			const auto& effect = kEffects[a_effect];
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			a_out.effect = static_cast<std::uint32_t>(a_effect);
			if (!globals::game::renderer) {
				a_refusal = "no renderer";
				return false;
			}
			const auto output = static_cast<std::int32_t>(shadow.renderTargets[0]);
			if (!IsEngineTarget(output)) {
				a_refusal = "its output is not an engine render target";
				return false;
			}
			// SV_Target1 for an effect that writes two (ISWaterBlend): an engine target with a texture of its own, not the first.
			if (effect.targets > 1) {
				const auto second = static_cast<std::int32_t>(shadow.renderTargets[1]);
				if (!IsEngineTarget(second) || second == output || !globals::game::renderer->GetRuntimeData().renderTargets[second].texture) {
					a_refusal = "its second output is not an engine render target with a texture";
					return false;
				}
				a_out.output1 = second;
			}
			for (std::size_t i = effect.targets; i < std::size(shadow.renderTargets); ++i)
				if (static_cast<std::int32_t>(shadow.renderTargets[i]) != static_cast<std::int32_t>(RE::RENDER_TARGET::kNONE)) {
					a_refusal = "more render targets than its shader writes";
					return false;
				}
			// The engine leaves a depth target bound through the chain with the test off: the pass binds none then, and the draw is the same.
			if (shadow.depthStencil != UINT32_MAX &&
				(shadow.depthStencilDepthMode != RE::BSGraphics::DepthStencilDepthMode::kDisabled || shadow.depthStencilStencilMode != 0)) {
				a_refusal = "a depth or stencil test";
				return false;
			}
			// A clear is applied by SetDirtyStates when the target is bound; the pending batch would run after it.
			constexpr std::uint32_t kDirtyRenderTarget = 0x1;
			if ((shadow.stateUpdateFlags.underlying() & kDirtyRenderTarget) && shadow.setRenderTargetMode[0] <= RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR_STENCIL) {
				a_refusal = "its target is cleared at the bind";
				return false;
			}
			if (!ResolveOutput(output, a_output, a_refusal))
				return false;
			if (shadow.rasterStateScissorMode != 0) {
				a_refusal = "a scissor rectangle";
				return false;
			}
			a_out.output = a_output.name;
			if (!ReadInputs(effect, shadow.PSTexture, shadow.PSTextureAddressMode, shadow.PSTextureFilterMode, a_out, a_refusal))
				return false;
			const auto draw = Pipeline::EngineDrawState::Latch();
			a_out.blend = PackBlend(draw.blendMode, draw.alphaToCoverage, draw.writeMode, draw.blendExtra);
			if (D3D11_BLEND_DESC desc{}; !BlendDescOf(a_out.blend, desc)) {
				a_refusal = "a blend state outside the engine's table";
				return false;
			}
			return true;
		}

		std::uint32_t SamplerFor(std::uint32_t a_address, std::uint32_t a_filter)
		{
			auto& state = IS();
			const auto key = std::make_pair(a_address, a_filter);
			if (const auto it = state.samplers.find(key); it != state.samplers.end())
				return it->second ? it->second.Index() : UINT32_MAX;
			// The engine fills its sampler table at its renderer's start, after CS's Setup: an entry missing now is asked for again later.
			auto* sampler = Pipeline::EngineSamplerState(a_address, a_filter);
			if (!sampler)
				return UINT32_MAX;
			D3D11_SAMPLER_DESC desc{};
			sampler->GetDesc(&desc);
			org::OwnedDescriptorBinding binding = ShaderBindings::CreateSamplerFromD3D11(desc);
			state.samplers.emplace(key, binding);
			return binding ? binding.Index() : UINT32_MAX;
		}

		bool BlendDescOf(std::uint32_t a_blend, D3D11_BLEND_DESC& a_out)
		{
			auto& state = IS();
			if (auto* object = BlendStateOf(a_blend)) {
				object->GetDesc(&a_out);
				state.blendDescs[a_blend] = a_out;
				return true;
			}
			const auto kept = state.blendDescs.find(a_blend);
			if (kept == state.blendDescs.end())
				return false;
			a_out = kept->second;
			return true;
		}

		/** @brief A blend description as the manifest keeps it: the two flags, then each target's eight fields. */
		json BlendToJson(const D3D11_BLEND_DESC& a_desc)
		{
			json out = json::array({ static_cast<std::int64_t>(a_desc.AlphaToCoverageEnable), static_cast<std::int64_t>(a_desc.IndependentBlendEnable) });
			for (const auto& target : a_desc.RenderTarget) {
				out.push_back(static_cast<std::int64_t>(target.BlendEnable));
				out.push_back(static_cast<std::int64_t>(target.SrcBlend));
				out.push_back(static_cast<std::int64_t>(target.DestBlend));
				out.push_back(static_cast<std::int64_t>(target.BlendOp));
				out.push_back(static_cast<std::int64_t>(target.SrcBlendAlpha));
				out.push_back(static_cast<std::int64_t>(target.DestBlendAlpha));
				out.push_back(static_cast<std::int64_t>(target.BlendOpAlpha));
				out.push_back(static_cast<std::int64_t>(target.RenderTargetWriteMask));
			}
			return out;
		}

		/** @brief The manifest's form of a blend description read back; false for a malformed one. */
		bool BlendFromJson(const json& a_item, D3D11_BLEND_DESC& a_out)
		{
			if (!a_item.is_array() || a_item.size() != 2 + 8 * std::size(a_out.RenderTarget))
				return false;
			try {
				a_out.AlphaToCoverageEnable = a_item[0].get<std::int64_t>() != 0;
				a_out.IndependentBlendEnable = a_item[1].get<std::int64_t>() != 0;
				std::size_t at = 2;
				for (auto& target : a_out.RenderTarget) {
					target.BlendEnable = a_item[at++].get<std::int64_t>() != 0;
					target.SrcBlend = static_cast<D3D11_BLEND>(a_item[at++].get<std::int64_t>());
					target.DestBlend = static_cast<D3D11_BLEND>(a_item[at++].get<std::int64_t>());
					target.BlendOp = static_cast<D3D11_BLEND_OP>(a_item[at++].get<std::int64_t>());
					target.SrcBlendAlpha = static_cast<D3D11_BLEND>(a_item[at++].get<std::int64_t>());
					target.DestBlendAlpha = static_cast<D3D11_BLEND>(a_item[at++].get<std::int64_t>());
					target.BlendOpAlpha = static_cast<D3D11_BLEND_OP>(a_item[at++].get<std::int64_t>());
					target.RenderTargetWriteMask = static_cast<UINT8>(a_item[at++].get<std::int64_t>());
				}
			} catch (const std::exception&) {
				return false;
			}
			return true;
		}

		/** @brief The ORG form of a packed engine blend state for a_targets attachments: false with a reason when it has none. */
		bool BlendFor(std::uint32_t a_blend, std::uint32_t a_targets, std::array<rhi::BlendAttachment, 2>& a_out, std::string& a_error)
		{
			D3D11_BLEND_DESC desc{};
			if (!BlendDescOf(a_blend, desc)) {
				a_error = "no engine blend state";
				return false;
			}
			rhi::BlendState blend{};
			if (!Pipeline::BlendFromD3D11(desc, blend, a_error) || Pipeline::ReadsBlendFactor(desc) || Pipeline::UsesDualSource(desc)) {
				if (a_error.empty())
					a_error = "a blend factor or dual-source blending";
				return false;
			}
			a_out[0] = blend.attachments[0];
			// D3D11 blends every target as the first unless IndependentBlendEnable.
			a_out[1] = a_targets > 1 && desc.IndependentBlendEnable ? blend.attachments[1] : blend.attachments[0];
			return true;
		}

		/** @brief The effect's macros and CS's feature defines, as ShaderCache composes them for its D3D11 build (without the stage define). */
		std::vector<std::pair<std::string, std::string>> LiveDefinesOf(std::size_t a_effect)
		{
			std::vector<std::pair<std::string, std::string>> defines;
			const auto* shader = ShaderOf(a_effect);
			if (!shader)
				return defines;
			const auto shaderClass = kEffects[a_effect].compute ? SIE::ShaderClass::Compute : SIE::ShaderClass::Pixel;
			for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(*shader, shaderClass, 0)) {
				if (name == "VSHADER" || name == "PSHADER" || name == "CSHADER")
					continue;
				defines.emplace_back(name, value);
			}
			return defines;
		}

		/** @brief Whether a_effect's program defines are known now: its engine effect object exists, or the manifest saved them. */
		bool HasDefines(std::size_t a_effect) { return ShaderOf(a_effect) || IS().savedDefines.contains(a_effect); }

		/**
		 * @brief The effect's program defines: composed now from its engine effect object, else as the manifest saved them (an effect
		 * the manager makes after the graph's first build).
		 */
		std::vector<std::pair<std::string, std::string>> NarrowDefinesOf(std::size_t a_effect)
		{
			auto& state = IS();
			auto narrow = LiveDefinesOf(a_effect);
			if (!ShaderOf(a_effect))
				if (const auto saved = state.savedDefines.find(a_effect); saved != state.savedDefines.end())
					narrow = saved->second;
			return narrow;
		}

		/** @brief NarrowDefinesOf as the program builds take them. */
		std::vector<std::pair<std::wstring, std::wstring>> DefinesOf(std::size_t a_effect) { return AnchorPasses::Defines(NarrowDefinesOf(a_effect)); }

		/**
		 * @brief The heap index of the no-fog volume (ImageSpaceState::noFogView), made on first use; UINT32_MAX when it could not be
		 * made. Render thread, at a graph build.
		 */
		std::uint32_t NoFogVolume()
		{
			auto& state = IS();
			if (state.noFogTried)
				return state.noFogIndex;
			state.noFogTried = true;
			auto* device = globals::d3d::device;
			if (!device)
				return UINT32_MAX;
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = desc.Height = desc.Depth = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			const std::array<float, 4> texel{ 0.0f, 0.0f, 0.0f, 1.0f };
			D3D11_SUBRESOURCE_DATA data{ .pSysMem = texel.data(), .SysMemPitch = sizeof(texel), .SysMemSlicePitch = sizeof(texel) };
			winrt::com_ptr<ID3D11Texture3D> texture;
			if (FAILED(device->CreateTexture3D(&desc, &data, texture.put())) ||
				FAILED(device->CreateShaderResourceView(texture.get(), nullptr, state.noFogView.put()))) {
				logger::warn("[ORG] ImageSpace: the no-fog volume could not be made");
				return UINT32_MAX;
			}
			Util::SetResourceName(texture.get(), "ImageSpace::NoFogVolume");
			Util::SetResourceName(state.noFogView.get(), "ImageSpace::NoFogVolume SRV");
			constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::FrameSlot, 0);
			auto binding = DCLF::GpuTextures::Get().ResolveBinding(state.noFogView.get(), kSourceTag);
			if (binding.index == DCLF::GpuTextures::kInvalid) {
				logger::warn("[ORG] ImageSpace: the no-fog volume has no heap index");
				return UINT32_MAX;
			}
			state.noFogIndex = binding.index;
			state.noFogOwner = std::move(binding.owner);
			return state.noFogIndex;
		}

		/**
		 * @brief The feature handles of an effect that reads CS's feature headers (EffectInfo::features), in its resource list's order
		 * after its samplers: s9, IBL's environment and sky SH, Exponential Height Fog's near and far volumes, Dynamic Cubemaps'
		 * environment. A header the program's defines do not include is unread (handle 0). An included one reads its feature's
		 * published output, so the pass is ordered after the feature's writes; false with a_missing when that feature is not on the
		 * graph (its pass is left out then: the D3D11 view is not the graph's).
		 */
		bool FeatureBinds(std::size_t a_effect, std::vector<AnchorPasses::Bind>& a_out, std::string& a_missing)
		{
			auto& state = IS();
			const auto defines = NarrowDefinesOf(a_effect);
			const auto has = [&](std::string_view a_name) { return std::ranges::any_of(defines, [&](const auto& a_define) { return a_define.first == a_name; }); };
			const bool fog = has("EXP_HEIGHT_FOG") && has("APPLY_FOG");
			// s9: the sampler the fog header names. CS binds nothing there for this draw on D3D11, which reads D3D11's default sampler
			// (linear, clamped) unless an earlier draw left one: the default here.
			if (!state.defaultSampler) {
				D3D11_SAMPLER_DESC desc{};
				desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.MaxAnisotropy = 1;
				desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
				desc.MinLOD = -FLT_MAX;
				desc.MaxLOD = FLT_MAX;
				state.defaultSampler = ShaderBindings::CreateSamplerFromD3D11(desc);
			}
			a_out.push_back(fog && state.defaultSampler ? AnchorPasses::Bind::Handle(state.defaultSampler.Index()) : AnchorPasses::Bind::Unused());
			const auto read = [&](bool a_included, std::string_view a_id, bool a_fogVolume) {
				if (!a_included) {
					a_out.push_back(AnchorPasses::Bind::Unused());
					return true;
				}
				// IsDeclarable notes an id it says no to: the id's publish builds every graph again, and this pass then reads it.
				if (PublishedResources::IsDeclarable(a_id)) {
					a_out.push_back(AnchorPasses::Bind::Published(a_id));
					return true;
				}
				// The fog publishes its volumes at its first graph frame, and never while its volumetric fog is off: until then the pass
				// reads the no-fog volume, as D3D11 reads a null view.
				if (a_fogVolume) {
					if (const auto index = NoFogVolume(); index != UINT32_MAX) {
						a_out.push_back(AnchorPasses::Bind::Handle(index));
						return true;
					}
				}
				a_missing = std::string(a_id);
				return false;
			};
			return read(has("IBL"), ORGIBL::kEnvId, false) && read(has("IBL"), ORGIBL::kSkyId, false) && read(fog, ORGExponentialHeightFog::kNearIntegratedId, true) &&
			       read(fog, ORGExponentialHeightFog::kFarIntegratedId, true) && read(fog && has("DYNAMIC_CUBEMAPS"), ORGDynamicCubemaps::kEnvBC6HId, false);
		}

		/** @brief The raster program's description for a key (the blend was checked by BlendFor). Needs the engine's effect object. */
		RasterProgram::Desc DescribeRaster(const ProgramKey& a_key)
		{
			const auto& [effectIndex, format, format1, blendBits] = a_key;
			const auto& effect = kEffects[effectIndex];
			std::array<rhi::BlendAttachment, 2> blend{};
			std::string error;
			BlendFor(blendBits, effect.targets, blend, error);
			RasterProgram::Desc desc{};
			desc.pixelSource = effect.source;
			// A geometry effect's vertex stage is its source's ORG build, which pulls the geometry (TakeDraw writes where it is).
			if (effect.geometry)
				desc.vertexSource = effect.source;
			// FXC-era sources (vector conditions in ?:).
			desc.languageVersion = L"2018";
			// Shared (4 words), the SRVs, the samplers, the feature handles, the latch (2).
			desc.constantWords = 4 + effect.slots + effect.samplers + (effect.features ? kFeatureWords : 0) + 2;
			// The stage defines are RasterProgram's own.
			desc.defines = DefinesOf(effectIndex);
			desc.targets = { { .format = format, .blend = blend[0] } };
			if (format1 != rhi::Format::Unknown)
				desc.targets.push_back({ .format = format1, .blend = blend[1] });
			return desc;
		}

		/** @brief The compute program's description (BSImagespaceShader::DispatchComputeShader dispatches technique 0, 100952 / 107734). */
		ComputeProgram::Desc DescribeCompute(std::size_t a_effect)
		{
			const auto& effect = kEffects[a_effect];
			ComputeProgram::Desc desc{};
			desc.source = effect.source;
			// Shared (4 words), the SRVs, the samplers, the UAV, the latch (2).
			desc.constantWords = 4 + effect.slots + effect.samplers + 1 + 2;
			// The engine's macros (GRIDSIZE, TEXTAP, ...) and CS's feature defines.
			desc.defines = DefinesOf(a_effect);
			return desc;
		}

		/** @brief A spec's raster program key. */
		ProgramKey KeyOf(const Spec& a_spec) { return { a_spec.key.effect, a_spec.format, a_spec.format1, a_spec.key.blend }; }

		/**
		 * @brief Queues the program of every registered pass on the program build thread once its defines (the engine's effect object or
		 * the manifest's) and its blend state (the engine's table, filled after Setup) are known: at Setup, then once a frame until all are.
		 */
		void QueuePrograms()
		{
			auto& state = IS();
			if (state.programsQueued.size() == state.specs.size())
				return;
			for (const auto& spec : state.specs) {
				if (state.programsQueued.contains(spec.get()) || !HasDefines(spec->key.effect))
					continue;
				std::array<rhi::BlendAttachment, 2> blend{};
				std::string error;
				if (!spec->key.compute && !BlendFor(spec->key.blend, kEffects[spec->key.effect].targets, blend, error))
					continue;
				state.programsQueued.insert(spec.get());
				if (spec->key.compute)
					ComputePrograms().Get(spec->key.effect, [&] { return DescribeCompute(spec->key.effect); });
				else
					Programs().Get(KeyOf(*spec), [&] { return DescribeRaster(KeyOf(*spec)); });
			}
		}

		/**
		 * @brief The graph pass of a spec, made at a build of the anchor's graph (render thread): its program taken now (built, waited
		 * for, or built here and logged), then the pass AnchorPasses makes for its draw or dispatch. Null leaves it out of that build
		 * (logged): its shape's draws are refused by the commit's caller, since the gate entry stays closed and CanRun still holds.
		 */
		std::shared_ptr<org::RenderPass> MakePass(const std::shared_ptr<const Spec>& a_spec, const std::shared_ptr<const org::LatchBlock>& a_latch,
			const std::shared_ptr<const RunGate>& a_gate)
		{
			const auto& spec = *a_spec;
			const auto& effect = kEffects[spec.key.effect];
			auto& state = IS();
			if (!HasDefines(spec.key.effect)) {
				logger::info("[ORG] ImageSpace: {}: the engine has not made its effect yet and the manifest has no defines for it; its pass joins the next session",
					spec.name);
				return nullptr;
			}
			// The engine's blend and sampler tables are read here, at the graph's build, not at Setup (they are filled after it).
			if (!spec.key.compute) {
				std::array<rhi::BlendAttachment, 2> blend{};
				std::string error;
				if (!BlendFor(spec.key.blend, effect.targets, blend, error)) {
					if (D3D11_BLEND_DESC desc{}; !BlendDescOf(spec.key.blend, desc))
						state.retryBuild = true;
					logger::info("[ORG] ImageSpace: {}: its blend state has no ORG form at the graph's build ({}); its pass is left out", spec.name, error);
					return nullptr;
				}
			}
			std::array<std::uint32_t, kMaxSlots> samplers{};
			for (std::uint32_t slot = 0; slot < effect.samplers; ++slot) {
				if (!spec.inputs[slot] || (effect.cubes & (1u << slot)))
					continue;
				samplers[slot] = SamplerFor(spec.key.address[slot], spec.key.filter[slot]);
				if (samplers[slot] == UINT32_MAX) {
					if (!Pipeline::EngineSamplerState(spec.key.address[slot], spec.key.filter[slot]))
						state.retryBuild = true;
					logger::info("[ORG] ImageSpace: {}: sampler {} (address {}, filter {}) has no ORG form at the graph's build; its pass is left out", spec.name,
						slot, spec.key.address[slot], spec.key.filter[slot]);
					return nullptr;
				}
			}
			bool builtHere = false;
			std::shared_ptr<const RasterProgram> raster;
			std::shared_ptr<const ComputeProgram> compute;
			if (spec.key.compute)
				compute = ComputePrograms().GetNow(spec.key.effect, [&] { return DescribeCompute(spec.key.effect); }, &builtHere);
			else
				raster = Programs().GetNow(KeyOf(spec), [&] { return DescribeRaster(KeyOf(spec)); }, &builtHere);
			if (builtHere)
				logger::info("[ORG] ImageSpace: built the program of {} on the render thread at the graph's build (Setup did not queue it)", spec.name);
			if (!raster && !compute) {
				logger::error("[ORG] ImageSpace: {} has no program; its pass is left out of the graph", spec.name);
				return nullptr;
			}
			// The shader's resource list order: the shared constants, t0.., s0.., (u0), the constants' location.
			std::vector<AnchorPasses::Bind> bindings;
			bindings.push_back(AnchorPasses::Bind::Shared());
			for (std::uint32_t slot = 0; slot < effect.slots; ++slot)
				bindings.push_back(spec.inputs[slot] ? AnchorPasses::Bind::Srv(spec.inputs[slot], MipOfName(spec.key.inputs[slot])) : AnchorPasses::Bind::Unused());
			for (std::uint32_t slot = 0; slot < effect.samplers; ++slot)
				bindings.push_back(spec.inputs[slot] && !(effect.cubes & (1u << slot)) ? AnchorPasses::Bind::Handle(samplers[slot]) : AnchorPasses::Bind::Unused());
			if (effect.features) {
				std::string missing;
				if (!FeatureBinds(spec.key.effect, bindings, missing)) {
					logger::info("[ORG] ImageSpace: {}: {} is not published (its feature's ORG path is off or has not run yet); its pass is left out until it is", spec.name, missing);
					return nullptr;
				}
			}
			if (effect.compute)
				bindings.push_back(AnchorPasses::Bind::Uav(spec.output));
			bindings.push_back(AnchorPasses::Bind::Latch(spec.entry * static_cast<std::uint32_t>(sizeof(ConstantsAt))));
			AnchorPasses::Builder builder(Anchor::ImageSpace, kClientId, static_cast<std::int32_t>(spec.entry), a_latch, a_gate);
			if (effect.compute) {
				AnchorPasses::Dispatch dispatch{};
				dispatch.name = spec.name;
				dispatch.program = compute;
				dispatch.bindings = std::move(bindings);
				dispatch.gate = spec.entry;
				builder.Add(std::move(dispatch));
			} else {
				AnchorPasses::Draw draw{};
				draw.name = spec.name;
				draw.program = raster;
				draw.bindings = std::move(bindings);
				draw.target = spec.output;
				draw.mip = MipOfName(spec.key.output);
				if (spec.output1)
					draw.extraTargets.push_back(spec.output1);
				// The engine's viewport (ReadViewport): the whole target, or a part of it (dynamic resolution, a custom viewport).
				const auto& viewport = spec.key.viewport;
				draw.viewport = { .x = viewport[0], .y = viewport[1], .width = viewport[2], .height = viewport[3] };
				draw.load = rhi::LoadOp::Load;
				draw.gate = spec.entry;
				builder.Add(std::move(draw));
			}
			auto passes = builder.Take();
			if (builder.Failed() || passes.empty() || !passes.front().make) {
				logger::error("[ORG] ImageSpace: {} could not be made ({}); its pass is left out of the graph", spec.name, builder.Failure());
				return nullptr;
			}
			return passes.front().make();
		}

		/** @brief MakePass, noting whether the build holds the spec's pass (AdmitShape refuses the shape's draws when it does not). */
		std::shared_ptr<org::RenderPass> MakeNotedPass(const std::shared_ptr<const Spec>& a_spec, const std::shared_ptr<const org::LatchBlock>& a_latch,
			const std::shared_ptr<const RunGate>& a_gate)
		{
			auto pass = MakePass(a_spec, a_latch, a_gate);
			if (a_spec->entry < kGateEntries)
				IS().passMade[a_spec->entry] = pass != nullptr;
			return pass;
		}

		/**
		 * @brief The pass for a shape, made at Setup (its textures imported), or none (logged once). Its samplers, blend state and program
		 * are taken when the graph is built (MakePass): the engine fills its state tables after Setup.
		 */
		std::shared_ptr<const Spec> MakeSpec(const ShapeKey& a_key, std::uint32_t a_entry)
		{
			auto& state = IS();
			const auto& effect = kEffects[a_key.effect];
			auto spec = std::make_shared<Spec>();
			spec->key = a_key;
			spec->entry = a_entry;
			spec->output = ImportOf(ImportOfName(a_key.output));
			spec->output1 = ImportOf(a_key.output1);
			std::string_view refusal;
			if (!spec->output || (a_key.output1 >= 0 && !spec->output1))
				refusal = "its output could not be imported";
			else if (!a_key.compute) {
				// The viewport was read against the target's size when the shape was seen; this session's import must hold it.
				const auto mip = MipOfName(a_key.output);
				const auto shift = mip == AnchorPasses::kAllMips ? 0u : mip;
				const auto width = (std::max)(spec->output->GetWidth() >> shift, 1u);
				const auto height = (std::max)(spec->output->GetHeight() >> shift, 1u);
				if (a_key.viewport[0] + a_key.viewport[2] > width || a_key.viewport[1] + a_key.viewport[3] > height)
					refusal = "its viewport is outside its imported output (another resolution)";
				spec->format = RtvFormatOf(a_key.output, spec->output);
				if (spec->output1)
					spec->format1 = RtvFormatOf(a_key.output1, spec->output1);
			}
			for (std::uint32_t slot = 0; refusal.empty() && slot < effect.slots; ++slot) {
				// A latched source has no import: the shader reads it by the heap index in the draw's latch.
				if (!(effect.reads & (1u << slot)) || (a_key.inputs[slot] >= 0 && (a_key.inputs[slot] & kLatchSourceInput)))
					continue;
				spec->inputs[slot] = ImportOf(a_key.inputs[slot]);
				if (!spec->inputs[slot])
					refusal = "an input could not be imported";
			}
			if (!refusal.empty()) {
				if (state.refusedShapes.insert(a_key).second)
					logger::info("[ORG] ImageSpace: {} into {} has no pass this session ({})", effect.effectName, NameForLog(a_key.output), refusal);
				return nullptr;
			}
			spec->name = fmt::format("{}.{}.{}", effect.effectName, NameForLog(a_key.output), spec->entry);
			return spec;
		}

		/** @brief The registration's pass for a shape, or null. */
		const Spec* SpecFor(const ShapeKey& a_key)
		{
			const auto& specs = IS().specOf;
			const auto found = specs.find(a_key);
			return found != specs.end() ? found->second : nullptr;
		}

		/** @brief a_key with every mention of one target of a_pair replaced by the other. */
		ShapeKey Swapped(const ShapeKey& a_key, const std::pair<std::int32_t, std::int32_t>& a_pair)
		{
			const auto swap = [&](std::int32_t a_target) { return a_target == a_pair.first ? a_pair.second : a_target == a_pair.second ? a_pair.first :
				                                                                                                                         a_target; };
			ShapeKey out = a_key;
			out.output = swap(out.output);
			out.output1 = swap(out.output1);
			for (auto& input : out.inputs)
				input = swap(input);
			return out;
		}

		bool Mentions(const ShapeKey& a_key, std::int32_t a_target)
		{
			return a_key.output == a_target || a_key.output1 == a_target || std::ranges::find(a_key.inputs, a_target) != a_key.inputs.end();
		}

		/**
		 * @brief Whether a kEffects index belongs to a family whose members share shapes: the blurs of one kind (one shader per radius;
		 * the bloom radius and the blur composites pick one), and the tonemap with its fade form (the manager's fade picks one).
		 */
		bool SharesShapes(std::size_t a_effect)
		{
			const auto group = kEffects[a_effect].group;
			return group == "bloom" || group == "blur" || group == "nonhdrblur" || group == "tonemap";
		}

		/** @brief Whether two outputs of a_effect from the same inputs are a pair it alternates between (AddImpliedShapes). */
		bool PingPongs(std::size_t a_effect)
		{
			const auto group = kEffects[a_effect].group;
			return group != "iscopy" && group != "tonemap";
		}

		/**
		 * @brief The shapes the RE says the manifest's shapes imply, so they are passes before the engine first draws them:
		 * - the chain's source alternates between kMAIN and kMAIN_COPY (the effects before HDR copy back and forth);
		 * - the luminance chain's last level alternates by frame (+2 on odd frames, skeleton of HDR's Render in imagespace.md): two
		 *   outputs of one effect with the same inputs are such a pair, and every shape mentioning one gets the other;
		 * - the bloom radius picks one of seven bright-pass blur shaders (slot 79 + radius) with the same targets: a bloom shape
		 *   implies the other six, and so for the plain and non-HDR blurs (SharesShapes);
		 * - the manager's fade picks the tonemap or its fade form with the same targets: each implies the other;
		 * - an effect that keeps the viewport the effect before it left (EffectInfo::keepsViewport: refraction, the distant blur) draws
		 *   into a part of its target or the whole of it, by which effects ran before it: each shape implies the whole-target one, its
		 *   size from the engine's target table (with the swap, ISRefraction into kMAIN_COPY at the full size).
		 * a_positions gets each implied shape's place (its source's).
		 */
		void AddImpliedShapes(std::vector<ShapeKey>& a_keys, std::map<ShapeKey, std::uint32_t>& a_positions)
		{
			auto& state = IS();
			// Never more than kGateEntries shapes: the references the lambdas below hold into a_keys stay valid as they add to it.
			a_keys.reserve(kGateEntries);
			const auto addWhole = [&](const ShapeKey& a_key) {
				if (a_key.compute || !kEffects[a_key.effect].keepsViewport || a_key.output < 0 || (a_key.output & kKindBits) ||
					MipOfName(a_key.output) != AnchorPasses::kAllMips || !globals::game::renderer)
					return;
				OutputView output;
				std::string_view refusal;
				if (!ResolveOutput(a_key.output, output, refusal))
					return;
				ShapeKey whole = a_key;
				whole.viewport = { 0, 0, output.width, output.height };
				if (a_keys.size() < kGateEntries && std::ranges::find(a_keys, whole) == a_keys.end() && !state.refusedShapes.contains(whole)) {
					a_keys.push_back(whole);
					a_positions.emplace(whole, a_positions.contains(a_key) ? a_positions[a_key] : 0u);
				}
			};
			for (std::size_t i = 0, count = a_keys.size(); i < count; ++i)
				addWhole(a_keys[i]);
			std::set<std::pair<std::int32_t, std::int32_t>> swaps;
			for (std::size_t i = 0; i < a_keys.size(); ++i)
				for (std::size_t j = i + 1; j < a_keys.size(); ++j) {
					const auto& a = a_keys[i];
					const auto& b = a_keys[j];
					// The copies and the tonemap write one source into several targets by caller, not by frame: no swap pair.
					if (a.effect == b.effect && PingPongs(a.effect) && a.inputs == b.inputs && a.address == b.address && a.filter == b.filter && a.blend == b.blend &&
						a.output != b.output)
						swaps.insert(std::minmax(a.output, b.output));
				}
			swaps.insert({ static_cast<std::int32_t>(RE::RENDER_TARGET::kMAIN), static_cast<std::int32_t>(RE::RENDER_TARGET::kMAIN_COPY) });
			const auto add = [&](const ShapeKey& a_partner, const ShapeKey& a_from) {
				if (a_keys.size() >= kGateEntries || std::ranges::find(a_keys, a_partner) != a_keys.end() || state.refusedShapes.contains(a_partner))
					return;
				a_keys.push_back(a_partner);
				a_positions.emplace(a_partner, a_positions.contains(a_from) ? a_positions[a_from] : 0u);
			};
			// Each pair applied once to every shape: one partner per mention.
			const auto seen = a_keys.size();
			for (std::size_t i = 0; i < seen; ++i)
				for (const auto& pair : swaps)
					if (Mentions(a_keys[i], pair.first) || Mentions(a_keys[i], pair.second))
						add(Swapped(a_keys[i], pair), a_keys[i]);
			const auto withSwaps = a_keys.size();
			for (std::size_t i = 0; i < withSwaps; ++i) {
				if (!SharesShapes(a_keys[i].effect))
					continue;
				for (std::size_t other = 0; other < kEffects.size(); ++other) {
					if (kEffects[other].group != kEffects[a_keys[i].effect].group || !state.enabled[other] || other == a_keys[i].effect)
						continue;
					ShapeKey partner = a_keys[i];
					partner.effect = static_cast<std::uint32_t>(other);
					add(partner, a_keys[i]);
				}
			}
		}

		/**
		 * @brief Registers the client with a pass for every shape of a_keys (numbered from 0 by their place in the frame), replacing any
		 * earlier registration. Setup's, and again on the same shapes when the engine made its targets again. The anchor's graph is
		 * built with it at the next build point; FrameAnchors::CanRun says when.
		 */
		void RegisterClient(const std::vector<ShapeKey>& a_keys)
		{
			auto& state = IS();
			state.specs.clear();
			state.specOf.clear();
			state.importedObjects.clear();
			state.passMade.fill(false);
			state.programsQueued.clear();
			for (const auto& key : a_keys) {
				if (state.specs.size() >= kGateEntries)
					break;
				if (auto spec = MakeSpec(key, static_cast<std::uint32_t>(state.specs.size()))) {
					for (const auto name : { spec->key.output, spec->key.output1 })
						if (name >= 0)
							state.importedObjects[ImportOfName(name)] = EngineObjectOf(name);
					for (const auto name : spec->key.inputs)
						if (name >= 0)
							state.importedObjects[ImportOfName(name)] = EngineObjectOf(name);
					state.specOf.emplace(spec->key, spec.get());
					state.specs.push_back(std::move(spec));
				}
			}
			if (state.specs.empty()) {
				logger::info("[ORG] ImageSpace: no shape to register; this session's draws stay with the engine and fill the manifest");
				return;
			}
			std::vector<FrameAnchors::PassDesc> passes;
			for (const auto& spec : state.specs) {
				passes.push_back({ .name = fmt::format("{}.{}", kClientId, spec->name),
					.anchor = Anchor::ImageSpace,
					.order = static_cast<std::int32_t>(spec->entry),
					.kind = spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
					.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(state.latch), gate = std::shared_ptr<const RunGate>(state.gate)] {
						return MakeNotedPass(spec, latch, gate);
					},
					// A run opens a few of the shapes' gate entries: recorded after the commit, the closed ones record nothing.
					.atExecution = true });
			}
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			client.passes = std::move(passes);
			client.sharedConstants = true;
			client.gates = { state.gate };
			client.active = [] { return !IS().batch.empty(); };
			client.commit = [](const FrameAnchors::Point&) {
				auto& current = IS();
				const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
				// The sources this lease index's last run read by heap index are retired now: that run retired before the index was
				// opened again.
				if (current.heldSources.size() <= run.lease)
					current.heldSources.resize(static_cast<std::size_t>(run.lease) + 1);
				auto& held = current.heldSources[run.lease];
				held.clear();
				for (const auto& pending : current.batch) {
					if (pending.source)
						held.push_back(pending.source);
					current.latch->WriteValue(run, pending.entry * static_cast<std::uint32_t>(sizeof(ConstantsAt)), pending.at);
					if (pending.compute)
						current.gate->OpenDispatch(pending.entry, pending.groups[0], pending.groups[1], pending.groups[2]);
					else
						current.gate->OpenDraw(pending.entry, pending.vertices);
				}
			};
			client.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
			client.onFailed = [] {
				auto& current = IS();
				if (current.on) {
					current.on = false;
					logger::error("[ORG] ImageSpace: the ImageSpace anchor failed; every ImageSpace effect returns to the engine");
				}
			};
			if (!FrameAnchors::Register(std::move(client))) {
				Fail("the ImageSpace anchor refused the client");
				return;
			}
			++state.registrations;
			logger::info("[ORG] ImageSpace: registered {} passes (registration {}); the anchor's graph holds them from its next build", state.specs.size(),
				state.registrations);
		}

		/** @brief The manifest's file, beside the shader store (Util::kShaderStoreDirName: Data/ShaderCache keeps only that folder). */
		std::filesystem::path ManifestPath()
		{
			return std::filesystem::path(L"Data/ShaderCache") / Util::kShaderStoreDirName / L"ImageSpaceShapes.json";
		}

		json ToJson(const ShapeKey& a_key, const ManifestEntry& a_entry)
		{
			return json{ { "effect", kEffects[a_key.effect].effectName },
				{ "output", a_key.output },
				{ "output1", a_key.output1 },
				{ "inputs", a_key.inputs },
				{ "address", a_key.address },
				{ "filter", a_key.filter },
				{ "blend", a_key.blend },
				{ "compute", a_key.compute },
				{ "viewport", a_key.viewport },
				{ "position", a_entry.position },
				{ "lastSession", a_entry.lastSession } };
		}

		/** @brief A manifest shape, or nothing for an entry this build does not know (another version's effect, a malformed line). */
		std::optional<std::pair<ShapeKey, ManifestEntry>> FromJson(const json& a_item)
		{
			try {
				const auto effect = EffectNamed(a_item.at("effect").get<std::string>());
				if (effect < 0)
					return std::nullopt;
				ShapeKey key{};
				key.effect = static_cast<std::uint32_t>(effect);
				key.output = a_item.at("output").get<std::int32_t>();
				key.output1 = a_item.at("output1").get<std::int32_t>();
				// Arrays of up to kMaxSlots (manifests written before the SAO composite hold five): the rest keep their defaults.
				const auto fill = [&](const char* a_name, auto& a_out) {
					using Value = typename std::remove_reference_t<decltype(a_out)>::value_type;
					const auto values = a_item.at(a_name).get<std::vector<Value>>();
					if (values.size() > a_out.size())
						throw std::out_of_range(a_name);
					std::ranges::copy(values, a_out.begin());
				};
				fill("inputs", key.inputs);
				fill("address", key.address);
				fill("filter", key.filter);
				key.blend = a_item.at("blend").get<std::uint32_t>();
				key.compute = a_item.at("compute").get<bool>();
				key.viewport = a_item.at("viewport").get<decltype(key.viewport)>();
				if (key.compute != kEffects[key.effect].compute)
					return std::nullopt;
				ManifestEntry entry{ .position = a_item.at("position").get<std::uint32_t>(), .lastSession = a_item.at("lastSession").get<std::uint32_t>() };
				return std::make_pair(key, entry);
			} catch (const std::exception&) {
				return std::nullopt;
			}
		}

		/** @brief Setup: the manifest read (a missing or malformed file is an empty one), this session's number, and the old shapes dropped. */
		void ReadManifest()
		{
			auto& state = IS();
			std::error_code error;
			const auto path = ManifestPath();
			if (!std::filesystem::exists(path, error)) {
				state.session = 1;
				logger::info("[ORG] ImageSpace: no shape manifest yet ({}); this session's shapes stay with the engine and are written to it",
					path.string());
				return;
			}
			try {
				std::ifstream file(path);
				const auto root = json::parse(file);
				if (root.value("version", 0u) != kManifestVersion) {
					state.session = 1;
					logger::info("[ORG] ImageSpace: the shape manifest is another version's; it is written again from this session");
					return;
				}
				state.session = root.value("session", 0u) + 1;
				std::uint32_t dropped = 0;
				for (const auto& item : root.value("shapes", json::array())) {
					auto shape = FromJson(item);
					if (!shape)
						continue;
					if (state.session - shape->second.lastSession > kManifestSessions) {
						++dropped;
						continue;
					}
					state.manifest.insert_or_assign(shape->first, shape->second);
				}
				for (const auto& [key, item] : root.value("blends", json::object()).items()) {
					D3D11_BLEND_DESC desc{};
					try {
						if (BlendFromJson(item, desc))
							state.blendDescs.emplace(static_cast<std::uint32_t>(std::stoul(key)), desc);
					} catch (const std::exception&) {
					}
				}
				for (const auto& [name, defines] : root.value("defines", json::object()).items()) {
					const auto effect = EffectNamed(name);
					if (effect >= 0 && defines.is_array())
						state.savedDefines[static_cast<std::size_t>(effect)] = defines.get<std::vector<std::pair<std::string, std::string>>>();
				}
				logger::info("[ORG] ImageSpace: the shape manifest holds {} shapes (session {}; {} not seen for {} sessions dropped)", state.manifest.size(),
					state.session, dropped, kManifestSessions);
			} catch (const std::exception& e) {
				state.manifest.clear();
				state.session = 1;
				logger::warn("[ORG] ImageSpace: the shape manifest could not be read ({}); it is written again from this session", e.what());
			}
		}

		/**
		 * @brief The manifest written: every shape with the last session that saw it, the shapes seen this session marked with this one.
		 * At most kGateEntries shapes, the most recently seen first. Render thread; a small file.
		 */
		void WriteManifest()
		{
			auto& state = IS();
			for (const auto& key : state.seen)
				state.manifest[key].lastSession = state.session;
			std::vector<std::pair<ShapeKey, ManifestEntry>> shapes(state.manifest.begin(), state.manifest.end());
			std::ranges::stable_sort(shapes, [](const auto& a_left, const auto& a_right) { return a_left.second.lastSession > a_right.second.lastSession; });
			if (shapes.size() > kGateEntries)
				shapes.resize(kGateEntries);
			json items = json::array();
			for (const auto& [key, entry] : shapes)
				items.push_back(ToJson(key, entry));
			// Each seen effect's defines as composed now, so a session whose manager makes that effect after the graph's build has them.
			for (const auto& key : state.seen)
				if (ShaderOf(key.effect))
					state.savedDefines[key.effect] = LiveDefinesOf(key.effect);
			json defines = json::object();
			for (const auto& [effect, list] : state.savedDefines)
				defines[std::string(kEffects[effect].effectName)] = list;
			// The blend descriptions the shapes use, so a build before the engine fills its blend table makes their passes.
			json blends = json::object();
			for (const auto& [key, entry] : shapes)
				if (const auto kept = state.blendDescs.find(key.blend); !key.compute && kept != state.blendDescs.end())
					blends[std::to_string(key.blend)] = BlendToJson(kept->second);
			const json root{ { "version", kManifestVersion }, { "session", state.session }, { "shapes", std::move(items) }, { "defines", std::move(defines) },
				{ "blends", std::move(blends) } };
			std::error_code error;
			const auto path = ManifestPath();
			std::filesystem::create_directories(path.parent_path(), error);
			const auto temporary = std::filesystem::path(path).concat(L".tmp");
			{
				std::ofstream file(temporary, std::ios::trunc);
				file << root.dump(1, '\t');
				if (!file) {
					logger::warn("[ORG] ImageSpace: the shape manifest could not be written ({})", path.string());
					return;
				}
			}
			std::filesystem::rename(temporary, path, error);
			if (error) {
				logger::warn("[ORG] ImageSpace: the shape manifest could not be replaced ({}: {})", path.string(), error.message());
				return;
			}
			state.seenAtWrite = state.seen.size();
			state.manifestWritten = state.frame;
			logger::info("[ORG] ImageSpace: wrote the shape manifest ({} shapes, {} seen this session)", shapes.size(), state.seen.size());
		}

		/**
		 * @brief Once a frame: the manifest written when this session saw shapes since the last write (new ones, or known ones whose last
		 * session it updates), at most every kManifestWriteFrames frames. So the world's shapes are written after the main menu's.
		 */
		void MaybeWriteManifest()
		{
			auto& state = IS();
			if (state.seen.size() == state.seenAtWrite || state.frame - state.manifestWritten < kManifestWriteFrames)
				return;
			WriteManifest();
		}

		/** @brief A shape drawn or refused this session: noted for the manifest (a new one at its place in the frame). */
		void NoteShape(const ShapeKey& a_key)
		{
			auto& state = IS();
			if (!state.seen.insert(a_key).second)
				return;
			if (state.manifest.contains(a_key))
				return;
			state.manifest.emplace(a_key, ManifestEntry{ .position = state.scope << 16 | std::min(state.drawIndex, 0xFFFFu), .lastSession = state.session });
			if (state.logShapes) {
				const auto& viewport = a_key.viewport;
				logger::info("[ORG] ImageSpace: new shape {} into {}{}{} from {} {} {}, viewport {}x{} at ({}, {})", kEffects[a_key.effect].effectName,
					NameForLog(a_key.output), a_key.output1 >= 0 ? " and " : "", a_key.output1 >= 0 ? NameForLog(a_key.output1) : "", NameForLog(a_key.inputs[0]),
					NameForLog(a_key.inputs[1]), NameForLog(a_key.inputs[2]), viewport[2], viewport[3], viewport[0], viewport[1]);
			}
		}

		/**
		 * @brief The shapes to register at Setup: the manifest's (selected effects only), the water's flow copy (known from the RE, no
		 * session needs to see it first: the flow map from its source by heap index), and the shapes they imply, in frame order.
		 */
		std::vector<ShapeKey> ShapesToRegister()
		{
			auto& state = IS();
			std::vector<ShapeKey> keys;
			std::map<ShapeKey, std::uint32_t> positions;
			for (const auto& [key, entry] : state.manifest) {
				if (!state.enabled[key.effect])
					continue;
				keys.push_back(key);
				positions.emplace(key, entry.position);
			}
			if (const auto copy = EffectNamed("ISCopySubRegionCS"); copy >= 0 && state.enabled[static_cast<std::size_t>(copy)] && FlowMap()) {
				ShapeKey flow{};
				flow.effect = static_cast<std::uint32_t>(copy);
				flow.compute = true;
				flow.output = kFlowMapOutput;
				flow.inputs[0] = kLatchSourceInput;
				if (std::ranges::find(keys, flow) == keys.end()) {
					keys.push_back(flow);
					positions.emplace(flow, static_cast<std::uint32_t>(ScopeKind::FlowMap) << 16);
				}
			}
			AddImpliedShapes(keys, positions);
			// Entries (and so the passes' order) follow each shape's place in its scope, so a frame's draws make one batch per scope
			// whichever partners it uses.
			std::ranges::stable_sort(keys, [&](const ShapeKey& a_left, const ShapeKey& a_right) { return positions[a_left] < positions[a_right]; });
			return keys;
		}

		/**
		 * @brief Once a frame: when the engine made a registered target again (a resize), the client registers the same shapes on the
		 * new imports. FrameAnchors::EngineTarget's new import already asks every anchor's graph to be built again; this registration
		 * joins that build. Until it is built, CanRun refuses the draws.
		 */
		void CheckImports()
		{
			auto& state = IS();
			bool changed = false;
			for (const auto& [name, object] : state.importedObjects)
				if (EngineObjectOf(name) != object) {
					changed = true;
					break;
				}
			if (!changed)
				return;
			logger::info("[ORG] ImageSpace: the engine made its targets again; the {} passes register again on the new imports", state.specs.size());
			std::vector<ShapeKey> keys;
			for (const auto& spec : state.specs)
				keys.push_back(spec->key);
			state.refusedShapes.clear();
			RegisterClient(keys);
		}

		/**
		 * @brief A pass left out at a build because the engine had not filled its blend or sampler table yet: the client's graph is built
		 * once more, at the next frame outside the world (no player cell: the main menu, where the first build happens) once the tables
		 * hold what the passes need. Never in play: a world frame leaves it for the next session (logged).
		 */
		void RetryBuild()
		{
			auto& state = IS();
			if (!state.retryBuild)
				return;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && player->GetParentCell()) {
				state.retryBuild = false;
				logger::warn("[ORG] ImageSpace: passes left out at the graph's build for an unfilled engine state table stay out until the next session");
				return;
			}
			for (const auto& spec : state.specs) {
				if (D3D11_BLEND_DESC desc{}; !spec->key.compute && !BlendDescOf(spec->key.blend, desc))
					return;
				const auto& effect = kEffects[spec->key.effect];
				for (std::uint32_t slot = 0; slot < effect.samplers; ++slot)
					if (spec->inputs[slot] && !(effect.cubes & (1u << slot)) && !Pipeline::EngineSamplerState(spec->key.address[slot], spec->key.filter[slot]))
						return;
			}
			state.retryBuild = false;
			logger::info("[ORG] ImageSpace: the engine's state tables are filled; the client's graph is built again (outside the world)");
			FrameAnchors::Refresh(kClientId);
		}

		bool Usable()
		{
			const auto& state = IS();
			return state.on && !state.flushing && FrameAnchors::IsRenderOwner() && RenderGraphRuntime::Get().IsActive();
		}

		/**
		 * @brief Runs the pending batch at the ImageSpace anchor, here in the D3D11 stream.
		 *
		 * Its draws were taken only while FrameAnchors::CanRun said the run would draw them, so a run that did not submit is an epoch
		 * failure (its onFailed returns ImageSpace to the engine at the next poll). The engine's state for those draws is gone by
		 * now, so they cannot be drawn by the engine any more: they are counted and logged as an error.
		 */
		void Flush()
		{
			auto& state = IS();
			if (state.batch.empty() || state.flushing)
				return;
			const auto count = state.batch.size();
			state.executed = false;
			state.flushing = true;
			const auto result = FrameAnchors::Run(Anchor::ImageSpace);
			state.flushing = false;
			++state.runsThisFrame;
			state.batch.clear();
			state.keepAlive.clear();
			if (!result || !state.executed) {
				state.lost += count;
				static Util::ReasonLog s_lost("[ORG] ImageSpace: a batch the anchor could run did not draw", spdlog::level::err);
				s_lost.Report(result ? "its graph did not hold the client" : "the anchor's run did not submit", fmt::format("{} draws missed this frame", count));
				return;
			}
			state.taken += count;
			++state.batches;
			if (state.batches == 1)
				logger::info("[ORG] ImageSpace: the first batch ({} draws) ran on the graph", count);
		}

		/**
		 * @brief Counts an ImageSpace draw or dispatch the engine makes on D3D11: one inside an engine ImageSpace call (named by the
		 * innermost call's shader), or one outside them whose technique is an ImageSpace shader (State::currentShader: an effect drawn
		 * through another path, such as BSBatchRenderer). Other draws are not counted. a_context says why the port did not look at
		 * it (empty when it did). LogEngineDraws writes the counts.
		 */
		void NoteEngineDraw(bool a_compute, std::string_view a_context)
		{
			auto& state = IS();
			const char* name = nullptr;
			std::string why;
			if (!state.callShaders.empty()) {
				const auto* shader = state.callShaders.back();
				name = shader && shader->name ? shader->name : "an unnamed effect";
				if (state.current && state.current->covered)
					why = fmt::format("covered, {}", state.lastRefusal.empty() ? std::string("refused") : state.lastRefusal);
				else
					why = "not covered";
			} else {
				const auto* current = globals::state ? globals::state->currentShader : nullptr;
				if (!current || current->shaderType.get() != RE::BSShader::Type::ImageSpace)
					return;
				const auto* shader = static_cast<const RE::BSImagespaceShader*>(current);
				name = shader->name ? shader->name : "an unnamed effect";
				why = "outside the hooked ImageSpace calls";
			}
			if (!a_context.empty())
				why = fmt::format("{}, {}", why, a_context);
			const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			std::string key;
			if (a_compute)
				key = fmt::format("{} dispatch ({})", name, why);
			else
				key = fmt::format("{} into rt{} depth {} ({})", name, static_cast<std::int32_t>(shadow.renderTargets[0]),
					shadow.depthStencil == UINT32_MAX ? std::string("none") : fmt::format("{}", shadow.depthStencil), why);
			++state.engineDraws[key];
		}

		/** @brief Once every kEngineDrawFrames frames: the counts NoteEngineDraw made since the last summary, as one line. */
		void LogEngineDraws()
		{
			constexpr std::uint32_t kEngineDrawFrames = 300;
			auto& state = IS();
			if (state.frame - state.engineDrawsSince < kEngineDrawFrames)
				return;
			if (!state.engineDraws.empty()) {
				std::string list;
				for (const auto& [key, count] : state.engineDraws)
					list += fmt::format("{}{}: {}", list.empty() ? "" : ", ", key, count);
				logger::info("[ORG] ImageSpace: draws and dispatches left to D3D11 in {} frames: {}", state.frame - state.engineDrawsSince, list);
				state.engineDraws.clear();
			}
			state.engineDrawsSince = state.frame;
		}

		/** @brief Once a frame, on the first ImageSpace call of the render owner: the frame's runs, a resize, the programs, the manifest. */
		void BeginFrame()
		{
			auto& state = IS();
			const auto frame = globals::state ? globals::state->frameCount : 0u;
			if (frame == state.frame)
				return;
			Flush();
			state.frame = frame;
			state.runsThisFrame = 0;
			state.scopeRuns.fill(0);
			QueuePrograms();
			CheckImports();
			RetryBuild();
			MaybeWriteManifest();
			LogEngineDraws();
		}

		/**
		 * @brief The pass for a read shape, and room for it in a batch: the spec when the draw or dispatch may be withheld (any pending
		 * batch it cannot join has run), or null when the engine keeps it (its reason counted).
		 */
		const Spec* AdmitShape(const ShapeKey& a_key, std::size_t a_effect)
		{
			auto& state = IS();
			NoteShape(a_key);
			const Spec* spec = SpecFor(a_key);
			if (!spec) {
				Flush();
				Refuse("a shape this session did not register; the manifest has it from the next session", a_effect);
				return nullptr;
			}
			if (!state.passMade[spec->entry]) {
				Flush();
				Refuse("its pass was left out of the graph's build (logged with the reason)", a_effect);
				return nullptr;
			}
			// A batch runs in its passes' order: a pass at or before the last one taken (the engine's order differs from the manifest's,
			// or the same pass again) starts a new batch.
			if (!state.batch.empty() && spec->entry <= state.lastEntry)
				Flush();
			// A new batch is a new run: withheld only when the run can draw it now (owner, built graph), and only as often as its kind of
			// scope may start one this frame (kScopeRuns).
			if (state.batch.empty()) {
				if (state.scopeRuns[state.scope] >= kScopeRuns[state.scope]) {
					Refuse("its scope used its runs this frame", a_effect);
					return nullptr;
				}
				if (!FrameAnchors::CanRun(kClientId, Anchor::ImageSpace)) {
					// Before the first build after Setup or after a resize's registration (one build point).
					Refuse("the ImageSpace anchor's graph does not hold this registration yet", a_effect);
					return nullptr;
				}
			}
			return spec;
		}

		/** @brief A withheld draw or dispatch joins the batch (a new batch counts against its scope). */
		void AddToBatch(const Spec& a_spec, Pending a_pending, std::span<const std::shared_ptr<const org::LatchBlock>> a_blocks)
		{
			auto& state = IS();
			if (state.batch.empty())
				++state.scopeRuns[state.scope];
			a_pending.entry = a_spec.entry;
			state.batch.push_back(a_pending);
			for (const auto& block : a_blocks)
				state.keepAlive.push_back(block);
			state.lastEntry = a_spec.entry;
		}

		/**
		 * @brief A geometry effect's draw (the ripple makers): its geometry (DrawTriShape's shape and index range) leased and written into
		 * a_pending's PerFrame words, which such an effect does not read (ISWaterDisplacement.hlsl, PullRippleVertex): frameSrv and
		 * frameOffset the vertex buffer's address, frameSize and source the index buffer's, padding the stride and the first index. The
		 * gate opens it with its index count. False (refused) when the geometry cannot be read.
		 */
		bool LatchGeometry(Pending& a_pending, std::size_t a_effect)
		{
			auto& state = IS();
			const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			DCLF::Family::Geometry geometry;
			auto owners = std::make_shared<DCLF::Family::Owners>();
			std::string detail;
			const auto result = DCLF::Family::LatchGeometry(static_cast<const DCLF::Family::TriShape*>(state.drawShape), state.drawStart, state.drawTriangles, ~std::uint64_t(0),
				static_cast<std::uint32_t>(shadow.topology), geometry, *owners, &detail);
			if (result != DCLF::Family::GeometryResult::Ok) {
				Flush();
				Refuse(DCLF::Family::ReasonOf(result), a_effect, detail);
				return false;
			}
			if (geometry.firstIndex > 0xFFFF || geometry.stride > 0xFFFF) {
				Flush();
				Refuse("its first index or stride does not fit the latch's 16-bit fields", a_effect);
				return false;
			}
			a_pending.at.frameSrv = static_cast<std::uint32_t>(geometry.vertexAddress);
			a_pending.at.frameOffset = static_cast<std::uint32_t>(geometry.vertexAddress >> 32);
			a_pending.at.frameSize = static_cast<std::uint32_t>(geometry.indexAddress);
			a_pending.at.source = static_cast<std::uint32_t>(geometry.indexAddress >> 32);
			a_pending.at.padding = geometry.stride | (geometry.firstIndex << 16);
			a_pending.vertices = geometry.indexCount;
			a_pending.source = std::move(owners);
			return true;
		}

		/**
		 * @brief The engine's draw of the current effect: taken into the batch (true; the caller skips the engine's draw) or left to the
		 * engine (false, after running any pending batch so the engine's order holds).
		 */
		bool TakeDraw()
		{
			auto& state = IS();
			auto& scope = *state.current;
			scope.drew = true;
			const auto& effect = kEffects[scope.effect];
			++state.drawIndex;
			ShapeKey key{};
			OutputView output;
			std::string_view refusal;
			if (!ReadShape(scope.effect, key, output, refusal)) {
				Flush();
				Refuse(refusal, scope.effect);
				return false;
			}
			// The engine's state for the draw goes to D3D11 as before (the draw would apply it; when the engine draws, its own call
			// finds nothing left to apply). The shadow state then holds the viewport the draw uses.
			static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
			setDirtyStates(false);
			std::string detail;
			if (!ReadViewport(output, key, refusal, detail)) {
				Flush();
				Refuse(refusal, scope.effect, detail);
				return false;
			}
			const Spec* spec = AdmitShape(key, scope.effect);
			if (!spec)
				return false;
			// The capture records the draw's constants as a use: the draw is withheld, so no draw hook records it.
			const auto use = Constants::Capture(Constants::kDrawStages);
			const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 2) : nullptr;
			// The ImageSpace setups write PerGeometry into the PS b2 size-class pool (PSGeometry); a custom mapper's ring is accepted too.
			// None bound is a shader without PerGeometry constants (ISCopy's): D3D11 reads 0 there, and so does the latch's empty window.
			if (slot && slot->group != Constants::Group::PSGeometry && slot->group != Constants::Group::Custom) {
				Flush();
				Failure("the capture holds PerGeometry constants outside the geometry and custom pools for its draw", scope.effect);
				return false;
			}
			// FrameBuffer as the draw had it bound (PS b12, the engine's per-frame buffer), not CS's shared snapshot.
			const auto* frameSlot = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 12) : nullptr;
			if (!frameSlot) {
				Flush();
				Failure("the capture holds no PerFrame constants for its draw", scope.effect);
				return false;
			}
			const auto frameAt = Constants::Where(*frameSlot);
			// No PerGeometry: an empty window into the PerFrame chunk, so the row reads stay in a valid buffer and read 0.
			const auto at = slot ? Constants::Where(*slot) : frameAt;
			const std::uint32_t size = slot ? at.size : 0u;
			if (!at.block || !frameAt.block) {
				Flush();
				Failure("its captured constants have no ring chunk", scope.effect);
				return false;
			}
			const std::array blocks{ at.block, frameAt.block };
			Pending pending{ .at = { .srv = at.srvIndex, .offset = at.offset, .frameSrv = frameAt.srvIndex, .frameOffset = frameAt.offset, .size = size, .frameSize = frameAt.size } };
			if (effect.geometry && !LatchGeometry(pending, scope.effect))
				return false;
			AddToBatch(*spec, std::move(pending), blocks);
			if (state.logShapes && state.loggedConstants.insert(key).second)
				logger::info("[ORG] ImageSpace: {} into {} taken with {} bytes of PerGeometry ({}) and {} of PerFrame", effect.effectName, NameForLog(key.output), size,
					!slot ? "none bound" : slot->group == Constants::Group::PSGeometry ? "PS geometry pool" : "custom", frameAt.size);
			return true;
		}

		/**
		 * @brief The engine's dispatch of the current compute effect (DispatchCSShader inside DispatchComputeShader): taken into the
		 * batch with its group counts (true; the caller skips the engine's dispatch) or left to the engine (false, after running any
		 * pending batch).
		 */
		bool TakeDispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
		{
			auto& state = IS();
			auto& scope = *state.current;
			scope.drew = true;
			const auto& effect = kEffects[scope.effect];
			++state.drawIndex;
			ShapeKey key{};
			std::string_view refusal;
			ID3D11ShaderResourceView* source = nullptr;
			if (!ReadComputeShape(scope.effect, key, refusal, source)) {
				Flush();
				Refuse(refusal, scope.effect);
				return false;
			}
			if (a_x == 0 || a_y == 0 || a_z == 0) {
				// Nothing to dispatch: the engine's call does nothing either.
				Flush();
				Refuse("an empty dispatch", scope.effect);
				return false;
			}
			const Spec* spec = AdmitShape(key, scope.effect);
			if (!spec)
				return false;
			// A flow copy's source, a loaded texture (an ORG engine texture, or DXVK's imported), read by its heap index from the latch.
			DCLF::GpuTextures::Binding sourceBinding;
			if (source) {
				constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::FrameSlot, 0);
				sourceBinding = DCLF::GpuTextures::Get().ResolveBinding(source, kSourceTag);
				if (sourceBinding.index == DCLF::GpuTextures::kInvalid) {
					Flush();
					Failure("its source texture has no heap index", scope.effect);
					return false;
				}
			}
			// As DispatchCSShader would: the compute state to D3D11 (SetDirtyStates(true) clears the modified bits as the dispatch would),
			// then CS b0, which DispatchCSShader binds itself: bound here so the capture records this dispatch's constants as a use.
			static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
			setDirtyStates(true);
			ID3D11Buffer* constants = Tripwire::Unwrap(Constants::EngineComputeBuffer());
			if (!constants) {
				Flush();
				Failure("no engine compute constant buffer", scope.effect);
				return false;
			}
			globals::d3d::context->CSSetConstantBuffers(0, 1, &constants);
			const auto use = Constants::Capture(Constants::StageBit(Constants::Stage::Compute));
			const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Compute, 0) : nullptr;
			if (!slot || slot->group != Constants::Group::Compute) {
				Flush();
				Failure("the capture holds no compute constants for its dispatch", scope.effect);
				return false;
			}
			const auto at = Constants::Where(*slot);
			if (!at.block) {
				Flush();
				Failure("its captured constants have no ring chunk", scope.effect);
				return false;
			}
			const std::array blocks{ at.block };
			ConstantsAt where{ .srv = at.srvIndex, .offset = at.offset, .size = at.size };
			if (source)
				where.source = sourceBinding.index;
			AddToBatch(*spec, { .at = where, .compute = true, .groups = { a_x, a_y, a_z }, .source = std::move(sourceBinding.owner) }, blocks);
			if (state.logShapes && state.loggedConstants.insert(key).second)
				logger::info("[ORG] ImageSpace: {} into {} taken with {} bytes of compute constants", effect.effectName, NameForLog(key.output), at.size);
			return true;
		}

		/** @brief Why an effect's Render of a_geometry cannot have its draw taken, or empty. */
		std::string_view Ineligible(const void* a_geometry, bool a_geometryEffect)
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active (CS_ORG_ENGINE_CONSTANTS=1)";
			// ImageSpaceEffect+0x88 (unk88): the effect keeps its own viewport; BSImagespaceShader::Render skips UpdateViewPort then
			// (checked in Ghidra on SE 0x14130cd20 and AE 0x1414f88b0). Not a refusal: the draw's shape takes the viewport the shadow
			// state holds once SetDirtyStates ran (ReadViewport), whoever set it.
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager)
				return "no ImageSpaceManager";
			// The manager's rectangle (+0x10 left, +0x14 right, +0x18 top, +0x1C bottom; menu 3D sets it): Render applies it as the
			// viewport when left and right are not -1. That is the engine's own test (the same two fields on SE and AE), so this
			// tests the same two.
			// NiRect keeps its fields protected: read them in their order (left, right, top, bottom).
			const auto* rect = reinterpret_cast<const std::int32_t*>(&manager->rect);
			if (rect[0] != -1 && rect[1] != -1)
				return "a sub-rectangle of its target";
			// +0x58 (RUNTIME_DATA::unk58, SE and AE; VR's layout differs): RenderEffect draws the dynamic-resolution quad (+0x50)
			// instead of the full one (+0x48) when it is set (SE 0x141297b40, AE 0x141481ff0).
			// A geometry effect (the ripple makers) draws its own shape, whatever quad the manager would use.
			if (a_geometryEffect)
				return {};
			if (manager->GetRuntimeData().unk58)
				return "dynamic resolution";
			// Render draws its geometry argument (AE 0x1414f88b0): the manager's quad (+0x48, +0x50 for dynamic resolution) for an
			// effect, or the ripple makers' own quads in the water's displacement update. The graph's fullscreen triangle is the first.
			const auto* base = reinterpret_cast<const std::byte*>(manager);
			if (a_geometry != *reinterpret_cast<void* const*>(base + 0x48) && a_geometry != *reinterpret_cast<void* const*>(base + 0x50))
				return "a geometry other than the full-screen quad";
			return {};
		}

		/**
		 * @brief Why a draw through the explicit-target Render (100951 / 107733) cannot be taken, or empty. That Render sets neither
		 * the target nor the viewport (its caller does; the draw's shape checks both) and applies no manager rectangle; it draws
		 * a_geometry, or the manager's quad (+0x48) when that is null (checked in Ghidra on AE 0x1414f8b00).
		 */
		std::string_view IneligibleExplicit(const void* a_geometry)
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active (CS_ORG_ENGINE_CONSTANTS=1)";
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager)
				return "no ImageSpaceManager";
			const auto* quad = *reinterpret_cast<void* const*>(reinterpret_cast<const std::byte*>(manager) + 0x48);
			if (a_geometry && a_geometry != quad)
				return "a geometry other than the full-screen quad";
			return {};
		}

		/** @brief Why a compute effect's dispatch cannot be taken, or empty. */
		std::string_view IneligibleDispatch()
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active (CS_ORG_ENGINE_CONSTANTS=1)";
			return {};
		}

		/**
		 * @brief A scope begins (ScopeKind): its draws' passes are ordered by their place in it, and its kind may start kScopeRuns
		 * batches a frame. Nested scopes are part of the outer one.
		 */
		void EnterScope(ScopeKind a_scope)
		{
			if (!Usable())
				return;
			BeginFrame();
			auto& state = IS();
			if (state.scopeDepth++ == 0) {
				state.scope = static_cast<std::uint32_t>(a_scope);
				state.drawIndex = 0;
			}
		}

		void LeaveScope()
		{
			auto& state = IS();
			if (state.scopeDepth == 0)
				return;
			if (--state.scopeDepth == 0 && FrameAnchors::IsRenderOwner())
				Flush();
		}

		/**
		 * @brief The effect call in progress inside a scope: a_shader's draw or dispatch is taken when the effect is covered, selected
		 * and a_ineligible gives no reason; a_call runs the engine's function.
		 */
		template <class Call>
		void WithEffect(const RE::BSImagespaceShader* a_shader, bool a_compute, std::string_view a_ineligible, Call&& a_call)
		{
			auto& state = IS();
			const auto index = EffectOf(a_shader);
			EffectScope scope{ .shader = a_shader, .effect = index >= 0 ? static_cast<std::size_t>(index) : 0, .compute = a_compute };
			if (index >= 0 && state.enabled[static_cast<std::size_t>(index)] && kEffects[scope.effect].compute == a_compute) {
				scope.covered = true;
				scope.eligible = a_ineligible.empty();
				if (!scope.eligible)
					Refuse(a_ineligible, scope.effect);
			}
			const auto previous = state.current;
			state.current = scope;
			a_call();
			if (state.current->eligible && !state.current->drew)
				state.fallbacks[scope.effect]->Report(a_compute ? "it ran without DispatchCSShader; its dispatches stay the engine's" :
																  "it drew without DrawTriShape; its draws stay the engine's");
			state.current = previous;
		}

		/** @brief Whether a_shader is a selected effect drawn outside every scope, whose call is a scope of its own (ScopeKind::Effect). */
		bool OwnsScope(const RE::BSImagespaceShader* a_shader)
		{
			const auto index = EffectOf(a_shader);
			return index >= 0 && kEffects[static_cast<std::size_t>(index)].ownScope && IS().enabled[static_cast<std::size_t>(index)];
		}

		/**
		 * @brief a_call inside the scope the effect needs: the enclosing scope, or one of its own for an own-scope effect called outside
		 * every scope (ScopeKind::FlowMap for a flow copy, whose target the shadow state holds before the dispatch; else
		 * ScopeKind::Effect); false when the effect is not taken there (a_call not run).
		 */
		template <class Call>
		bool InScope(const RE::BSImagespaceShader* a_shader, Call&& a_call)
		{
			auto& state = IS();
			if (state.scopeDepth > 0) {
				a_call();
				return true;
			}
			if (!OwnsScope(a_shader))
				return false;
			const bool flowCopy = kEffects[static_cast<std::size_t>(EffectOf(a_shader))].id == Effects::ISCopySubRegionCS && FlowCopyBound();
			EnterScope(flowCopy ? ScopeKind::FlowMap : ScopeKind::Effect);
			if (state.scopeDepth == 0)
				return false;
			a_call();
			LeaveScope();
			return true;
		}

		/**
		 * @brief a_call (the engine's ImageSpace function) counted as an engine ImageSpace call, with its shader: a draw or dispatch
		 * inside it that the port does not take is named by that shader (NoteEngineDraw).
		 */
		template <class Call>
		void EngineCall(const RE::BSImagespaceShader* a_shader, Call&& a_call)
		{
			auto& state = IS();
			++state.engineCalls;
			state.callShaders.push_back(a_shader);
			a_call();
			state.callShaders.pop_back();
			--state.engineCalls;
		}

		/**
		 * @brief The engine's draw or dispatch, which ImageSpace leaves to it, unvouched: the flush latch skips it and the census
		 * counts it. Named in the summary when it is an ImageSpace draw (NoteEngineDraw); a covered effect's refusal or failure also
		 * has its reason logged once.
		 */
		template <class Call>
		void EngineDraw(bool a_compute, std::string_view a_context, Call&& a_call)
		{
			NoteEngineDraw(a_compute, a_context);
			a_call();
		}

		/** @brief BSImagespaceShader::Render (100950 / 107732): every raster ImageSpace shader's draw, through DrawTriShape. */
		struct ShaderRender
		{
			static void thunk(void* a_effect, void* a_quad, void* a_param)
			{
				if (!a_effect || !Usable()) {
					func(a_effect, a_quad, a_param);
					return;
				}
				// The ImageSpaceEffect base sits at +0x90 of a BSImagespaceShader (SE and AE: Render's this - 0x90).
				const auto* effect = static_cast<const RE::ImageSpaceEffect*>(a_effect);
				const auto* shader = static_cast<const RE::BSImagespaceShader*>(effect);
				const bool covered = EffectOf(shader) >= 0;
				EngineCall(shader, [&] {
					const bool scoped = InScope(shader, [&] {
						WithEffect(shader, false, covered ? Ineligible(a_quad, kEffects[static_cast<std::size_t>(EffectOf(shader))].geometry) : std::string_view{}, [&] { func(a_effect, a_quad, a_param); });
					});
					if (!scoped)
						func(a_effect, a_quad, a_param);
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSImagespaceShader's explicit-target Render (100951 / 107733): the draw of a shader whose caller bound its target and
		 * textures (RenderSSAO, the reflections pass, the water blend), through DrawTriShape. Its this is the BSImagespaceShader itself.
		 * Outside every scope only an own-scope effect is taken (the water blend).
		 */
		struct ExplicitRender
		{
			static void thunk(RE::BSImagespaceShader* a_shader, void* a_geometry)
			{
				if (!a_shader || !Usable()) {
					func(a_shader, a_geometry);
					return;
				}
				const bool covered = EffectOf(a_shader) >= 0;
				EngineCall(a_shader, [&] {
					const bool scoped = InScope(a_shader, [&] {
						WithEffect(a_shader, false, covered ? IneligibleExplicit(a_geometry) : std::string_view{}, [&] { func(a_shader, a_geometry); });
					});
					if (!scoped)
						func(a_shader, a_geometry);
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSImagespaceShader::DispatchComputeShader (100952 / 107734): a compute effect's dispatch (its setup, then
		 * BSComputeShader::Dispatch, then DispatchCSShader, where it is taken).
		 */
		struct DispatchComputeShader
		{
			static void thunk(RE::BSImagespaceShader* a_shader, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
			{
				if (!a_shader || !Usable()) {
					func(a_shader, a_x, a_y, a_z);
					return;
				}
				EngineCall(a_shader, [&] {
					const bool scoped = InScope(a_shader, [&] { WithEffect(a_shader, true, IneligibleDispatch(), [&] { func(a_shader, a_x, a_y, a_z); }); });
					if (!scoped)
						func(a_shader, a_x, a_y, a_z);
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceEffectHDR::Render (100465 / 107183): the downsample, luminance, adaptation, bloom and tonemap draws. */
		struct HDRRender
		{
			static void thunk(void* a_effect, void* a_quad, void* a_param)
			{
				EnterScope(ScopeKind::HDR);
				func(a_effect, a_quad, a_param);
				LeaveScope();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceManager::RenderPreTonemapComposites (99024 / 105675): SIL, the lens flares, the VL / lens flare composite. */
		struct PreTonemapComposites
		{
			static void thunk(void* a_manager, std::uintptr_t a_source, std::uintptr_t a_destination)
			{
				EnterScope(ScopeKind::Composites);
				func(a_manager, a_source, a_destination);
				LeaveScope();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief RenderSSAO (100520 / 107245; its this is the SAO helper at ImageSpaceManager+0x1c8, then the source and destination):
		 * the camera depth, its mips, the raw AO and its blurs, then the composite. Called from Main::RenderWorld.
		 */
		struct RenderSSAO
		{
			static std::uint64_t thunk(void* a_helper, std::int32_t a_source, std::uint64_t a_destination)
			{
				EnterScope(ScopeKind::SSAO);
				const auto result = func(a_helper, a_source, a_destination);
				LeaveScope();
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The screen-space reflections pass (100428 / 107146, no arguments; the helper at ImageSpaceManager+0x1c0): the normal
		 * mask's downsample, the ray march, and the separable compute blur (107207).
		 */
		struct RenderReflections
		{
			static void thunk()
			{
				EnterScope(ScopeKind::Reflections);
				func();
				LeaveScope();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The water system's displacement update (31385 / 32176; its this is the water system): the WaterDisplacement effect
		 * (slot 31) through RenderEffect, its simulation draws into kWATER_DISPLACEMENT and its swap.
		 */
		struct WaterDisplacement
		{
			static void thunk(void* a_system)
			{
				EnterScope(ScopeKind::Water);
				func(a_system);
				LeaveScope();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSGraphics::Renderer::DrawTriShape (75477 / 77263): the current effect's draw is taken; any other runs the batch first. */
		struct DrawTriShape
		{
			static void thunk(void* a_renderer, void* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
			{
				auto& state = IS();
				if (state.on && !state.flushing && FrameAnchors::IsRenderOwner()) {
					// Only the manager's quad is an effect's draw. An effect may draw other shapes inside its scope (a batch's passes
					// through RenderPassImmediately): those go to the engine, and must not reach SetDirtyStates through TakeDraw first.
					// DrawTriShape gets the geometry's renderer data, not the geometry the manager holds (+0x48, +0x50 for dynamic
					// resolution): compare with theirs.
					const auto* manager = RE::ImageSpaceManager::GetSingleton();
					const auto* base = reinterpret_cast<const std::byte*>(manager);
					const auto rendererDataOf = [&](std::size_t a_offset) -> const void* {
						auto* geometry = manager ? *reinterpret_cast<RE::BSGeometry* const*>(base + a_offset) : nullptr;
						return geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
					};
					const bool quad = a_shape && (a_shape == rendererDataOf(0x48) || a_shape == rendererDataOf(0x50));
					// A geometry effect (the ripple makers) draws its own shape, which its vertex stage pulls (LatchGeometry).
					const bool geometry = a_shape && state.current && kEffects[state.current->effect].geometry;
					state.drawShape = a_shape;
					state.drawStart = a_start;
					state.drawTriangles = a_triangles;
					if ((quad || geometry) && state.scopeDepth > 0 && state.current && state.current->eligible && !state.current->compute && !state.current->drew) {
						// TakeDraw applies the engine's state (SetDirtyStates) before the engine's DrawTriShape would have: give the
						// shadow state this shape's vertex description first, as DrawTriShape does. Otherwise the input layout is
						// built from the previous draw's description and the effect's vertex shader.
						// BSGraphics::TriShape: vertex buffer, index buffer, then the VertexDesc at +0x10.
						const auto shapeDesc = *reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(a_shape) + 0x10);
						auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
						if (shadow.vertexDesc != shapeDesc) {
							shadow.vertexDesc = shapeDesc;
							shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
						}
						if (TakeDraw())
							return;
					} else
						Flush();
					EngineDraw(false, {}, [&] { func(a_renderer, a_shape, a_start, a_triangles); });
					return;
				}
				if (state.on) {
					EngineDraw(false, state.flushing ? "during a batch's run" : "not the render owner", [&] { func(a_renderer, a_shape, a_start, a_triangles); });
					return;
				}
				func(a_renderer, a_shape, a_start, a_triangles);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSGraphics::Renderer::DispatchCSShader (75532 / 77329): the current compute effect's dispatch is taken; any other
		 * engine dispatch runs the batch first.
		 */
		struct DispatchCSShader
		{
			static void thunk(void* a_renderer, void* a_shader, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
			{
				auto& state = IS();
				if (state.on && !state.flushing && FrameAnchors::IsRenderOwner()) {
					if (state.scopeDepth > 0 && state.current && state.current->eligible && state.current->compute && !state.current->drew) {
						if (TakeDispatch(a_x, a_y, a_z))
							return;
					} else
						Flush();
					EngineDraw(true, {}, [&] { func(a_renderer, a_shader, a_x, a_y, a_z); });
					return;
				}
				if (state.on) {
					EngineDraw(true, state.flushing ? "during a batch's run" : "not the render owner", [&] { func(a_renderer, a_shader, a_x, a_y, a_z); });
					return;
				}
				func(a_renderer, a_shader, a_x, a_y, a_z);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (!Requested())
			return;
		stl::detour_thunk<ShaderRender>(REL::RelocationID(100950, 107732));
		stl::detour_thunk<HDRRender>(REL::RelocationID(100465, 107183));
		stl::detour_thunk<PreTonemapComposites>(REL::RelocationID(99024, 105675));
		stl::detour_thunk<ExplicitRender>(REL::RelocationID(100951, 107733));
		stl::detour_thunk<DispatchComputeShader>(REL::RelocationID(100952, 107734));
		stl::detour_thunk<RenderSSAO>(REL::RelocationID(100520, 107245));
		stl::detour_thunk<RenderReflections>(REL::RelocationID(100428, 107146));
		stl::detour_thunk<WaterDisplacement>(REL::RelocationID(31385, 32176));
		stl::detour_thunk<DrawTriShape>(REL::RelocationID(75477, 77263));
		stl::detour_thunk<DispatchCSShader>(REL::RelocationID(75532, 77329));
		auto& state = IS();
		state.installed = ShaderRender::func.address() && HDRRender::func.address() && PreTonemapComposites::func.address() && ExplicitRender::func.address() &&
		                  DispatchComputeShader::func.address() && RenderSSAO::func.address() && RenderReflections::func.address() && WaterDisplacement::func.address() &&
		                  DrawTriShape::func.address() &&
		                  DispatchCSShader::func.address();
		if (state.installed)
			logger::info(
				"[ORG] ImageSpace: hooks installed (BSImagespaceShader::Render and its explicit-target form, DispatchComputeShader, the HDR effect, the pre-tonemap "
				"composites, RenderSSAO, the reflections pass, the water's displacement update, DrawTriShape, DispatchCSShader)");
		else
			logger::error("[ORG] ImageSpace: a hook could not be installed; the ImageSpace chain stays on D3D11");
	}

	void Reserve()
	{
		if (Requested())
			IS().reserved = FrameAnchors::Reserve(Anchor::ImageSpace);
	}

	void Setup()
	{
		auto& state = IS();
		if (!Requested() || state.on)
			return;
		const auto off = [](std::string_view a_why) { logger::warn("[ORG] ImageSpace: CS_ORG_IMAGESPACE=1, but {}; the ImageSpace chain stays on D3D11", a_why); };
		if (!state.installed)
			return off("its hooks are not installed");
		if (!state.reserved || !FrameAnchors::HasEpoch(Anchor::ImageSpace))
			return off("the ImageSpace anchor has no epoch");
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !RenderGraphRuntime::Get().IsActive())
			return off("the render graph is not active");
		if (!Constants::Requested())
			return off("the engine-constants capture is off (set CS_ORG_ENGINE_CONSTANTS=1)");
		// The ORG builds read PerGeometry through this header. Shaders staged from before the ORG builds (no header) compile their
		// D3D11 declarations under DXC, and every pipeline then fails on an unmapped cbuffer (PerFrame at b12 first).
		if (std::error_code error; !std::filesystem::exists(std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / "Common/OrgEngineConstants.hlsli", error))
			return off("Data/Shaders has no Common/OrgEngineConstants.hlsli: the staged shaders predate the ORG builds of ISHDR, ISBlur and the VL composite");
		state.gate = RunGate::Create("ImageSpace", kGateEntries);
		state.latch = org::LatchBlock::Create("ImageSpace::ConstantsAt", kGateEntries * static_cast<std::uint32_t>(sizeof(ConstantsAt)));
		state.heldSources.clear();
		if (!state.gate || !state.latch)
			return off("its run gate or latch could not be made");
		SelectEffects();
		for (std::size_t i = 0; i < kEffects.size(); ++i) {
			state.fallbacks[i] = std::make_unique<Util::ReasonLog>(fmt::format("[ORG] ImageSpace: {} stays with the engine", kEffects[i].effectName));
			state.failures[i] = std::make_unique<Util::ReasonLog>(fmt::format("[ORG] ImageSpace: {} failed on the graph and stays with the engine", kEffects[i].effectName),
				spdlog::level::err);
		}
		state.logShapes = Switches::On("CS_ORG_IMAGESPACE_LOG");
		state.on = true;
		std::string selected;
		for (std::size_t i = 0; i < kEffects.size(); ++i)
			if (state.enabled[i])
				selected += fmt::format("{}{}", selected.empty() ? "" : ",", kEffects[i].effectName);
		logger::info("[ORG] ImageSpace: on (CS_ORG_IMAGESPACE); effects {}", selected.empty() ? "none" : selected);
		// The water's flow map, made now if the engine has not yet (it does at the first cell with flowing water), so the flow copies'
		// pass names it from this registration.
		if (const auto copy = EffectNamed("ISCopySubRegionCS"); copy >= 0 && state.enabled[static_cast<std::size_t>(copy)])
			MakeFlowMap();
		// The session's shapes, registered once: the manifest's, the flow copy's and the shapes they imply. Their programs are queued
		// now when the engine's effects exist (else at the first ImageSpace call); each pass takes its own at the graph's first build.
		ReadManifest();
		RegisterClient(ShapesToRegister());
		QueuePrograms();
	}

	std::string Status()
	{
		const auto& state = IS();
		return fmt::format("ImageSpace: {}, {} passes, {} registrations, {} shapes in the manifest ({} seen this session), {} draws taken in {} batches, {} left to the "
						   "engine, {} missed",
			state.on ? "on" : "off", state.specs.size(), state.registrations, state.manifest.size(), state.seen.size(), state.taken, state.batches, state.engineDrawn,
			state.lost);
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::ImageSpace
{
	void Install()
	{
		if (Requested())
			logger::warn("[ORG] ImageSpace: CS_ORG_IMAGESPACE=1, but this build has no render graph");
	}
	void Reserve() {}
	void Setup() {}
	std::string Status() { return "ImageSpace: no render graph in this build"; }
}

#endif
