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
#	include "RenderGraph/Engine/ImageSpace/PostTable.h"
#	include "RenderGraph/Engine/Pipeline/Descriptions.h"
#	include "RenderGraph/Engine/Pipeline/StateConversion.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/PostChain.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/GraphResources.h"
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
#	include <atomic>
#	include <bit>
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
		// Texture slots a shape names at most (t0..t15, the engine's PS slots): ISAlphaBlend reads all sixteen, ISLightingComposite eight.
		constexpr std::uint32_t kMaxSlots = 16;
		// The shape manifest: a shape is dropped once its effect ran in this many sessions without it, and the file is written at
		// most this often. Sessions in which the effect did not run do not count: a water shape is not dropped by sessions without water.
		constexpr std::uint32_t kManifestSessions = 16;
		constexpr std::uint32_t kManifestWriteFrames = 300;
		// 2: the viewport as float bits with its depth range, the depth-stencil target and modes, and the scissor.
		constexpr std::uint32_t kManifestVersion = 2;

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
		/**
		 * @brief One of the engine's default textures (BSGraphics::State's black, white, grey and flat normal map), by DefaultTextureOf's
		 * index. The engine binds one where an input has nothing else: ISTemporalAA's t5 when Upscaling turns the vanilla TAA setting off.
		 */
		constexpr std::int32_t kDefaultTextureInput = 0x1000000;  // above the mip bits (kMipShift)
		constexpr std::int32_t kKindBits = kDepthInput | kCubeInput | kFlowMapOutput | kLatchSourceInput | kDefaultTextureInput;
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
			// Its source has an ORG vertex stage over the graph's fullscreen triangle (the copies that move their coordinates there).
			bool vertexStage = false;
			// Its vertex shader writes POSITION, not SV_Position (vanilla's ISMap and both ISNoise forms; CS's ISNoise.hlsl the same, and
			// ISMap stays on vanilla's DXBC): the engine's draw rasterizes nothing (imagespace-shaders.md, inferred). Taken with no pass:
			// the engine's state is applied as its draw would, and nothing is drawn.
			bool rastersNothing = false;
			// A bit per texture slot that holds a loaded texture, not an engine target (ISAlphaBlend's tint masks, the IB lens flares'
			// ramps and masks): such a slot is read by the heap index the draw's latch holds (SourcesAt, after its constants), so the
			// source's resource list has no SRV entry for it, and a second LATCH follows the constants' one.
			std::uint32_t latched = 0;
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
			// The copies whose vertex shader passes the coordinates through: back to the chain's destination in the pre-tonemap composites, and the main
			// frame's, the menus' and the end-of-frame chain's copies outside every scope (each call a scope of its own).
			{ Effects::ISCopy, "ISCopy", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISCopyDynamicFetchDisabled, "ISCopyDynamicFetchDisabled", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISCopyCustomViewport, "ISCopyCustomViewport", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			{ Effects::ISCopyGrayScale, "ISCopyGrayScale", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true },
			// The copies that move the coordinates in their vertex shader (a scale and bias, or a turn about the centre): ISCopy.hlsl's
			// ORG vertex stage makes the same change over the fullscreen triangle (EffectInfo::vertexStage).
			{ Effects::ISCopyScaleBias, "ISCopyScaleBias", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true, 1, false, false, false, true },
			{ Effects::ISCopyTextureMask, "ISCopyTextureMask", "iscopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false, true, 1, false, false, false, true },
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
			// The image-based lens flares (the pre-tonemap composites' +0x180): the brightest-pick downsample with its jitter
			// compensation (t0 the source, t1 the last frame's, t2 the motion vectors), the plain blur (its radius a constant), and the
			// flares from the blurred image. Own scopes outside the composites.
			{ Effects::ISDownsample, "ISDownsample", "iblensflare", "ISDownsample.hlsl", 3, 0b111, 3, 0, false, true },
			{ Effects::ISBlur, "ISBlur", "iblensflare", "ISBlur.hlsl", 2, 0b01, 2, 0, false, true },
			{ Effects::ISIBLensFlares, "ISIBLensFlares", "iblensflare", "ISIBLensFlare.hlsl", 1, 0b1, 1, 0, false, true },
			// The SIL composite (the pre-tonemap composites' +0x1d8): t0 the source, t1 the indirect lighting.
			{ Effects::ISSILComposite, "ISSILComposite", "composite", "ISSILComposite.hlsl", 2, 0b11, 2 },
			// The SIL raw indirect lighting before it: t0 the camera depth, t1 the normals, t2 the motion vectors, t3 the emissive colour,
			// t4 the accumulated indirect lighting.
			{ Effects::ISSILRawInd, "ISSILRawInd", "composite", "ISSILRawInd.hlsl", 5, 0b11111, 5 },
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
			// FXAA (the end-of-frame chain's slot 2, with the game's FXAA setting): t0 the image. Each call a scope of its own.
			{ Effects::ISFXAA, "ISFXAA", "fxaa", "ISFXAA.hlsl", 1, 0b1, 1, 0, false, true },
			// The world map's depth blur (the map menu's view, outside the frame's scopes: each call a scope of its own): t0 the image,
			// t1 the depth. The NoSkyBlur form leaves the sky sharp (NO_SKY).
			{ Effects::ISWorldMap, "ISWorldMap", "worldmap", "ISWorldMap.hlsl", 2, 0b11, 2, 0, false, true },
			{ Effects::ISWorldMapNoSkyBlur, "ISWorldMapNoSkyBlur", "worldmap", "ISWorldMap.hlsl", 2, 0b11, 2, 0, false, true },
			// The depth-of-field compute blur (107734 <- 107209 <- 107208, inside 99023): ISBlurCS's depth-of-field build, which reads
			// the depth at t1 (DEPTH_THRESHOLD). Horizontal, then vertical, each call a scope of its own.
			{ Effects::ISDepthOfFieldBlurHCS, "ISDepthOfFieldBlurHCS", "dof", "ISBlurCS.hlsl", 2, 0b11, 2, 0, true, true },
			{ Effects::ISDepthOfFieldBlurVCS, "ISDepthOfFieldBlurVCS", "dof", "ISBlurCS.hlsl", 2, 0b11, 2, 0, true, true },
			// The parallax mask's separable compute blur (ISBlurCS's parallax-mask build, the horizontal pass reading alpha): t0 only. Its
			// own scope outside the other scopes.
			{ Effects::ISParallaxMaskBlurHCS, "ISParallaxMaskBlurHCS", "parallaxmask", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true, true },
			{ Effects::ISParallaxMaskBlurVCS, "ISParallaxMaskBlurVCS", "parallaxmask", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true, true },
			// The sub-rectangle copy, t0 to u0, no sampler: the snow pass's (107734 <- 107217 <- Main::RenderWorld, engine targets) and
			// Main::RenderWaterEffects' (35561 / 36560: each queued flow texture into the flow map, the source by heap index).
			{ Effects::ISCopySubRegionCS, "ISCopySubRegionCS", "copy", "ISCopySubRegionCS.hlsl", 1, 0b1, 0, 0, true, true },
			// The water's temporal blend (the explicit-target Render, outside the other scopes): t0 the image, t1 the water history,
			// t2 the motion vectors, t3 the depth, t4 the water mask; into kMAIN (SV_Target0) and the next history (SV_Target1).
			{ Effects::ISWaterBlend, "ISWaterBlend", "waterblend", "ISWaterBlend.hlsl", 5, 0b11111, 5, 0, false, true, 2 },
			// The underwater mask (the manager's +0x208): Water.hlsl's UNDERWATERMASK form over the engine's own geometry, pulled by its
			// vertex stage (EffectInfo::geometry), usually with the main depth test. Its own scope outside the other scopes.
			{ Effects::ISUnderwaterMask, "ISUnderwaterMask", "underwatermask", "Water.hlsl", 0, 0, 0, 0, false, true, 1, false, false, true },
			// The vanilla TAA (ImageSpaceManager+0x1f0 in the end-of-frame chain, when Upscaling does not replace it): t0 the image, t1
			// the history, t2 the motion vectors, t3 the depth, t4 the TAA mask, t5 the alpha; into the image (SV_Target0) and the next
			// history (SV_Target1). Each call outside a scope is a scope of its own. The UI and water forms have no CS source path
			// (imagespace-shaders.md) and stay the engine's.
			{ Effects::ISTemporalAA, "ISTemporalAA", "taa", "ISTemporalAA.hlsl", 6, 0b111111, 6, 0, false, true, 2 },
			// The local map's edge pass (16094: kLOCAL_MAP_SWAP into kLOCAL_MAP, outside the frame's scopes: each call a scope of its own).
			{ Effects::ISLocalMap, "ISLocalMap", "localmap", "ISLocalMap.hlsl", 1, 0b1, 1, 0, false, true },
			// The snow's subsurface scattering (Main::RenderWorld, outside the frame's scopes: each call a scope of its own): t0 the depth,
			// t1 the snow's diffuse, t2 its alpha and specular.
			{ Effects::ISSnowSSS, "ISSnowSSS", "snow", "ISSnowSSS.hlsl", 3, 0b111, 3, 0, false, true },
			// A colour fill (menu 3D's clear among others), with menu 3D's stencil test and scissor when the engine sets them.
			{ Effects::ISSimpleColor, "ISSimpleColor", "simplecolor", "ISSimpleColor.hlsl", 0, 0, 0, 0, false, true },
			// The race menu's face tint (outside the frame's scopes: each call a scope of its own): sixteen loaded tint masks, each read
			// by its heap index from the draw's latch (EffectInfo::latched), blended over grey.
			{ Effects::ISAlphaBlend, "ISAlphaBlend", "alphablend", "ISAlphaBlend.hlsl", 16, 0xFFFF, 16, 0, false, true, 1, false, false, false, false, false, 0xFFFF },
			// The screen-space reflections' apply (t0 the blurred reflections, t1 the water mask, t2 the image). Its own scope outside the
			// other scopes, part of the enclosing one inside them.
			{ Effects::ISApplyReflections, "ISApplyReflections", "applyreflections", "ISApplyReflections.hlsl", 3, 0b111, 3, 0, false, true },
			// The lighting composite (albedo, diffuse, specular, SAO, fog, the directional light's diffuse and specular, the shadow mask):
			// the directional-light form reads all eight, the plain form no directional light, the menu form no SAO or shadow mask.
			{ Effects::ISLightingComposite, "ISLightingComposite", "lightingcomposite", "ISLightingComposite.hlsl", 8, 0xFF, 8, 0, false, true },
			{ Effects::ISLightingCompositeNoDirectionalLight, "ISLightingCompositeNoDirectionalLight", "lightingcomposite", "ISLightingComposite.hlsl", 8, 0b10011111, 8, 0,
				false, true },
			{ Effects::ISLightingCompositeMenu, "ISLightingCompositeMenu", "lightingcomposite", "ISLightingComposite.hlsl", 8, 0b01110111, 8, 0, false, true },
			// The dynamic-resolution upsample (the end-of-frame chain's +0x1f8): the full quad, its coordinates scaled in the shader.
			{ Effects::ISUpsampleDynamicResolution, "ISUpsampleDynamicResolution", "upsample", "ISUpsampleDynamicResolution.hlsl", 1, 0b1, 1, 0, false, true },
			// The effects whose vertex shader writes no SV_Position (EffectInfo::rastersNothing): no pass, nothing drawn.
			{ Effects::ISMap, "ISMap", "nodraw", "ISMap.hlsl", 0, 0, 0, 0, false, true, 1, false, false, false, false, true },
			{ Effects::ISNoiseScrollAndBlend, "ISNoiseScrollAndBlend", "nodraw", "ISNoise.hlsl", 0, 0, 0, 0, false, true, 1, false, false, false, false, true },
			{ Effects::ISNoiseNormalmap, "ISNoiseNormalmap", "nodraw", "ISNoise.hlsl", 0, 0, 0, 0, false, true, 1, false, false, false, false, true },
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

		/** @brief As BlendDescOf, for the engine's depth-stencil state at a table index (depth mode * kStencilModes + stencil mode). */
		bool DepthDescOf(std::uint32_t a_index, D3D11_DEPTH_STENCIL_DESC& a_out);

		/** @brief Whether a depth-stencil state writes depth or stencil (the pass then declares its target read-write). */
		bool DepthWrites(const D3D11_DEPTH_STENCIL_DESC& a_desc)
		{
			const auto writes = [](const D3D11_DEPTH_STENCILOP_DESC& a_face) {
				return a_face.StencilFailOp != D3D11_STENCIL_OP_KEEP || a_face.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP || a_face.StencilPassOp != D3D11_STENCIL_OP_KEEP;
			};
			return (a_desc.DepthEnable && a_desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ||
			       (a_desc.StencilEnable && a_desc.StencilWriteMask != 0 && (writes(a_desc.FrontFace) || writes(a_desc.BackFace)));
		}

		/** @brief Every slot unread (-1). */
		constexpr std::array<std::int32_t, kMaxSlots> NoInputs()
		{
			std::array<std::int32_t, kMaxSlots> inputs{};
			inputs.fill(-1);
			return inputs;
		}

		/** @brief The engine's depth-stencil table's shape (RE::BSGraphics, p4.6-shaders-states.md "State tables"). */
		constexpr std::uint32_t kDepthModes = 6;
		constexpr std::uint32_t kStencilModes = 40;

		/**
		 * @brief What one pass is made for, by value (the shape manifest stores it): the effect, its outputs and inputs, its samplers,
		 * its blend state, its viewport, its depth-stencil target and modes, and its scissor. A compute shape's output is the render
		 * target behind its UAV (u0); it has no blend state, viewport, depth or scissor.
		 */
		struct ShapeKey
		{
			std::uint32_t effect = 0;   // kEffects index
			std::int32_t output = -1;   // RE::RENDER_TARGET, with a mip (WithMip)
			std::int32_t output1 = -1;  // SV_Target1's render target (EffectInfo::targets 2), or -1
			std::array<std::int32_t, kMaxSlots> inputs = NoInputs();  // kDepthInput / kCubeInput / kLatchSourceInput, or a render target (with a mip)
			std::array<std::uint32_t, kMaxSlots> address{};
			std::array<std::uint32_t, kMaxSlots> filter{};
			std::uint32_t blend = kNoBlend;  // PackBlend
			bool compute = false;
			// The engine's viewport as float bits: x, y, width, height in the output's pixels (of its mip), then the depth range. Values
			// within 1/64 of a whole pixel are that pixel (ReadViewport), so dynamic resolution's float products give one shape.
			std::array<std::uint32_t, 6> viewport{};
			// The depth-stencil target (RE::RENDER_TARGETS_DEPTHSTENCIL) when the draw tests or writes depth or stencil, else -1; its
			// depth and stencil modes (the engine's table) and the stencil reference.
			std::int32_t depth = -1;
			std::uint32_t depthMode = 0, stencilMode = 0, stencilRef = 0;
			// The scissor (left, top, right, bottom, as RSSetScissorRects got it) when the raster state scissors (the manager's
			// rectangle: menu 3D).
			bool scissored = false;
			std::array<std::int32_t, 4> scissor{};

			auto Tie() const
			{
				return std::tie(effect, output, output1, inputs, address, filter, blend, compute, viewport, depth, depthMode, stencilMode, stencilRef, scissored, scissor);
			}
			bool operator<(const ShapeKey& a_other) const { return Tie() < a_other.Tie(); }
			bool operator==(const ShapeKey& a_other) const { return Tie() == a_other.Tie(); }
		};

		/** @brief One pass: its immutable spec, made at a registration. Its program is taken when the pass is made (a graph build). */
		struct Spec
		{
			ShapeKey key;
			std::uint32_t entry = 0;  // gate entry, latch region (entry * 32) and order key
			std::string name;
			FrameAnchors::Texture output;
			FrameAnchors::Texture output1;
			std::array<FrameAnchors::Texture, kMaxSlots> inputs;
			rhi::Format format = rhi::Format::Unknown;        // the outputs' render target view formats
			rhi::Format format1 = rhi::Format::Unknown;
			FrameAnchors::Texture depth;                      // the depth-stencil target's import (ShapeKey::depth)
			rhi::Format depthFormat = rhi::Format::Unknown;   // its depth-stencil view's format
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

		/** @brief OrgEngineSourcesAt in Common/OrgEngineConstants.hlsli: a latched-sources effect's heap index per texture slot. */
		using SourcesAt = std::array<std::uint32_t, kMaxSlots>;
		static_assert(sizeof(SourcesAt) == 64);

		/** @brief A gate entry's bytes in a client's latch: its ConstantsAt, then its SourcesAt. */
		constexpr std::uint32_t kLatchStride = static_cast<std::uint32_t>(sizeof(ConstantsAt) + sizeof(SourcesAt));

		/** @brief A taken draw or dispatch waiting for its batch's run: its pass, where its constants are, and a dispatch's groups. */
		struct Pending
		{
			std::uint32_t entry = 0;
			ConstantsAt at;
			bool compute = false;
			std::array<std::uint32_t, 3> groups{};
			std::shared_ptr<const void> source;  // the descriptor and texture behind at.source, held until the run's GPU work retires
			std::uint32_t vertices = 3;          // a draw's vertex count: the fullscreen triangle's, or a geometry effect's indices
			std::optional<SourcesAt> sources;    // the latched slots' heap indices (EffectInfo::latched); source holds them
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
		 * @brief A shape first seen in play (not in the session's registration): a client of its own with one pass, registered on first
		 * sight. FrameAnchors::Register asks the build manager for the anchor's graph, which builds it off the render thread; the main
		 * registration keeps running meanwhile. Once the built graph holds it, each of its draws is a run of its own (the main batch runs
		 * first). The manifest has the shape from the next session, which registers it with the rest at Setup.
		 */
		struct LateClient
		{
			std::string id;
			std::shared_ptr<const Spec> spec;  // entry 0 of its own gate and latch
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> latch;
			std::atomic<bool> made = false;  // the last build made its pass (set on the build thread)
			bool failed = false;             // its epoch failed with it: its draws stay with the engine
			bool ready = false;              // its first draw on the graph was logged
			std::vector<Pending> batch;      // the draw its run takes (one)
		};

		/**
		 * @brief One of the post segments' clients (RegisterPost): its table rows' passes, by row index, with its own gate and latch
		 * (a segment closes its clients' gates when its ticket is taken, and the two post segments' runs are different runs).
		 */
		struct PostClient
		{
			const char* id = nullptr;
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> latch;
			std::vector<std::shared_ptr<const Spec>> specs;
			std::array<const Spec*, kGateEntries> byRow{};  // the pass of each table row this client draws, by row index
			std::array<bool, kGateEntries> made{};          // the last build made it (set on the build thread)
			std::vector<Pending> batch;                     // the draws its next tap commits
		};

		/** @brief At most this many shapes are built on first sight in a session: more means something draws a new shape each frame. */
		constexpr std::size_t kMaxLateClients = 64;

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
		 * @brief A raster program's key: the effect, the outputs' formats, the packed blend state, the depth-stencil format (Unknown:
		 * none) and the depth-stencil table index (depth mode * kStencilModes + stencil mode).
		 */
		using ProgramKey = std::tuple<std::size_t, rhi::Format, rhi::Format, std::uint32_t, rhi::Format, std::uint32_t>;

		/**
		 * @brief One shape of the manifest: where it is in the frame, the last session that saw it, and how many sessions its effect had
		 * run in at that session (the effect's count in ImageSpaceState::effectSessions).
		 */
		struct ManifestEntry
		{
			std::uint32_t position = 0;  // scope << 16 | draw within the scope
			std::uint32_t lastSession = 0;
			std::uint32_t lastEffectSession = 0;
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
			// By run: the sources a run's latched draws read by heap index (Pending::source), held until the run's lease index runs
			// again (its last run retired before the index was opened again). Several commits into one run (regions, a stage's taps)
			// add to its list. A stage's commit moves its staging run's list onto the run that submits it (RunSlotsRegistry::MoveAll),
			// so a staging run, which names the same run every frame, holds only the frame's own sources.
			RunSlots<std::vector<std::shared_ptr<const void>>> heldSources;
			// Regions: the last gate entry the last commit opened. A later batch joins the same run only with higher entries (its passes
			// run after those already opened, and no entry is opened twice).
			std::uint32_t committedLast = 0;
			// The registration: every pass, by entry and by shape, and the engine objects its imports were made from (a resize check).
			std::vector<std::shared_ptr<const Spec>> specs;
			std::map<ShapeKey, const Spec*> specOf;
			std::map<std::int32_t, const void*> importedObjects;
			std::set<ShapeKey> refusedShapes;    // shapes no pass can be made for (logged once)
			// Shapes first seen in play, each a client of its own built on first sight (LateClient), by shape.
			std::map<ShapeKey, std::shared_ptr<LateClient>> late;
			std::uint32_t lateSerial = 0;
			std::set<ShapeKey> loggedConstants;  // shapes whose constant sizes were logged (CS_ORG_IMAGESPACE_LOG)
			std::uint32_t registrations = 0;
			// The shape manifest (Data/ShaderCache/v2/ImageSpaceShapes.json): read at Setup, written when a session sees new shapes.
			std::map<ShapeKey, ManifestEntry> manifest;
			std::set<ShapeKey> seen;  // shapes drawn or refused this session
			std::uint32_t session = 0;
			// By effect: the sessions before this one in which the effect drew a shape. This session counts once it draws one.
			std::map<std::size_t, std::uint32_t> effectSessions;
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
			// The same for the engine's depth-stencil states, by table index (depth mode * kStencilModes + stencil mode).
			std::map<std::uint32_t, D3D11_DEPTH_STENCIL_DESC> depthDescs;
			// The last scissor the engine set (75564 / 77365: RSSetScissorRects, left, top, right, bottom). The raster state's scissor
			// mode says whether a draw uses it. scissorKnown: one was set this session.
			std::array<std::int32_t, 4> scissor{};
			bool scissorKnown = false;
			// The post segments drawing from the pass-state table (RegisterPost): on for the session once registered; which part of this
			// frame's chain draws from it (OnPostPart: the composites, the end-of-frame effects); the constant chunks their draws read, held
			// for the frame; a registration asked for at the next frame (a row made for another size).
			bool postOn = false;
			bool postRegister = false;
			std::array<PostClient, 5> post{};
			std::array<bool, 2> partDrawing{};
			// The composites part cannot draw from the table this frame: the post composites segment is cut at the composites' call, after
			// the volumetric lighting (whose anchor is the segment's), so that work is submitted before today's path draws the composites.
			std::string compositesCut;
			// Between RenderSSAO's two calls in Main::RenderWorld: an own-scope copy-subregion call there is the snow scope's copy.
			bool snowWindow = false;
			std::vector<std::shared_ptr<const org::LatchBlock>> postKeep;
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
			if (a_name & kDefaultTextureInput)
				return fmt::format("default{}", IndexOfName(a_name));
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
			for (const auto& [key, late] : state.late)
				FrameAnchors::Unregister(late->id);
			state.late.clear();
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
		 * another's texture), a depth-stencil target's depth SRV as kDepthInput | index, or -1. kFRAMEBUFFER is one when the engine
		 * made an SRV of the back buffer (the screenshot copies read it): FrameAnchors::EngineTarget imports the back buffer with that
		 * view (in native mode its graph-owned slot), so the pass declares the read.
		 */
		/** @brief The engine's default textures an input may be: BSGraphics::State's black, white, grey and flat normal map. */
		constexpr std::uint32_t kDefaultTextures = 4;

		/** @brief Default texture a_index's renderer texture (kDefaultTextureInput), or null before the engine made it. */
		RE::BSGraphics::Texture* DefaultTextureOf(std::uint32_t a_index)
		{
			auto* state = globals::game::graphicsState;
			if (!state)
				return nullptr;
			auto& data = state->GetRuntimeData();
			RE::NiSourceTexture* texture = nullptr;
			switch (a_index) {
			case 0:
				texture = data.defaultTextureBlack.get();
				break;
			case 1:
				texture = data.defaultTextureWhite.get();
				break;
			case 2:
				texture = data.defaultTextureGrey.get();
				break;
			case 3:
				texture = data.defaultTextureNormalMap.get();
				break;
			default:
				break;
			}
			return texture ? texture->rendererTexture : nullptr;
		}

		std::int32_t InputOfView(ID3D11ShaderResourceView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return -1;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			if (targets[RE::RENDER_TARGET::kFRAMEBUFFER].SRV == a_view)
				return static_cast<std::int32_t>(RE::RENDER_TARGET::kFRAMEBUFFER);
			for (std::uint32_t i = RE::RENDER_TARGET::kMAIN; i < RE::RENDER_TARGET::kTOTAL; ++i)
				if (targets[i].SRV == a_view)
					return targets[i].texture ? static_cast<std::int32_t>(i) : MipInputOfView(a_view);
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			for (std::uint32_t i = 0; i < RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL; ++i)
				if (depths[i].depthSRV == a_view)
					return kDepthInput | static_cast<std::int32_t>(i);
			for (std::uint32_t i = 0; i < kDefaultTextures; ++i)
				if (auto* texture = DefaultTextureOf(i); texture && texture->resourceView == a_view)
					return kDefaultTextureInput | static_cast<std::int32_t>(i);
			return -1;
		}

		/**
		 * @brief Names the texture behind a refused input, once per effect and slot: its debug name, size and format, so a refusal says
		 * which texture is not an engine target.
		 */
		void LogForeignInput(const EffectInfo& a_effect, std::uint32_t a_slot, ID3D11ShaderResourceView* a_view)
		{
			static std::mutex lock;
			static std::set<std::pair<const void*, std::uint32_t>> logged;
			{
				std::lock_guard guard(lock);
				if (!logged.emplace(&a_effect, a_slot).second)
					return;
			}
			std::string name = "(no view)";
			D3D11_TEXTURE2D_DESC desc{};
			if (a_view) {
				winrt::com_ptr<ID3D11Resource> resource;
				a_view->GetResource(resource.put());
				char text[128]{};
				UINT size = sizeof(text) - 1;
				name = resource && SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, text)) ? std::string(text, size) : std::string("(unnamed)");
				if (auto texture = resource.try_as<ID3D11Texture2D>())
					texture->GetDesc(&desc);
			}
			logger::info("[ORG] ImageSpace: {} slot {} reads '{}' ({}x{}, DXGI format {}), which is not an engine target", a_effect.effectName, a_slot, name,
				desc.Width, desc.Height, static_cast<int>(desc.Format));
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

		/** @brief The flow map's import (kFlowMapId), or null without a flow map. */
		FrameAnchors::Texture FlowMapImport()
		{
			auto* map = FlowMap();
			static_assert(kFlowMapId == "cs.d3d11.engine.flow-map", "kFlowMapId is FrameAnchors::D3D11Texture's id for engine.flow-map");
			return map ? FrameAnchors::D3D11Texture("engine.flow-map", FlowMapTexture(), map->resourceView, map->UAV) : nullptr;
		}

		/**
		 * @brief Publishes the flow map's import under its id, with the view the water's draws find bound at t8 as they unwrap it, so they
		 * declare the copies' resource and read it by id (WaterDraws' inputs). Main thread, at Setup.
		 */
		void PublishFlowMap()
		{
			auto* map = FlowMap();
			const auto import = FlowMapImport();
			if (!map || !import || !PublishedResources::PublishImport(kFlowMapId, import, Tripwire::Unwrap(map->resourceView)))
				logger::warn("[ORG] ImageSpace: the flow map is not published; the water's draws import its view");
		}

		/** @brief The graph's import of a target as ShapeKey names it (a render target, a depth target, a cubemap or the flow map; any mip is its texture's). */
		FrameAnchors::Texture ImportOf(std::int32_t a_name)
		{
			if (a_name < 0 || (a_name & kLatchSourceInput))
				return nullptr;
			if (a_name & kFlowMapOutput)
				return FlowMapImport();
			const auto index = IndexOfName(a_name);
			if (a_name & kDefaultTextureInput) {
				auto* texture = DefaultTextureOf(index);
				return texture ? FrameAnchors::ImmutableTexture(fmt::format("engine.default-texture.{}", index), static_cast<ID3D11Texture2D*>(texture->texture), texture->resourceView) : nullptr;
			}
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
			if (a_name & kDefaultTextureInput) {
				auto* texture = DefaultTextureOf(index);
				return texture ? texture->texture : nullptr;
			}
			if (a_name & kDepthInput)
				return index < RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL ? renderer->GetDepthStencilData().depthStencils[index].texture : nullptr;
			if (a_name & kCubeInput)
				return index < RE::RENDER_TARGETS_CUBEMAP::kTOTAL ? renderer->GetRendererData().cubemapRenderTargets[index].texture : nullptr;
			if (index >= RE::RENDER_TARGET::kTOTAL)
				return nullptr;
			const auto& target = renderer->GetRuntimeData().renderTargets[index];
			return target.texture ? static_cast<const void*>(target.texture) : static_cast<const void*>(target.RTV);
		}

		/** @brief The format of an engine depth-stencil target's first depth-stencil view, or Unknown. */
		rhi::Format DsvFormatOf(std::uint32_t a_depth)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || a_depth >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL)
				return rhi::Format::Unknown;
			auto* view = renderer->GetDepthStencilData().depthStencils[a_depth].views[0];
			if (!view)
				return rhi::Format::Unknown;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			Tripwire::Unwrap(view)->GetDesc(&desc);
			return rhi::helpers::ToRHI(desc.Format);
		}

		/** @brief The format of an output's render target view (the program's target format), from the target or the texture it is a mip of. */
		rhi::Format RtvFormatOf(std::int32_t a_name, const FrameAnchors::Texture& a_import)
		{
			const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			if (auto* rtv = targets[IndexOfName(a_name)].RTV) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				rtv->GetDesc(&view);
				return rhi::helpers::ToRHI(view.Format);
			}
			return a_import ? a_import.Format() : rhi::Format::Unknown;
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
				// A loaded texture, read by its heap index from the draw's latch (TakeDraw resolves the shadow state's view).
				if (a_effect.latched & (1u << slot)) {
					a_out.inputs[slot] = kLatchSourceInput;
					if (slot < a_effect.samplers) {
						a_out.address[slot] = a_address[slot].underlying();
						a_out.filter[slot] = a_filter[slot].underlying();
					}
					continue;
				}
				const auto input = InputOfView(a_views[slot]);
				if (input < 0) {
					a_refusal = "an input that is not an engine target";
					LogForeignInput(a_effect, slot, a_views[slot]);
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

		/** @brief A shape's viewport as floats (ShapeKey::viewport). */
		rhi::Viewport ViewportOf(const ShapeKey& a_key)
		{
			const auto& bits = a_key.viewport;
			return { .x = std::bit_cast<float>(bits[0]),
				.y = std::bit_cast<float>(bits[1]),
				.width = std::bit_cast<float>(bits[2]),
				.height = std::bit_cast<float>(bits[3]),
				.minDepth = std::bit_cast<float>(bits[4]),
				.maxDepth = std::bit_cast<float>(bits[5]) };
		}

		/** @brief ShapeKey::viewport of a viewport. */
		std::array<std::uint32_t, 6> ViewportBits(const rhi::Viewport& a_viewport)
		{
			return { std::bit_cast<std::uint32_t>(a_viewport.x), std::bit_cast<std::uint32_t>(a_viewport.y), std::bit_cast<std::uint32_t>(a_viewport.width),
				std::bit_cast<std::uint32_t>(a_viewport.height), std::bit_cast<std::uint32_t>(a_viewport.minDepth), std::bit_cast<std::uint32_t>(a_viewport.maxDepth) };
		}

		/**
		 * @brief The pass's render area for a shape drawn into an output of a_width x a_height: the viewport's pixels inside the output
		 * (D3D11 clips the rest), rounded outward. The viewport itself goes to the draw when it is not exactly that area
		 * (AnchorPasses::Draw::dynamicViewport), so TEXCOORD0 spans the engine's viewport. False when nothing of it is inside.
		 */
		bool RenderAreaOf(const ShapeKey& a_key, std::uint32_t a_width, std::uint32_t a_height, RasterProgram::Viewport& a_area, bool& a_exact)
		{
			const auto viewport = ViewportOf(a_key);
			const double left = (std::max)(std::floor(static_cast<double>(viewport.x)), 0.0);
			const double top = (std::max)(std::floor(static_cast<double>(viewport.y)), 0.0);
			const double right = (std::min)(std::ceil(static_cast<double>(viewport.x) + viewport.width), static_cast<double>(a_width));
			const double bottom = (std::min)(std::ceil(static_cast<double>(viewport.y) + viewport.height), static_cast<double>(a_height));
			if (!(right > left) || !(bottom > top))
				return false;
			a_area = { .x = static_cast<std::uint32_t>(left),
				.y = static_cast<std::uint32_t>(top),
				.width = static_cast<std::uint32_t>(right - left),
				.height = static_cast<std::uint32_t>(bottom - top),
				.minDepth = viewport.minDepth,
				.maxDepth = viewport.maxDepth };
			a_exact = viewport.x == static_cast<float>(a_area.x) && viewport.y == static_cast<float>(a_area.y) && viewport.width == static_cast<float>(a_area.width) &&
			          viewport.height == static_cast<float>(a_area.height);
			return true;
		}

		/**
		 * @brief The viewport the engine's draw uses, from the shadow state (RendererShadowState::viewPort, +0x70: what SetDirtyStates
		 * applies): the whole target (Render's UpdateViewPort), the caller's (the explicit-target Render), the one a custom-viewport
		 * effect keeps (ImageSpaceEffect::unk88: Render leaves the viewport as it is), or the dynamic-resolution part of the target. Never
		 * the D3D11 context's: under the flush latch it is stale. It may be fractional or reach past the output: the pass draws its
		 * pixels inside the output as its render area, with the viewport itself as the draw's (RenderAreaOf). False with a reason (and
		 * the viewport as a_detail) when it is empty or entirely outside the output.
		 */
		bool ReadViewport(const OutputView& a_output, ShapeKey& a_key, std::string_view& a_refusal, std::string& a_detail)
		{
			const auto& viewport = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().viewPort;
			a_detail = fmt::format("{}x{} at ({}, {}) in {}x{}, depth {} to {}", viewport.Width, viewport.Height, viewport.TopLeftX, viewport.TopLeftY, a_output.width,
				a_output.height, viewport.MinDepth, viewport.MaxDepth);
			// The dynamic-resolution sizes are float products. Within 1/64 of a whole pixel, the pixels covered are the same and
			// TEXCOORD0 differs by far less than a texel: such a value is the whole pixel, so the shape is the same every frame.
			const auto snap = [](float a_value, float a_tolerance) {
				const float rounded = std::round(a_value);
				return std::abs(a_value - rounded) <= a_tolerance ? rounded : a_value;
			};
			rhi::Viewport read{ .x = snap(viewport.TopLeftX, 1.0f / 64.0f),
				.y = snap(viewport.TopLeftY, 1.0f / 64.0f),
				.width = snap(viewport.Width, 1.0f / 64.0f),
				.height = snap(viewport.Height, 1.0f / 64.0f),
				// The dynamic-resolution viewport's depth range is a float product too (0 to 0.99998 seen).
				.minDepth = snap(viewport.MinDepth, 1e-3f),
				.maxDepth = snap(viewport.MaxDepth, 1e-3f) };
			if (!std::isfinite(read.x) || !std::isfinite(read.y) || !std::isfinite(read.width) || !std::isfinite(read.height) || !(read.width > 0.0f) ||
				!(read.height > 0.0f) || !std::isfinite(read.minDepth) || !std::isfinite(read.maxDepth)) {
				a_refusal = "an empty viewport";
				return false;
			}
			a_key.viewport = ViewportBits(read);
			RasterProgram::Viewport area;
			bool exact = false;
			if (!RenderAreaOf(a_key, a_output.width, a_output.height, area, exact)) {
				a_refusal = "a viewport entirely outside its target";
				return false;
			}
			return true;
		}

		/** @brief Whether a render target index is one of the engine's table (kFRAMEBUFFER included). */
		bool IsEngineTarget(std::int32_t a_target)
		{
			return a_target >= static_cast<std::int32_t>(RE::RENDER_TARGET::kFRAMEBUFFER) && a_target < static_cast<std::int32_t>(RE::RENDER_TARGET::kTOTAL);
		}

		/**
		 * @brief The draw's shape from the shadow state, as SetDirtyStates is about to apply it (all but the viewport: ReadViewport
		 * reads it once SetDirtyStates ran); false with a reason when no pass can draw it. a_clears: SetDirtyStates clears the target
		 * as it binds it (the caller runs the pending batch first, so the clear lands between it and this draw).
		 */
		bool ReadShape(std::size_t a_effect, ShapeKey& a_out, OutputView& a_output, std::string_view& a_refusal, bool& a_clears)
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
			// A clear is applied by SetDirtyStates when the target is bound, through the D3D11 context (the native context and DXVK keep
			// it in the stream's order). The pending batch must run before it.
			constexpr std::uint32_t kDirtyRenderTarget = 0x1;
			const bool dirtyTargets = (shadow.stateUpdateFlags.underlying() & kDirtyRenderTarget) != 0;
			a_clears = dirtyTargets && shadow.setRenderTargetMode[0] <= RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR_STENCIL;
			// The engine leaves a depth target bound through the chain with the test off: the pass binds none then, and the draw is the
			// same. With a depth or stencil mode the target is the pass's depth attachment, with the engine's state for those modes.
			const auto depthMode = static_cast<std::uint32_t>(shadow.depthStencilDepthMode);
			const auto stencilMode = shadow.depthStencilStencilMode;
			if (shadow.depthStencil != UINT32_MAX && (depthMode != 0 || stencilMode != 0)) {
				if (shadow.depthStencil >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL || shadow.depthStencilSlice != 0) {
					a_refusal = "a depth target outside the engine's table, or a slice of one";
					return false;
				}
				D3D11_DEPTH_STENCIL_DESC desc{};
				if (depthMode >= kDepthModes || stencilMode >= kStencilModes || !DepthDescOf(depthMode * kStencilModes + stencilMode, desc)) {
					a_refusal = "a depth or stencil mode outside the engine's table";
					return false;
				}
				a_out.depth = static_cast<std::int32_t>(shadow.depthStencil);
				a_out.depthMode = depthMode;
				a_out.stencilMode = stencilMode;
				a_out.stencilRef = desc.StencilEnable ? (shadow.stencilRef & 0xFF) : 0u;
				// A depth clear at the bind (modes 0 to 2 and 6) goes to D3D11 in the stream's order too.
				const auto depthClear = shadow.setDepthStencilMode;
				a_clears = a_clears || (dirtyTargets && (depthClear <= RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR_STENCIL ||
															depthClear == RE::BSGraphics::SetRenderTargetMode::SRTM_INIT));
			}
			if (!ResolveOutput(output, a_output, a_refusal))
				return false;
			// The manager's rectangle (menu 3D): BSImagespaceShader::Render turns the raster state's scissor on and sets the rectangle
			// through 75564 (RSSetScissorRects, checked in the SE binary), which the Scissor hook keeps.
			if (shadow.rasterStateScissorMode != 0) {
				if (!IS().scissorKnown) {
					a_refusal = "a scissor the engine did not set through its scissor function";
					return false;
				}
				a_out.scissored = true;
				a_out.scissor = IS().scissor;
			}
			a_out.output = a_output.name;
			if (!ReadInputs(effect, shadow.PSTexture, shadow.PSTextureAddressMode, shadow.PSTextureFilterMode, a_out, a_refusal))
				return false;
			// One pass cannot read a depth target as a texture and bind it as its depth attachment.
			if (a_out.depth >= 0 && std::ranges::find(a_out.inputs, kDepthInput | a_out.depth) != a_out.inputs.end()) {
				a_refusal = "it reads the depth target it tests against";
				return false;
			}
			const auto draw = Pipeline::EngineDrawState::Latch();
			a_out.blend = PackBlend(draw.blendMode, draw.alphaToCoverage, draw.writeMode, draw.blendExtra);
			if (D3D11_BLEND_DESC desc{}; !BlendDescOf(a_out.blend, desc)) {
				a_refusal = "a blend state outside the engine's table";
				return false;
			}
			return true;
		}

		/** @brief The engine's sampler table's shape: 6 address modes by 5 filter modes (p4.6-shaders-states.md "State tables"). */
		constexpr std::uint32_t kSamplerAddressModes = 6;
		constexpr std::uint32_t kSamplerFilterModes = 5;

		/**
		 * @brief The description of the engine's sampler for an address and filter mode, as the renderer makes its table (SE 75831
		 * 0x140d89480, read in the binary; AE 77653): the address modes clamp, U clamp and V wrap, U wrap and V clamp, wrap, border
		 * (black) and mirror (W follows U); the filters point and linear without mips (MaxLOD 0), trilinear, anisotropic and the
		 * comparison sampler (LESS), with MinLOD -FLT_MAX. The anisotropy is CS's cap (EngineHooks' CreateSamplerState detour). Made
		 * from the modes alone, so a pass has its sampler before the engine made its table. False for modes outside the table.
		 */
		bool EngineSamplerDesc(std::uint32_t a_address, std::uint32_t a_filter, D3D11_SAMPLER_DESC& a_out)
		{
			if (a_address >= kSamplerAddressModes || a_filter >= kSamplerFilterModes)
				return false;
			constexpr std::array<std::array<D3D11_TEXTURE_ADDRESS_MODE, 2>, kSamplerAddressModes> kAddress{ {
				{ D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP },
				{ D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP },
				{ D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_CLAMP },
				{ D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP },
				{ D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_BORDER },
				{ D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_MIRROR },
			} };
			constexpr std::array<D3D11_FILTER, kSamplerFilterModes> kFilter{ D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
				D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FILTER_ANISOTROPIC, D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT };
			// CS caps the engine's anisotropy at 8 (EngineHooks.cpp, kMaxSamplerAnisotropy); the engine asks for 16.
			constexpr UINT kAnisotropy = 8;
			a_out = {};
			a_out.Filter = kFilter[a_filter];
			a_out.AddressU = a_out.AddressW = kAddress[a_address][0];
			a_out.AddressV = kAddress[a_address][1];
			a_out.MipLODBias = 0.0f;
			a_out.MaxAnisotropy = a_filter >= 3 ? kAnisotropy : 1;
			// The engine leaves LESS set after the comparison sampler; only that filter reads it.
			a_out.ComparisonFunc = a_filter == 4 ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_NEVER;
			a_out.MinLOD = -FLT_MAX;
			a_out.MaxLOD = a_filter <= 1 ? 0.0f : FLT_MAX;
			return true;
		}

		/** @brief The heap index of the ORG sampler for an engine address and filter mode (EngineSamplerDesc), made on first use; UINT32_MAX on failure. */
		std::uint32_t SamplerFor(std::uint32_t a_address, std::uint32_t a_filter)
		{
			auto& state = IS();
			const auto key = std::make_pair(a_address, a_filter);
			if (const auto it = state.samplers.find(key); it != state.samplers.end())
				return it->second ? it->second.Index() : UINT32_MAX;
			D3D11_SAMPLER_DESC desc{};
			if (!EngineSamplerDesc(a_address, a_filter, desc))
				return UINT32_MAX;
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

		bool DepthDescOf(std::uint32_t a_index, D3D11_DEPTH_STENCIL_DESC& a_out)
		{
			auto& state = IS();
			if (auto* object = Pipeline::EngineDepthStencilState(a_index / kStencilModes, a_index % kStencilModes)) {
				object->GetDesc(&a_out);
				state.depthDescs[a_index] = a_out;
				return true;
			}
			const auto kept = state.depthDescs.find(a_index);
			if (kept == state.depthDescs.end())
				return false;
			a_out = kept->second;
			return true;
		}

		/** @brief A depth-stencil description as the manifest keeps it: the depth fields, the stencil masks, then each face's four ops. */
		json DepthToJson(const D3D11_DEPTH_STENCIL_DESC& a_desc)
		{
			json out = json::array({ static_cast<std::int64_t>(a_desc.DepthEnable), static_cast<std::int64_t>(a_desc.DepthWriteMask),
				static_cast<std::int64_t>(a_desc.DepthFunc), static_cast<std::int64_t>(a_desc.StencilEnable), static_cast<std::int64_t>(a_desc.StencilReadMask),
				static_cast<std::int64_t>(a_desc.StencilWriteMask) });
			for (const auto* face : { &a_desc.FrontFace, &a_desc.BackFace }) {
				out.push_back(static_cast<std::int64_t>(face->StencilFailOp));
				out.push_back(static_cast<std::int64_t>(face->StencilDepthFailOp));
				out.push_back(static_cast<std::int64_t>(face->StencilPassOp));
				out.push_back(static_cast<std::int64_t>(face->StencilFunc));
			}
			return out;
		}

		/** @brief The manifest's form of a depth-stencil description read back; false for a malformed one. */
		bool DepthFromJson(const json& a_item, D3D11_DEPTH_STENCIL_DESC& a_out)
		{
			if (!a_item.is_array() || a_item.size() != 14)
				return false;
			try {
				a_out.DepthEnable = a_item[0].get<std::int64_t>() != 0;
				a_out.DepthWriteMask = static_cast<D3D11_DEPTH_WRITE_MASK>(a_item[1].get<std::int64_t>());
				a_out.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(a_item[2].get<std::int64_t>());
				a_out.StencilEnable = a_item[3].get<std::int64_t>() != 0;
				a_out.StencilReadMask = static_cast<UINT8>(a_item[4].get<std::int64_t>());
				a_out.StencilWriteMask = static_cast<UINT8>(a_item[5].get<std::int64_t>());
				std::size_t at = 6;
				for (auto* face : { &a_out.FrontFace, &a_out.BackFace }) {
					face->StencilFailOp = static_cast<D3D11_STENCIL_OP>(a_item[at++].get<std::int64_t>());
					face->StencilDepthFailOp = static_cast<D3D11_STENCIL_OP>(a_item[at++].get<std::int64_t>());
					face->StencilPassOp = static_cast<D3D11_STENCIL_OP>(a_item[at++].get<std::int64_t>());
					face->StencilFunc = static_cast<D3D11_COMPARISON_FUNC>(a_item[at++].get<std::int64_t>());
				}
			} catch (const std::exception&) {
				return false;
			}
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
			// Immutable, made with its one texel and never written: the class the graph exempts from declarations, so a bindless index
			// is its read.
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

		/**
		 * @brief The ORG depth-stencil state of a ProgramKey's table index, and whether it writes; false when neither the engine's table
		 * nor the manifest has the entry yet.
		 */
		bool DepthStateFor(std::uint32_t a_index, rhi::DepthStencilState& a_out, bool* a_writes = nullptr)
		{
			D3D11_DEPTH_STENCIL_DESC desc{};
			if (!DepthDescOf(a_index, desc))
				return false;
			a_out = Pipeline::DepthStencilFromD3D11(desc);
			if (a_writes)
				*a_writes = DepthWrites(desc);
			return true;
		}

		/**
		 * @brief The raster program's description for a key (the blend was checked by BlendFor, the depth-stencil state by DepthStateFor).
		 * Needs the engine's effect object.
		 */
		RasterProgram::Desc DescribeRaster(const ProgramKey& a_key)
		{
			const auto& [effectIndex, format, format1, blendBits, depthFormat, depthIndex] = a_key;
			const auto& effect = kEffects[effectIndex];
			std::array<rhi::BlendAttachment, 2> blend{};
			std::string error;
			BlendFor(blendBits, effect.targets, blend, error);
			RasterProgram::Desc desc{};
			desc.pixelSource = effect.source;
			// A geometry effect's vertex stage is its source's ORG build, which pulls the geometry (TakeDraw writes where it is); a
			// vertexStage effect's moves the fullscreen triangle's coordinates.
			if (effect.geometry || effect.vertexStage)
				desc.vertexSource = effect.source;
			// FXC-era sources (vector conditions in ?:).
			desc.languageVersion = L"2018";
			// Shared (4 words), the SRVs, the samplers, the feature handles, the latch (2), the sources' latch (2).
			desc.constantWords = 4 + effect.slots - static_cast<std::uint32_t>(std::popcount(effect.latched)) + effect.samplers +
			                     (effect.features ? kFeatureWords : 0) + 2 + (effect.latched ? 2 : 0);
			// The stage defines are RasterProgram's own.
			desc.defines = DefinesOf(effectIndex);
			desc.targets = { { .format = format, .blend = blend[0] } };
			if (format1 != rhi::Format::Unknown)
				desc.targets.push_back({ .format = format1, .blend = blend[1] });
			// A draw with a depth or stencil test (menu 3D's stencil composite and fill): the engine's state for its modes.
			if (depthFormat != rhi::Format::Unknown) {
				desc.depthFormat = depthFormat;
				DepthStateFor(depthIndex, desc.depth);
			}
			return desc;
		}

		/**
		 * @brief Whether a spec reads a texture slot through a sampler of its own: an imported input, or a latched source; not a cube
		 * slot CS binds with its own sampler.
		 */
		bool SlotRead(const Spec& a_spec, std::uint32_t a_slot)
		{
			const auto& effect = kEffects[a_spec.key.effect];
			if (a_slot >= kMaxSlots || (effect.cubes & (1u << a_slot)))
				return false;
			const auto name = a_spec.key.inputs[a_slot];
			return a_spec.inputs[a_slot] || (name >= 0 && (name & kLatchSourceInput));
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
		ProgramKey KeyOf(const Spec& a_spec)
		{
			const auto depthIndex = a_spec.depth ? a_spec.key.depthMode * kStencilModes + a_spec.key.stencilMode : 0u;
			return { a_spec.key.effect, a_spec.format, a_spec.format1, a_spec.key.blend, a_spec.depth ? a_spec.depthFormat : rhi::Format::Unknown, depthIndex };
		}

		/** @brief Whether a raster spec's program can be described now: its blend and depth-stencil states are known. a_error says which is not. */
		bool StatesKnown(const Spec& a_spec, std::string& a_error)
		{
			std::array<rhi::BlendAttachment, 2> blend{};
			if (!BlendFor(a_spec.key.blend, kEffects[a_spec.key.effect].targets, blend, a_error))
				return false;
			rhi::DepthStencilState depth{};
			if (a_spec.depth && !DepthStateFor(std::get<5>(KeyOf(a_spec)), depth)) {
				a_error = "no engine depth-stencil state";
				return false;
			}
			return true;
		}

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
				std::string error;
				if (!spec->key.compute && !StatesKnown(*spec, error))
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
			const std::shared_ptr<const RunGate>& a_gate, const char* a_clientId = kClientId)
		{
			const auto& spec = *a_spec;
			const auto& effect = kEffects[spec.key.effect];
			auto& state = IS();
			if (!HasDefines(spec.key.effect)) {
				logger::info("[ORG] ImageSpace: {}: the engine has not made its effect yet and the manifest has no defines for it; its pass joins the next session",
					spec.name);
				return nullptr;
			}
			// The engine's blend and depth-stencil tables are read here, at the graph's build, not at Setup (they are filled after it);
			// the manifest keeps the entries its shapes use. The samplers are made from their modes alone (EngineSamplerDesc).
			if (!spec.key.compute) {
				std::string error;
				if (!StatesKnown(spec, error)) {
					// Only an entry neither the table nor the manifest has is worth another build; a state with no ORG form stays out.
					D3D11_BLEND_DESC blend{};
					D3D11_DEPTH_STENCIL_DESC depth{};
					if (!BlendDescOf(spec.key.blend, blend) || (spec.depth && !DepthDescOf(spec.key.depthMode * kStencilModes + spec.key.stencilMode, depth)))
						state.retryBuild = true;
					logger::info("[ORG] ImageSpace: {}: its blend or depth-stencil state has no ORG form at the graph's build ({}); its pass is left out", spec.name,
						error);
					return nullptr;
				}
			}
			std::array<std::uint32_t, kMaxSlots> samplers{};
			for (std::uint32_t slot = 0; slot < effect.samplers; ++slot) {
				if (!SlotRead(spec, slot))
					continue;
				samplers[slot] = SamplerFor(spec.key.address[slot], spec.key.filter[slot]);
				if (samplers[slot] == UINT32_MAX) {
					logger::error("[ORG] ImageSpace: {}: sampler {} (address {}, filter {}) could not be made; its pass is left out", spec.name, slot,
						spec.key.address[slot], spec.key.filter[slot]);
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
			// A latched slot is read by the latch's heap index: no SRV entry.
			for (std::uint32_t slot = 0; slot < effect.slots; ++slot)
				if (!(effect.latched & (1u << slot)))
					bindings.push_back(spec.inputs[slot] ? AnchorPasses::Bind::Srv(spec.inputs[slot], MipOfName(spec.key.inputs[slot])) : AnchorPasses::Bind::Unused());
			for (std::uint32_t slot = 0; slot < effect.samplers; ++slot)
				bindings.push_back(SlotRead(spec, slot) ? AnchorPasses::Bind::Handle(samplers[slot]) : AnchorPasses::Bind::Unused());
			if (effect.features) {
				std::string missing;
				if (!FeatureBinds(spec.key.effect, bindings, missing)) {
					logger::info("[ORG] ImageSpace: {}: {} is not published (its feature's ORG path is off or has not run yet); its pass is left out until it is", spec.name, missing);
					return nullptr;
				}
			}
			if (effect.compute)
				bindings.push_back(AnchorPasses::Bind::Uav(spec.output));
			bindings.push_back(AnchorPasses::Bind::Latch(spec.entry * kLatchStride));
			if (effect.latched)
				bindings.push_back(AnchorPasses::Bind::Latch(spec.entry * kLatchStride + static_cast<std::uint32_t>(sizeof(ConstantsAt))));
			AnchorPasses::Builder builder(Anchor::ImageSpace, a_clientId, static_cast<std::int32_t>(spec.entry), a_latch, a_gate);
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
				// The engine's viewport (ReadViewport): the whole target, or a part of it (dynamic resolution, a custom viewport). Its
				// pixels inside the output are the render area; a viewport that is not exactly that area is the draw's own.
				const auto mip = MipOfName(spec.key.output);
				const auto shift = mip == AnchorPasses::kAllMips ? 0u : mip;
				bool exact = false;
				if (!RenderAreaOf(spec.key, (std::max)(spec.output.Width() >> shift, 1u), (std::max)(spec.output.Height() >> shift, 1u), draw.viewport, exact)) {
					logger::error("[ORG] ImageSpace: {}: its viewport is outside its output; its pass is left out of the graph", spec.name);
					return nullptr;
				}
				if (!exact)
					draw.dynamicViewport = ViewportOf(spec.key);
				if (spec.key.scissored) {
					const auto& rect = spec.key.scissor;
					draw.scissor = rhi::ScissorRect{ .x = rect[0],
						.y = rect[1],
						.width = static_cast<std::uint32_t>((std::max)(rect[2] - rect[0], 0)),
						.height = static_cast<std::uint32_t>((std::max)(rect[3] - rect[1], 0)) };
				}
				if (spec.depth) {
					bool writes = false;
					rhi::DepthStencilState depth{};
					DepthStateFor(std::get<5>(KeyOf(spec)), depth, &writes);
					draw.depth = spec.depth;
					draw.depthReadOnly = !writes;
					draw.stencilReference = spec.key.stencilRef;
				}
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
		 * are taken when the graph is built (MakePass): the engine fills its state tables after Setup. a_tag replaces the entry in its
		 * name (a shape built on first sight: every such pass is entry 0 of its own client).
		 */
		std::shared_ptr<const Spec> MakeSpec(const ShapeKey& a_key, std::uint32_t a_entry, std::string_view a_tag = {})
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
				// The viewport was read against the target's size when the shape was seen; some of it must be inside this session's import.
				const auto mip = MipOfName(a_key.output);
				const auto shift = mip == AnchorPasses::kAllMips ? 0u : mip;
				const auto width = (std::max)(spec->output.Width() >> shift, 1u);
				const auto height = (std::max)(spec->output.Height() >> shift, 1u);
				RasterProgram::Viewport area;
				bool exact = false;
				if (!RenderAreaOf(a_key, width, height, area, exact))
					refusal = "its viewport is outside its imported output (another resolution)";
				spec->format = RtvFormatOf(a_key.output, spec->output);
				if (spec->output1)
					spec->format1 = RtvFormatOf(a_key.output1, spec->output1);
				// The depth-stencil target: imported as the depth inputs are, drawn through its depth-stencil view's format.
				if (refusal.empty() && a_key.depth >= 0) {
					spec->depth = ImportOf(kDepthInput | a_key.depth);
					spec->depthFormat = DsvFormatOf(static_cast<std::uint32_t>(a_key.depth));
					if (!spec->depth || spec->depthFormat == rhi::Format::Unknown)
						refusal = "its depth target could not be imported";
				}
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
			spec->name = a_tag.empty() ? fmt::format("{}.{}.{}", effect.effectName, NameForLog(a_key.output), spec->entry) :
			                             fmt::format("{}.{}.{}", effect.effectName, NameForLog(a_key.output), a_tag);
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
				whole.viewport = ViewportBits({ .width = static_cast<float>(output.width), .height = static_cast<float>(output.height) });
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
			// The TAA histories swap by frame (the read history is the other frame's written one): a shape that writes one and reads the
			// other implies the other direction, so both are passes whichever the first session drew first. Applied after the pairs
			// above, so the kMAIN / kMAIN_COPY partners get theirs too.
			constexpr std::array kHistories{
				std::pair{ RE::RENDER_TARGET::kTEMPORAL_AA_ACCUMULATION_1, RE::RENDER_TARGET::kTEMPORAL_AA_ACCUMULATION_2 },
				std::pair{ RE::RENDER_TARGET::kTEMPORAL_AA_UI_ACCUMULATION_1, RE::RENDER_TARGET::kTEMPORAL_AA_UI_ACCUMULATION_2 },
				std::pair{ RE::RENDER_TARGET::kTEMPORAL_AA_WATER_1, RE::RENDER_TARGET::kTEMPORAL_AA_WATER_2 },
			};
			const auto withPairs = a_keys.size();
			for (std::size_t i = 0; i < withPairs; ++i)
				for (const auto& [first, second] : kHistories) {
					const std::pair names{ static_cast<std::int32_t>(first), static_cast<std::int32_t>(second) };
					if (Mentions(a_keys[i], names.first) || Mentions(a_keys[i], names.second))
						add(Swapped(a_keys[i], names), a_keys[i]);
				}
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
		 * @brief A client's commit: each pending draw's constants location written into a_latch at its entry and its gate entry opened.
		 * The sources it reads by heap index are held until the run lease index's next run (its last run retired before the index was
		 * opened again).
		 */
		void CommitBatch(const std::vector<Pending>& a_batch, org::LatchBlock& a_latch, RunGate& a_gate)
		{
			auto& state = IS();
			const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
			// Another batch of the same run adds to its sources; a new run of the index lets the last one's go.
			std::shared_ptr<std::vector<std::shared_ptr<const void>>> held;
			for (const auto& pending : a_batch)
				if (pending.source) {
					if (!held) {
						const auto earlier = state.heldSources.Find(run);
						held = earlier ? std::make_shared<std::vector<std::shared_ptr<const void>>>(*earlier) : std::make_shared<std::vector<std::shared_ptr<const void>>>();
					}
					held->push_back(pending.source);
				}
			if (held)
				state.heldSources.Store(run, std::move(held));
			for (const auto& pending : a_batch) {
				a_latch.WriteValue(run, pending.entry * kLatchStride, pending.at);
				if (pending.sources)
					a_latch.WriteValue(run, pending.entry * kLatchStride + static_cast<std::uint32_t>(sizeof(ConstantsAt)), *pending.sources);
				if (pending.compute)
					a_gate.OpenDispatch(pending.entry, pending.groups[0], pending.groups[1], pending.groups[2]);
				else
					a_gate.OpenDraw(pending.entry, pending.vertices);
			}
		}

		void NoteImports(const Spec& a_spec);
		void RegisterPost();
		void CheckWorldSteps();

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
					NoteImports(*spec);
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
					.atExecution = true,
					// Every draw and dispatch goes through the gate, and none clears: a region execution may hold the pass unopened.
					.gated = true,
					// So native mode records it ahead, and a closed entry draws or dispatches nothing on the GPU.
					.gpuGated = true });
			}
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			client.passes = std::move(passes);
			client.sharedConstants = true;
			client.gates = { state.gate };
			client.active = [] { return !IS().batch.empty(); };
			client.commit = [](const FrameAnchors::Point&) {
				auto& current = IS();
				CommitBatch(current.batch, *current.latch, *current.gate);
				if (!current.batch.empty())
					current.committedLast = current.batch.back().entry;
			};
			// Regions: a batch joins the run an earlier batch of this frame committed into when its passes all come after that batch's
			// (the batch's entries rise: AdmitShape starts a new batch at an entry at or before the last one).
			client.joins = [] {
				const auto& current = IS();
				return !current.batch.empty() && current.batch.front().entry > current.committedLast;
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

		/** @brief Notes the engine objects a spec's imports were made from, for the resize check (CheckImports). */
		void NoteImports(const Spec& a_spec)
		{
			auto& state = IS();
			for (const auto name : { a_spec.key.output, a_spec.key.output1 })
				if (name >= 0)
					state.importedObjects[ImportOfName(name)] = EngineObjectOf(name);
			for (const auto name : a_spec.key.inputs)
				if (name >= 0)
					state.importedObjects[ImportOfName(name)] = EngineObjectOf(name);
		}

		/**
		 * @brief The client of a shape first seen in play, registered now if it has none (LateClient): its program is queued on the
		 * program build thread, and FrameAnchors::Register asks for the anchor's graph, which the build manager builds off the render
		 * thread. Null (with a_refusal) when no pass can be made for the shape or the session built too many. Render thread.
		 */
		LateClient* LateFor(const ShapeKey& a_key, std::string_view& a_refusal)
		{
			auto& state = IS();
			if (const auto found = state.late.find(a_key); found != state.late.end())
				return found->second.get();
			if (state.late.size() >= kMaxLateClients) {
				a_refusal = "the session built too many shapes on first sight";
				return nullptr;
			}
			const auto& effect = kEffects[a_key.effect];
			auto late = std::make_shared<LateClient>();
			late->id = fmt::format("{}.late.{}", kClientId, ++state.lateSerial);
			late->spec = MakeSpec(a_key, 0, fmt::format("late{}", state.lateSerial));
			if (!late->spec) {
				a_refusal = "no pass can be made for its shape (logged)";
				return nullptr;
			}
			late->gate = RunGate::Create(late->id, 1);
			late->latch = org::LatchBlock::Create(late->id + "::ConstantsAt", kLatchStride);
			if (!late->gate || !late->latch) {
				a_refusal = "its run gate or latch could not be made";
				return nullptr;
			}
			// Queued now, on the render thread (the defines read State); the pass takes it at the build.
			std::string error;
			if (a_key.compute)
				ComputePrograms().Get(a_key.effect, [&] { return DescribeCompute(a_key.effect); });
			else if (StatesKnown(*late->spec, error))
				Programs().Get(KeyOf(*late->spec), [&] { return DescribeRaster(KeyOf(*late->spec)); });
			FrameAnchors::ClientDesc client{};
			client.id = late->id;
			client.passes.push_back({ .name = fmt::format("{}.{}", late->id, late->spec->name),
				.anchor = Anchor::ImageSpace,
				.order = static_cast<std::int32_t>(kGateEntries + state.lateSerial),
				.kind = a_key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
				.make = [late, latch = std::shared_ptr<const org::LatchBlock>(late->latch), gate = std::shared_ptr<const RunGate>(late->gate)] {
					auto pass = MakePass(late->spec, latch, gate);
					late->made = pass != nullptr;
					return pass;
				},
				.atExecution = true,
				.gated = true,
				.gpuGated = true });
			client.sharedConstants = true;
			client.gates = { late->gate };
			client.active = [late] { return !late->batch.empty(); };
			client.commit = [late](const FrameAnchors::Point&) { CommitBatch(late->batch, *late->latch, *late->gate); };
			client.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
			client.onFailed = [late] {
				late->failed = true;
				logger::error("[ORG] ImageSpace: the pass of {}, built on first sight, failed with the anchor; its draws return to the engine", late->spec->name);
			};
			if (!FrameAnchors::Register(std::move(client))) {
				a_refusal = "the ImageSpace anchor refused its client";
				return nullptr;
			}
			NoteImports(*late->spec);
			logger::info("[ORG] ImageSpace: {} into {} is a shape this session did not register; its pass is built now off the render thread ({}), and the "
						 "manifest has it from the next session",
				effect.effectName, NameForLog(a_key.output), late->id);
			return state.late.emplace(a_key, std::move(late)).first->second.get();
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
				{ "depth", a_key.depth },
				{ "depthMode", a_key.depthMode },
				{ "stencilMode", a_key.stencilMode },
				{ "stencilRef", a_key.stencilRef },
				{ "scissored", a_key.scissored },
				{ "scissor", a_key.scissor },
				{ "position", a_entry.position },
				{ "lastSession", a_entry.lastSession },
				{ "lastEffectSession", a_entry.lastEffectSession } };
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
				key.depth = a_item.at("depth").get<std::int32_t>();
				key.depthMode = a_item.at("depthMode").get<std::uint32_t>();
				key.stencilMode = a_item.at("stencilMode").get<std::uint32_t>();
				key.stencilRef = a_item.at("stencilRef").get<std::uint32_t>();
				key.scissored = a_item.at("scissored").get<bool>();
				key.scissor = a_item.at("scissor").get<decltype(key.scissor)>();
				if (key.compute != kEffects[key.effect].compute || key.depthMode >= kDepthModes || key.stencilMode >= kStencilModes ||
					key.depth >= static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL))
					return std::nullopt;
				// lastEffectSession is UINT32_MAX in a manifest written before it existed: ReadManifest takes the effect's count then.
				ManifestEntry entry{ .position = a_item.at("position").get<std::uint32_t>(),
					.lastSession = a_item.at("lastSession").get<std::uint32_t>(),
					.lastEffectSession = a_item.value("lastEffectSession", UINT32_MAX) };
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
				for (const auto& [name, count] : root.value("effectSessions", json::object()).items()) {
					const auto effect = EffectNamed(name);
					if (effect >= 0 && count.is_number_unsigned())
						state.effectSessions[static_cast<std::size_t>(effect)] = count.get<std::uint32_t>();
				}
				std::uint32_t dropped = 0;
				for (const auto& item : root.value("shapes", json::array())) {
					auto shape = FromJson(item);
					if (!shape)
						continue;
					// A shape ages only in sessions in which its effect ran. Counting every session dropped the river's water shapes
					// after sixteen sessions elsewhere; the next river session then drew them on first sight (docs, "The declared
					// frame, stage 4").
					const auto effectCount = state.effectSessions[shape->first.effect];
					if (shape->second.lastEffectSession == UINT32_MAX || shape->second.lastEffectSession > effectCount)
						shape->second.lastEffectSession = effectCount;
					if (effectCount - shape->second.lastEffectSession > kManifestSessions) {
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
				for (const auto& [key, item] : root.value("depthStencils", json::object()).items()) {
					D3D11_DEPTH_STENCIL_DESC desc{};
					try {
						if (const auto index = static_cast<std::uint32_t>(std::stoul(key)); index < kDepthModes * kStencilModes && DepthFromJson(item, desc))
							state.depthDescs.emplace(index, desc);
					} catch (const std::exception&) {
					}
				}
				for (const auto& [name, defines] : root.value("defines", json::object()).items()) {
					const auto effect = EffectNamed(name);
					if (effect >= 0 && defines.is_array())
						state.savedDefines[static_cast<std::size_t>(effect)] = defines.get<std::vector<std::pair<std::string, std::string>>>();
				}
				logger::info("[ORG] ImageSpace: the shape manifest holds {} shapes (session {}; {} dropped, their effect ran in {} sessions without them)",
					state.manifest.size(), state.session, dropped, kManifestSessions);
			} catch (const std::exception& e) {
				state.manifest.clear();
				state.session = 1;
				logger::warn("[ORG] ImageSpace: the shape manifest could not be read ({}); it is written again from this session", e.what());
			}
		}

		/**
		 * @brief The manifest written: every shape with the last session that saw it, the shapes seen this session marked with this one.
		 * At most kGateEntries shapes, those whose effect last ran with them first. Render thread; a small file.
		 */
		void WriteManifest()
		{
			auto& state = IS();
			// The effects this session drew count this session once (the counts read at Setup plus one).
			std::set<std::size_t> effectsSeen;
			for (const auto& key : state.seen)
				effectsSeen.insert(key.effect);
			const auto effectCount = [&](std::size_t a_effect) {
				const auto found = state.effectSessions.find(a_effect);
				return (found != state.effectSessions.end() ? found->second : 0u) + (effectsSeen.contains(a_effect) ? 1u : 0u);
			};
			for (const auto& key : state.seen) {
				auto& entry = state.manifest[key];
				entry.lastSession = state.session;
				entry.lastEffectSession = effectCount(key.effect);
			}
			std::vector<std::pair<ShapeKey, ManifestEntry>> shapes(state.manifest.begin(), state.manifest.end());
			// Past the cap, the shapes whose effect ran most often without them go first.
			std::ranges::stable_sort(shapes, [&](const auto& a_left, const auto& a_right) {
				return effectCount(a_left.first.effect) - a_left.second.lastEffectSession < effectCount(a_right.first.effect) - a_right.second.lastEffectSession;
			});
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
			// And the depth-stencil descriptions, for the same reason.
			json depthStencils = json::object();
			for (const auto& [key, entry] : shapes) {
				const auto index = key.depthMode * kStencilModes + key.stencilMode;
				if (const auto kept = state.depthDescs.find(index); key.depth >= 0 && kept != state.depthDescs.end())
					depthStencils[std::to_string(index)] = DepthToJson(kept->second);
			}
			json effectSessions = json::object();
			for (std::size_t effect = 0; effect < kEffects.size(); ++effect)
				if (const auto count = effectCount(effect))
					effectSessions[std::string(kEffects[effect].effectName)] = count;
			const json root{ { "version", kManifestVersion }, { "session", state.session }, { "effectSessions", std::move(effectSessions) },
				{ "shapes", std::move(items) }, { "defines", std::move(defines) },
				{ "blends", std::move(blends) }, { "depthStencils", std::move(depthStencils) } };
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
			state.manifest.emplace(a_key, ManifestEntry{ .position = state.scope << 16 | std::min(state.drawIndex, 0xFFFFu), .lastSession = state.session,
											  .lastEffectSession = state.effectSessions[a_key.effect] + 1 });
			if (state.logShapes) {
				const auto viewport = ViewportOf(a_key);
				logger::info("[ORG] ImageSpace: new shape {} into {}{}{} from {} {} {}, viewport {}x{} at ({}, {}){}{}", kEffects[a_key.effect].effectName,
					NameForLog(a_key.output), a_key.output1 >= 0 ? " and " : "", a_key.output1 >= 0 ? NameForLog(a_key.output1) : "", NameForLog(a_key.inputs[0]),
					NameForLog(a_key.inputs[1]), NameForLog(a_key.inputs[2]), viewport.width, viewport.height, viewport.x, viewport.y,
					a_key.depth >= 0 ? fmt::format(", depth{} modes {}/{} ref {}", a_key.depth, a_key.depthMode, a_key.stencilMode, a_key.stencilRef) : std::string(),
					a_key.scissored ? fmt::format(", scissor ({}, {}) to ({}, {})", a_key.scissor[0], a_key.scissor[1], a_key.scissor[2], a_key.scissor[3]) : std::string());
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
			logger::info("[ORG] ImageSpace: the engine made its targets again; the {} passes and {} built on first sight register again on the new imports",
				state.specs.size(), state.late.size());
			std::vector<ShapeKey> keys;
			for (const auto& spec : state.specs)
				keys.push_back(spec->key);
			// The shapes built on first sight join the main registration: it is built again for the new imports anyway.
			for (const auto& [key, late] : state.late) {
				keys.push_back(key);
				FrameAnchors::Unregister(late->id);
			}
			state.late.clear();
			state.refusedShapes.clear();
			RegisterClient(keys);
			RegisterPost();
		}

		/**
		 * @brief A pass left out at a build because the engine had not filled its blend or depth-stencil table yet (and the manifest did
		 * not keep the entry): the client's graph is built
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
				if (D3D11_DEPTH_STENCIL_DESC desc{}; spec->key.depth >= 0 && !DepthDescOf(spec->key.depthMode * kStencilModes + spec->key.stencilMode, desc))
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
		/** @brief Runs the anchor for a_count withheld draws (the main batch's, or a shape built on first sight's) and counts them. */
		void RunAnchor(std::size_t a_count)
		{
			auto& state = IS();
			const auto count = a_count;
			state.executed = false;
			state.flushing = true;
			const auto result = FrameAnchors::Run(Anchor::ImageSpace);
			state.flushing = false;
			++state.runsThisFrame;
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

		void Flush()
		{
			auto& state = IS();
			std::size_t post = 0;
			for (const auto& client : state.post)
				post += client.batch.size();
			if ((state.batch.empty() && post == 0) || state.flushing)
				return;
			RunAnchor(state.batch.size() + post);
			state.batch.clear();
			state.keepAlive.clear();
			for (auto& client : state.post)
				client.batch.clear();
		}

		/**
		 * @brief A withheld draw of a shape built on first sight: the main batch runs first (the engine's order), then this draw as a run
		 * of its own, through its client's gate and latch.
		 */
		void RunLate(LateClient& a_late, Pending a_pending, std::span<const std::shared_ptr<const org::LatchBlock>> a_blocks)
		{
			Flush();
			// The constant chunks the draw reads, until its run.
			const std::vector<std::shared_ptr<const org::LatchBlock>> keep(a_blocks.begin(), a_blocks.end());
			a_pending.entry = 0;
			a_late.batch.push_back(std::move(a_pending));
			RunAnchor(1);
			a_late.batch.clear();
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
			// Last frame's post segments were submitted at their end taps: their draws' constant chunks are no longer read from here.
			state.postKeep.clear();
			state.compositesCut.clear();
			if (state.postRegister)
				RegisterPost();
			QueuePrograms();
			CheckImports();
			RetryBuild();
			CheckWorldSteps();
			MaybeWriteManifest();
			LogEngineDraws();
		}

		/** @brief kEffects' index of an engine effect (ImageSpaceManager::effects index), or -1 when the port does not cover it. */
		std::int32_t CoveredIndexOf(std::uint32_t a_engineEffect)
		{
			for (std::size_t i = 0; i < kEffects.size(); ++i)
				if (static_cast<std::uint32_t>(kEffects[i].id) == a_engineEffect)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/** @brief The engine's dynamic-resolution ratios now (1 when there is no graphics state). */
		std::pair<float, float> DynamicRatios()
		{
			if (!globals::game::graphicsState)
				return { 1.0f, 1.0f };
			const auto& data = globals::game::graphicsState->GetRuntimeData();
			return { data.dynamicResolutionWidthRatio, data.dynamicResolutionHeightRatio };
		}

		/** @brief A viewport of a_width x a_height at the origin, depth 0 to 1, snapped as ReadViewport snaps the engine's. */
		std::array<std::uint32_t, 6> OriginViewport(float a_width, float a_height)
		{
			const auto snap = [](float a_value) {
				const float rounded = std::round(a_value);
				return std::abs(a_value - rounded) <= 1.0f / 64.0f ? rounded : a_value;
			};
			return ViewportBits({ .width = snap(a_width), .height = snap(a_height), .minDepth = 0.0f, .maxDepth = 1.0f });
		}

		/**
		 * @brief The shape a table row declares, for its output's size now (its viewport by its rule). False with a_error when its effect
		 * is not covered or its output cannot be resolved.
		 */
		bool RowShape(const PostRow& a_row, ShapeKey& a_out, std::string& a_error)
		{
			const auto effect = CoveredIndexOf(a_row.effect);
			if (effect < 0) {
				a_error = fmt::format("its effect {} is not covered", a_row.effect);
				return false;
			}
			a_out = ShapeKey{};
			a_out.effect = static_cast<std::uint32_t>(effect);
			a_out.output = a_row.output;
			a_out.output1 = a_row.output1;
			for (std::uint32_t slot = 0; slot < kPostSlots; ++slot) {
				a_out.inputs[slot] = a_row.inputs[slot];
				a_out.address[slot] = a_row.address[slot];
				a_out.filter[slot] = a_row.filter[slot];
			}
			a_out.blend = a_row.blend;
			a_out.compute = a_row.compute;
			if (a_row.compute)
				return true;
			OutputView output;
			std::string_view refusal;
			if (!ResolveOutput(ImportOfName(a_row.output) & kInputIndexMask, output, refusal)) {
				a_error = fmt::format("its output {}: {}", NameForLog(a_row.output), refusal);
				return false;
			}
			const auto mip = MipOfName(a_row.output);
			const float width = static_cast<float>(mip == AnchorPasses::kAllMips ? output.width : (std::max)(output.width >> mip, 1u));
			const float height = static_cast<float>(mip == AnchorPasses::kAllMips ? output.height : (std::max)(output.height >> mip, 1u));
			if (a_row.viewport == ViewportRule::Dynamic) {
				const auto [x, y] = DynamicRatios();
				a_out.viewport = OriginViewport(width * x, height * y);
			} else {
				a_out.viewport = OriginViewport(width, height);
			}
			return true;
		}

		/** @brief "effect, output, inputs, samplers, blend, viewport" fields of two shapes that differ, for the mismatch's log. */
		std::string ShapeDiff(const ShapeKey& a_expected, const ShapeKey& a_seen)
		{
			std::string out;
			const auto add = [&](std::string_view a_name, const std::string& a_want, const std::string& a_got) {
				if (a_want != a_got)
					out += fmt::format("{}{} {} (engine {})", out.empty() ? "" : ", ", a_name, a_want, a_got);
			};
			add("effect", std::string(kEffects[a_expected.effect].effectName), std::string(kEffects[a_seen.effect].effectName));
			add("output", NameForLog(a_expected.output), NameForLog(a_seen.output));
			add("output1", NameForLog(a_expected.output1), NameForLog(a_seen.output1));
			for (std::uint32_t slot = 0; slot < kMaxSlots; ++slot) {
				add(fmt::format("t{}", slot), NameForLog(a_expected.inputs[slot]), NameForLog(a_seen.inputs[slot]));
				add(fmt::format("s{}", slot), fmt::format("{}/{}", a_expected.address[slot], a_expected.filter[slot]), fmt::format("{}/{}", a_seen.address[slot], a_seen.filter[slot]));
			}
			add("blend", fmt::format("{:#x}", a_expected.blend), fmt::format("{:#x}", a_seen.blend));
			add("compute", a_expected.compute ? "yes" : "no", a_seen.compute ? "yes" : "no");
			const auto viewport = [](const ShapeKey& a_key) {
				const auto v = ViewportOf(a_key);
				return fmt::format("{}x{} at ({}, {}) depth {}..{}", v.width, v.height, v.x, v.y, v.minDepth, v.maxDepth);
			};
			add("viewport", viewport(a_expected), viewport(a_seen));
			add("depth", std::to_string(a_expected.depth), std::to_string(a_seen.depth));
			add("scissor", a_expected.scissored ? "yes" : "no", a_seen.scissored ? "yes" : "no");
			return out.empty() ? std::string("nothing") : out;
		}

		/**
		 * @brief The generator (CS_ORG_POST_TABLE_DUMP=1): a table row for a declared step's draw, logged once per distinct row as the
		 * C++ initializer tools/post-table.py collects into PostTable.inl. The viewport's rule is the one that gives the engine's viewport;
		 * a viewport neither rule gives is logged as a conflict, as is a second shape for a key already logged.
		 */
		void DumpRow(const Frame::PostChain::RowKey& a_row, const ShapeKey& a_key)
		{
			static const bool dump = Switches::On("CS_ORG_POST_TABLE_DUMP");
			if (!dump)
				return;
			// The rows logged so far, as rows: a later draw of the same step is compared through its row (the viewport by its rule).
			static std::map<Frame::PostChain::RowKey, PostRow> logged;
			if (const auto found = logged.find(a_row); found != logged.end()) {
				ShapeKey expected;
				std::string error;
				if (!RowShape(found->second, expected, error) || !(expected == a_key)) {
					static std::set<std::pair<Frame::PostChain::RowKey, ShapeKey>> conflicts;
					if (conflicts.insert({ a_row, a_key }).second)
						logger::warn("[ORG] post table conflict: {} drew another shape than its first row gives: {}", Frame::PostChain::RowName(a_row),
							error.empty() ? ShapeDiff(expected, a_key) : error);
				}
				return;
			}
			const auto& effect = kEffects[a_key.effect];
			const char* rule = nullptr;
			std::string why;
			for (const auto candidate : { ViewportRule::Full, ViewportRule::Dynamic }) {
				PostRow row{ .key = a_row, .effect = static_cast<std::uint16_t>(effect.id), .output = a_key.output, .output1 = a_key.output1, .compute = a_key.compute, .viewport = candidate };
				ShapeKey expected;
				if (!RowShape(row, expected, why))
					break;
				if (a_key.compute || expected.viewport == a_key.viewport) {
					rule = candidate == ViewportRule::Full ? "Full" : "Dynamic";
					break;
				}
			}
			{
				PostRow row{ .key = a_row, .effect = static_cast<std::uint16_t>(effect.id), .output = a_key.output, .output1 = a_key.output1, .blend = a_key.blend,
					.compute = a_key.compute, .viewport = rule && std::string_view(rule) == "Dynamic" ? ViewportRule::Dynamic : ViewportRule::Full };
				for (std::uint32_t slot = 0; slot < kPostSlots; ++slot) {
					row.inputs[slot] = a_key.inputs[slot];
					row.address[slot] = static_cast<std::uint8_t>(a_key.address[slot]);
					row.filter[slot] = static_cast<std::uint8_t>(a_key.filter[slot]);
				}
				logged.emplace(a_row, row);
			}
			if (!rule || a_key.depth >= 0 || a_key.scissored) {
				logger::warn("[ORG] post table conflict: {} ({}) has a depth target, a scissor or a viewport no rule gives: {}x{} at ({}, {}) {}", Frame::PostChain::RowName(a_row),
					effect.effectName, ViewportOf(a_key).width, ViewportOf(a_key).height, ViewportOf(a_key).x, ViewportOf(a_key).y, why);
				rule = "Full";
			}
			const auto list = [](const auto& a_values, std::uint32_t a_count) {
				std::string out;
				for (std::uint32_t i = 0; i < a_count; ++i)
					out += fmt::format("{}{}", i ? ", " : "", static_cast<std::int64_t>(a_values[i]));
				return out;
			};
			for (std::uint32_t slot = kPostSlots; slot < kMaxSlots; ++slot)
				if (a_key.inputs[slot] >= 0)
					logger::warn("[ORG] post table conflict: {} reads slot {}, past the table's {}", Frame::PostChain::RowName(a_row), slot, kPostSlots);
			logger::info("[ORG] post table row: {{ {{ Step::{}, {}, {}, {}, {} }}, Effects::{}, {}, {}, {{ {} }}, {{ {} }}, {{ {} }}, {:#x}, {}, {} }},",
				[&] {
					std::string name = Frame::PostChain::StepName(a_row.step);
					std::string out;
					bool upper = true;
					for (const char c : name) {
						if (c == ' ') {
							upper = true;
							continue;
						}
						out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
						upper = false;
					}
					return out;
				}(),
				a_row.index, a_row.parity, a_row.in, a_row.out, effect.effectName, a_key.output, a_key.output1, list(a_key.inputs, kPostSlots), list(a_key.address, kPostSlots),
				list(a_key.filter, kPostSlots), a_key.blend, a_key.compute ? "true" : "false", rule);
		}

		/**
		 * @brief The table row of a draw or dispatch of the world post chain that the description declared as a step. A step the table
		 * has no row for is a tap error ("post state mismatch") and returns null.
		 *
		 * The check (with the post description, unless CS_ORG_POST_TABLE_CHECK=0) also compares the draw with its row. Any difference is
		 * a tap error, logged with what differs, and returns null. With the check off the row is returned without the comparison, so the
		 * draw still happens from its row.
		 */
		const PostRow* CheckRow(const ShapeKey& a_key)
		{
			const auto* row = Frame::PostChain::CurrentRow();
			if (!row)
				return nullptr;
			DumpRow(*row, a_key);
			const auto* found = FindPostRow(*row);
			if (!found) {
				Frame::Driver::NoteTapError(Frame::Driver::TapError::PostStateMismatch, Frame::SegmentId::Count,
					fmt::format("{} ({} into {}) has no table row", Frame::PostChain::RowName(*row), kEffects[a_key.effect].effectName, NameForLog(a_key.output)));
				Frame::PostChain::NoteRowCheck(false);
				return nullptr;
			}
			static const bool check = !Switches::Off("CS_ORG_POST_TABLE_CHECK");
			if (!check)
				return found;
			ShapeKey expected;
			std::string error;
			if (!RowShape(*found, expected, error)) {
				Frame::Driver::NoteTapError(Frame::Driver::TapError::PostStateMismatch, Frame::SegmentId::Count,
					fmt::format("{}: its row's shape cannot be made: {}", Frame::PostChain::RowName(*row), error));
				return nullptr;
			}
			if (!(expected == a_key)) {
				Frame::Driver::NoteTapError(Frame::Driver::TapError::PostStateMismatch, Frame::SegmentId::Count,
					fmt::format("{}: {}", Frame::PostChain::RowName(*row), ShapeDiff(expected, a_key)));
				Frame::PostChain::NoteRowCheck(false);
				return nullptr;
			}
			Frame::PostChain::NoteRowCheck(true);
			return found;
		}

		/**
		 * @brief The world post chain drawn from the pass-state table (CS_ORG_SEGMENT_POST_DRAW; docs, "The declared frame, stage 4b"):
		 * three clients at the ImageSpace anchor, each held by a step of a post segment and by nothing else (segment-only), with one pass
		 * per table row of its part of the chain, gated, numbered by the row's index in the table (gate entry, latch region, order key
		 * and PassDesc::row). The composites' client is the post composites segment's; the HDR effect's draws up to the bloom and the
		 * tonemap and after are the post end-of-frame segment's, either side of its Tonemap step.
		 */
		enum class PostPart : std::uint8_t
		{
			Composites,
			Hdr,
			Final,
			WorldScopes,  // the world segment's RenderWorld scopes: the reflections, the SAO, the snow pass or its copy
			WorldWater,   // the world segment's water blend, after the water's draws
			Count
		};
		constexpr std::array<const char*, static_cast<std::size_t>(PostPart::Count)> kPostIds{ "cs.engine.imagespace.post.composites", "cs.engine.imagespace.post.hdr",
			"cs.engine.imagespace.post.final", "cs.engine.imagespace.world.scopes", "cs.engine.imagespace.world.water" };

		/** @brief The client that draws a row: by its step (the slots 0 to 2 run before the tonemap; the LDR slots after it). */
		PostPart PostPartOf(const PostRow& a_row)
		{
			using Frame::PostChain::Step;
			switch (a_row.key.step) {
			case Step::SilRaw:
			case Step::SilComposite:
			case Step::Composite:
				return PostPart::Composites;
			case Step::Slot:
			case Step::Dof:
				return (a_row.key.index & 0x80) ? PostPart::Final : PostPart::Hdr;
			case Step::Copy:
			case Step::Downsample:
			case Step::Luminance:
			case Step::Adapt:
			case Step::Bloom:
				return PostPart::Hdr;
			default:
				return PostPart::Final;
			}
		}

		/**
		 * @brief A row's order key in its client: the engine's order of the steps (the LDR slots come after TAA and the screenshot copy,
		 * though they sort before the tonemap in the table), then the step's own order, then the row's index for its variants (rows of one
		 * step's variants never run in the same frame).
		 */
		std::int32_t PostOrderOf(const PostRow& a_row, std::uint32_t a_index)
		{
			using Frame::PostChain::Step;
			const auto& key = a_row.key;
			const bool late = (key.index & 0x80) != 0;
			std::uint32_t rank = 0, sub = 0;
			switch (key.step) {
			case Step::SilRaw: rank = 0; break;
			case Step::SilComposite: rank = 1; break;
			case Step::Composite: rank = 2; break;
			case Step::Slot: rank = late ? 12 : 3; sub = (key.index & 0x7f) << 5; break;
			case Step::Dof: rank = late ? 12 : 3; sub = (3u << 5) | (key.index & 0x1f); break;
			case Step::Copy: rank = 4; break;
			case Step::Downsample: rank = 5; break;
			case Step::Luminance: rank = 6; sub = key.index; break;
			case Step::Adapt: rank = 7; break;
			case Step::Bloom: rank = 8; sub = key.index; break;
			case Step::Tonemap: rank = 9; break;
			case Step::Taa: rank = 10; break;
			case Step::Screenshot: rank = 11; break;
			case Step::Upsample: rank = 13; break;
			default: rank = 14; break;
			}
			return static_cast<std::int32_t>((rank << 20) | ((sub & 0xfff) << 8) | (a_index & 0xff));
		}

		/** @brief A row's index in the table (its gate entry and PassDesc::row). */
		std::uint32_t RowIndexOf(const PostRow& a_row)
		{
			return static_cast<std::uint32_t>(&a_row - PostRows().data());
		}

		/** @brief Whether the post segments draw from the table this session: their clients are registered and none failed. */
		bool PostOn()
		{
			const auto& state = IS();
			return state.postOn && state.on;
		}

		/** @brief The ShapeKey of a draw that belongs to the post segments: the client and pass it goes to, or why it draws nothing. */
		struct PostTake
		{
			bool post = false;  // the draw is the post segments' (taken, or dropped with a tap error)
			PostClient* client = nullptr;
			const Spec* spec = nullptr;
		};

		/**
		 * @brief Where a draw of the world post chain goes when its part of the chain draws from the table this frame (OnPostPart): its
		 * row's pass in its client, or nothing (a tap error) when the draw left the description or does not match its row's pass. Outside
		 * such a part: not the post segments' (today's path).
		 */
		bool RowScheduled(PostPart a_part, std::size_t a_index);

		PostTake TakePost(const ShapeKey& a_key, const PostRow* a_row)
		{
			auto& state = IS();
			const auto part = Frame::PostChain::CurrentPart();
			const bool drawing = part != Frame::PostChain::Part::All && state.partDrawing[part == Frame::PostChain::Part::Composites ? 0 : 1];
			if (!PostOn() || !drawing)
				return {};
			PostTake take{ .post = true };
			if (!a_row) {
				// CheckRow counted it (no row, or another shape than its row), or the chain left its description (counted at the chain's end).
				if (!Frame::PostChain::CurrentRow())
					Frame::Driver::NoteTapError(Frame::Driver::TapError::PostNotDeclared, Frame::SegmentId::Count,
						fmt::format("{} into {} is not a declared step of the chain; it draws nothing", kEffects[a_key.effect].effectName, NameForLog(a_key.output)));
				return take;
			}
			const auto index = RowIndexOf(*a_row);
			auto& client = state.post[static_cast<std::size_t>(PostPartOf(*a_row))];
			const Spec* spec = index < client.byRow.size() ? client.byRow[index] : nullptr;
			if (!spec || !client.made[index] || !(spec->key == a_key)) {
				Frame::Driver::NoteTapError(Frame::Driver::TapError::PostStateMismatch, Frame::SegmentId::Count,
					fmt::format("{}: its pass {} (row {}); it draws nothing", Frame::PostChain::RowName(a_row->key),
						!spec ? "was not registered" : !client.made[index] ? "was left out of the build" : "is for another size", index));
				return take;
			}
			if (!RowScheduled(PostPartOf(*a_row), index)) {
				// A step decided at its own call (a sampler the step finds, the depth of field's composites) took a row this frame's
				// schedule has off. The segment is cut here: what it holds is submitted, and this draw and the rest of the part draw on
				// the ImageSpace anchor, after it. The slot's next schedule has the row (its last chain's fields).
				const auto partOf = PostPartOf(*a_row);
				const auto segment = partOf == PostPart::Composites ? RenderGraphRuntime::Segment::DeclaredPostComposites : RenderGraphRuntime::Segment::DeclaredPostEnd;
				// The draws the part took so far are committed into the segment's run first (the anchor's tap), then the segment ends.
				Flush();
				(void)FrameAnchors::CutSegment(static_cast<std::uint32_t>(segment),
					fmt::format("{} was decided at its call and is off in this frame's schedule", Frame::PostChain::RowName(a_row->key)));
				state.partDrawing[part == Frame::PostChain::Part::Composites ? 0 : 1] = false;
				return {};
			}
			take.client = &client;
			take.spec = spec;
			return take;
		}

		/** @brief A taken draw or dispatch of the post segments joins its client's batch; its constant chunks are held for the frame. */
		void AddToPost(PostClient& a_client, const Spec& a_spec, Pending a_pending, std::span<const std::shared_ptr<const org::LatchBlock>> a_blocks)
		{
			auto& state = IS();
			a_pending.entry = a_spec.entry;
			a_client.batch.push_back(std::move(a_pending));
			for (const auto& block : a_blocks)
				state.postKeep.push_back(block);
		}

		void RegisterPost();

		/**
		 * @brief Whether this frame's schedule has a row's pass on: its bit in the rows of the segment that holds its client, as the frame
		 * took them at Begin. A row the schedule has off records only its barriers, so a frame that draws it cuts the segment instead.
		 */
		bool RowScheduled(PostPart a_part, std::size_t a_index)
		{
			using Frame::SegmentId;
			const auto segment = a_part == PostPart::Composites ? SegmentId::PostComposites : SegmentId::PostEnd;
			const auto& rows = Frame::Driver::Current().Segment(segment).rows;
			return a_index < 256 && (rows[a_index / 64] >> (a_index % 64) & 1u) != 0;
		}

		/**
		 * @brief PostChain's decision events: a part of the world chain (the composites at RenderWorldPostProcess' entry, the end-of-frame
		 * effects at RenderEndOfFrameEffects' entry) draws from the table this frame when every draw the description declares for it is a
		 * row with a built pass for the sizes now, in a segment whose graph holds it. Otherwise its segment is cut here, before any of the
		 * part's draws, and the part draws on today's path (FrameAnchors::CutSegment, counted by reason): an effect the table does not
		 * describe yet (a group), a row the table lacks, a pass not built yet or made for another size (registered again, built off the
		 * render thread).
		 */
		void OnPostPart(Frame::PostChain::Part a_part, const std::vector<Frame::PostChain::Draw>& a_draws)
		{
			auto& state = IS();
			const std::size_t which = a_part == Frame::PostChain::Part::Composites ? 0 : 1;
			state.partDrawing[which] = false;
			if (!PostOn() || !Usable())
				return;
			const auto segment = static_cast<std::uint32_t>(which == 0 ? RenderGraphRuntime::Segment::DeclaredPostComposites : RenderGraphRuntime::Segment::DeclaredPostEnd);
			std::string why;
			for (const auto& draw : a_draws) {
				if (draw.effect >= Frame::PostChain::kGroupBase) {
					// The engine's volumetric lighting group is Volumetric Lighting's port (the VL anchor): no ImageSpace draw of its own.
					if (draw.effect == Frame::PostChain::kGroupVolumetricLighting)
						continue;
					why = fmt::format("{} is not on the pass-state table", Frame::PostChain::EffectName(draw.effect));
					break;
				}
				const auto* row = FindPostRow(draw.row);
				if (!row) {
					why = fmt::format("the table has no row for {}", Frame::PostChain::RowName(draw.row));
					break;
				}
				const auto index = RowIndexOf(*row);
				auto& client = state.post[static_cast<std::size_t>(PostPartOf(*row))];
				const Spec* spec = client.byRow[index];
				if (!spec || !client.made[index]) {
					why = fmt::format("{} has no built pass", Frame::PostChain::RowName(draw.row));
					break;
				}
				ShapeKey now;
				std::string error;
				if (!RowShape(*row, now, error) || !(now == spec->key)) {
					why = fmt::format("{}'s pass was made for another size; it registers again", Frame::PostChain::RowName(draw.row));
					state.postRegister = true;
					break;
				}
				if (!FrameAnchors::CanTap(client.id)) {
					why = fmt::format("the segment's graph does not hold {} yet", client.id);
					break;
				}
				if (!RowScheduled(PostPartOf(*row), index)) {
					why = fmt::format("{} is off in this frame's schedule; the schedule of a later frame has it", Frame::PostChain::RowName(draw.row));
					break;
				}
			}
			if (!why.empty()) {
				if (which == 0)
					state.compositesCut = std::move(why);
				else
					(void)FrameAnchors::CutSegment(segment, why);
				return;
			}
			state.partDrawing[which] = true;
		}

		/**
		 * @brief Registers the three post clients from the table, for the targets and sizes now (Setup, a resize, a row made for another
		 * dynamic-resolution size). A row whose effect is not covered or whose targets cannot be imported has no pass: a frame that
		 * selects it cuts its segment (OnPostPart).
		 */
		void RegisterPost()
		{
			auto& state = IS();
			state.postRegister = false;
			if (!state.postOn)
				return;
			const auto rows = PostRows();
			// The chain's three clients; the world scopes' are CS's own passes (RegisterWorldSteps).
			for (std::size_t p = 0; p < static_cast<std::size_t>(PostPart::WorldScopes); ++p) {
				auto& client = state.post[p];
				client.specs.clear();
				client.byRow.fill(nullptr);
				client.made.fill(false);
				client.batch.clear();
				std::vector<FrameAnchors::PassDesc> passes;
				for (const auto& row : rows) {
					if (row.key.step == Frame::PostChain::Step::None || static_cast<std::size_t>(PostPartOf(row)) != p)
						continue;
					const auto index = RowIndexOf(row);
					ShapeKey key;
					std::string error;
					if (index >= kGateEntries || !RowShape(row, key, error) || !state.enabled[key.effect])
						continue;
					auto spec = MakeSpec(key, index, fmt::format("row{}", index));
					if (!spec)
						continue;
					NoteImports(*spec);
					client.byRow[index] = spec.get();
					passes.push_back({ .name = fmt::format("{}.{}", client.id, spec->name),
						.anchor = Anchor::ImageSpace,
						.order = PostOrderOf(row, index),
						.kind = key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
						.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(client.latch), gate = std::shared_ptr<const RunGate>(client.gate), id = client.id, index] {
							auto pass = MakePass(spec, latch, gate, id);
							IS().post[static_cast<std::size_t>(PostPartOf(PostRows()[index]))].made[index] = pass != nullptr;
							return pass;
						},
						.atExecution = true,
						.gated = true,
						.gpuGated = true,
						.row = static_cast<std::int16_t>(index) });
					client.specs.push_back(std::move(spec));
				}
				FrameAnchors::ClientDesc desc{};
				desc.id = client.id;
				desc.passes = std::move(passes);
				desc.sharedConstants = true;
				desc.gates = { client.gate };
				desc.active = [p] { return !IS().post[p].batch.empty(); };
				desc.commit = [p](const FrameAnchors::Point&) {
					auto& current = IS().post[p];
					CommitBatch(current.batch, *current.latch, *current.gate);
				};
				desc.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
				desc.declared = [] { return Frame::PostChain::On(); };
				// The rows this frame's description selects for this client: from the chain's fields once its decision events ran, else
				// the slot's last chain's (the decisions made after Begin repeat from frame to frame). The world scopes' from the world
				// fields the same way. The water blend's rows are all on: water comes into view and leaves it often, and its passes are
				// gated on the GPU, so a frame without it records nothing again.
				desc.rows = [p](std::array<std::uint64_t, 4>& a_rows) {
					const auto& current = IS().post[p];
					std::vector<Frame::PostChain::Draw> draws;
					// The composites' rows are all on too: the composite's first sampler is the one it finds (wrap or clamp, by what drew
					// before it), which flips from frame to frame with no pattern a slot's last chain predicts. Gated on the GPU, the rows a
					// frame does not draw do nothing, so the schedule does not switch with it.
					if (p == static_cast<std::size_t>(PostPart::Composites)) {
						for (std::size_t index = 0; index < current.byRow.size() && index < 256; ++index)
							if (current.byRow[index])
								a_rows[index / 64] |= 1ull << (index % 64);
						return;
					}
					// The rows of every frame slot's last chain (and scopes), not only this slot's: a variant the engine picks without a
					// period of four frames (the depth of field's output, a step decided at its call) is then already on when it comes back,
					// and the schedule does not switch with it. Rows a frame does not draw do nothing (gated on the GPU).
					const auto add = [&](std::vector<Frame::PostChain::Draw> a_more) { draws.insert(draws.end(), a_more.begin(), a_more.end()); };
					add(Frame::PostChain::PredictDraws(Frame::PostChain::DeclaredFields()));
					for (std::uint32_t slot = 0; slot < RenderGraphRuntime::kGameFramesInFlight; ++slot) {
						bool have = false;
						if (const auto fields = Frame::PostChain::SlotFields(slot, &have); have)
							add(Frame::PostChain::PredictDraws(fields));
					}
					for (const auto& draw : draws)
						if (draw.row.step != Frame::PostChain::Step::None)
							if (const auto* row = FindPostRow(draw.row); row && static_cast<std::size_t>(PostPartOf(*row)) == p) {
								const auto index = RowIndexOf(*row);
								if (index < 256 && current.byRow[index])
									a_rows[index / 64] |= 1ull << (index % 64);
							}
				};
				desc.repeats = true;
				desc.segmentOnly = true;
				desc.onFailed = [] {
					auto& current = IS();
					if (current.postOn) {
						current.postOn = false;
						logger::error("[ORG] ImageSpace: a post segment's table passes failed; the post chain draws on the ImageSpace anchor for the session");
					}
				};
				if (!FrameAnchors::Register(std::move(desc))) {
					state.postOn = false;
					logger::error("[ORG] ImageSpace: the post client {} was refused; the post chain draws on the ImageSpace anchor", client.id);
					return;
				}
			}
			logger::info("[ORG] ImageSpace: the post stages draw from the pass-state table: {} composites, {} HDR and {} final passes of {} rows", state.post[0].specs.size(),
				state.post[1].specs.size(), state.post[2].specs.size(), rows.size());
		}

#	include "RenderGraph/Engine/ImageSpace/WorldSteps.inl"

		/** @brief Where a withheld draw goes: its pass, and the client built on first sight it belongs to (null: the main batch). */
		struct Admission
		{
			const Spec* spec = nullptr;
			LateClient* late = nullptr;
		};

		/**
		 * @brief The pass for a read shape, and room for it in a batch: the spec when the draw or dispatch may be withheld (any pending
		 * batch it cannot join has run), or no spec when the engine keeps it (its reason counted). A shape the session did not register
		 * is built on first sight (LateFor); its draws are taken once the anchor's built graph holds its pass, each a run of its own.
		 */
		Admission AdmitShape(const ShapeKey& a_key, std::size_t a_effect)
		{
			auto& state = IS();
			NoteShape(a_key);
			const Spec* spec = SpecFor(a_key);
			if (!spec) {
				std::string_view refusal;
				auto* late = LateFor(a_key, refusal);
				if (!late) {
					Flush();
					Refuse(refusal, a_effect);
					return {};
				}
				if (late->failed) {
					Flush();
					Refuse("its pass, built on first sight, failed with the anchor", a_effect);
					return {};
				}
				if (!FrameAnchors::CanRun(late->id, Anchor::ImageSpace)) {
					Flush();
					Refuse("a shape first seen this session, whose pass is being built off the render thread", a_effect);
					return {};
				}
				if (!late->made) {
					Flush();
					Refuse("its pass was left out of the graph's build (logged with the reason)", a_effect);
					return {};
				}
				if (!late->ready) {
					late->ready = true;
					logger::info("[ORG] ImageSpace: {} is drawn on the graph from now (built on first sight)", late->spec->name);
				}
				return { late->spec.get(), late };
			}
			if (!state.passMade[spec->entry]) {
				Flush();
				Refuse("its pass was left out of the graph's build (logged with the reason)", a_effect);
				return {};
			}
			// A batch runs in its passes' order: a pass at or before the last one taken (the engine's order differs from the manifest's,
			// or the same pass again) starts a new batch.
			if (!state.batch.empty() && spec->entry <= state.lastEntry)
				Flush();
			// A new batch is a new run: withheld only when the run can draw it now (owner, built graph). An anchor makes as many runs a
			// frame as its batches need (run leases), so no scope runs out of them.
			if (state.batch.empty()) {
				if (!FrameAnchors::CanRun(kClientId, Anchor::ImageSpace)) {
					// Before the first build after Setup or after a resize's registration (one build point).
					Refuse("the ImageSpace anchor's graph does not hold this registration yet", a_effect);
					return {};
				}
			}
			return { spec, nullptr };
		}

		/** @brief A withheld draw or dispatch joins the batch (a new batch counts against its scope). */
		void AddToBatch(const Spec& a_spec, Pending a_pending, std::span<const std::shared_ptr<const org::LatchBlock>> a_blocks)
		{
			auto& state = IS();
			a_pending.entry = a_spec.entry;
			state.batch.push_back(a_pending);
			for (const auto& block : a_blocks)
				state.keepAlive.push_back(block);
			state.lastEntry = a_spec.entry;
		}

		/**
		 * @brief A geometry effect's draw (the ripple makers): its geometry (DrawTriShape's shape and index range) leased and written into
		 * a_pending's PerFrame words, which such an effect does not read (ISWaterDisplacement.hlsl, PullRippleVertex): frameSrv and
		 * frameOffset the vertex buffer's view ref, frameSize and source the index buffer's, padding the stride and the first index. The
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
			// The buffers' view refs, which ISWaterDisplacement's pulled vertex stage reads through.
			a_pending.at.frameSrv = geometry.vertexRef.viewSlot;
			a_pending.at.frameOffset = geometry.vertexRef.byteOffset;
			a_pending.at.frameSize = geometry.indexRef.viewSlot;
			a_pending.at.source = geometry.indexRef.byteOffset;
			a_pending.at.padding = geometry.stride | (geometry.firstIndex << 16);
			a_pending.vertices = geometry.indexCount;
			a_pending.source = std::move(owners);
			return true;
		}

		/**
		 * @brief An effect's latched slots (EffectInfo::latched: ISAlphaBlend's tint masks): each one's view in the shadow state, by its
		 * heap index, into a_pending's SourcesAt, held with the run. They are loaded textures the engine made from files and never
		 * writes: the immutable class the graph exempts from declarations, so a bindless index is their read. An empty slot reads the
		 * null texture, as D3D11 reads a null view. False (refused) when a slot holds an engine target, which only a declared read may
		 * read; a failure when a view has no heap index.
		 */
		bool LatchSources(Pending& a_pending, std::size_t a_effect)
		{
			const auto& effect = kEffects[a_effect];
			const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto owners = std::make_shared<std::vector<std::shared_ptr<const void>>>();
			SourcesAt sources{};
			constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::FrameSlot, 0);
			for (std::uint32_t slot = 0; slot < effect.slots && slot < kMaxSlots; ++slot) {
				if (!(effect.reads & effect.latched & (1u << slot)))
					continue;
				if (InputOfView(shadow.PSTexture[slot]) >= 0) {
					Flush();
					Refuse("a slot it reads by heap index holds an engine target", a_effect);
					return false;
				}
				auto binding = shadow.PSTexture[slot] ? DCLF::GpuTextures::Get().ResolveBinding(shadow.PSTexture[slot], kSourceTag) :
				                                        DCLF::GpuTextures::Get().NullOf(DCLF::GpuTextures::ViewKind::Texture2D);
				if (binding.index == DCLF::GpuTextures::kInvalid) {
					Flush();
					Failure(fmt::format("its texture {} has no heap index", slot), a_effect);
					return false;
				}
				sources[slot] = binding.index;
				if (binding.owner)
					owners->push_back(std::move(binding.owner));
			}
			a_pending.sources = sources;
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
			static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
			if (effect.rastersNothing) {
				// Its state goes to D3D11 as the draw would apply it (a clear at the bind among it), after the pending batch.
				Flush();
				setDirtyStates(false);
				++state.taken;
				return true;
			}
			// A RenderWorld scope's step: a CS pass; the hook only stages its constants.
			if (const auto world = WorldStepScope(); world != WorldScope::None)
				return TakeWorldStep(world, scope.effect, false, {});
			ShapeKey key{};
			OutputView output;
			std::string_view refusal;
			bool clears = false;
			if (!ReadShape(scope.effect, key, output, refusal, clears)) {
				Flush();
				Refuse(refusal, scope.effect);
				return false;
			}
			// The clear SetDirtyStates makes below goes after the batch's draws, and this draw starts a new batch after it.
			if (clears)
				Flush();
			// The engine's state for the draw goes to D3D11 as before (the draw would apply it; when the engine draws, its own call
			// finds nothing left to apply). The shadow state then holds the viewport the draw uses.
			setDirtyStates(false);
			std::string detail;
			if (!ReadViewport(output, key, refusal, detail)) {
				Flush();
				Refuse(refusal, scope.effect, detail);
				return false;
			}
			// A declared step of a post-chain part that draws from the table goes to its row's pass, or draws nothing (a tap error).
			const auto take = TakePost(key, CheckRow(key));
			if (take.post && !take.spec)
				return true;
			Admission admitted{};
			if (!take.post) {
				admitted = AdmitShape(key, scope.effect);
				if (!admitted.spec)
					return false;
			}
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
			if (effect.latched && !LatchSources(pending, scope.effect))
				return false;
			if (take.spec)
				AddToPost(*take.client, *take.spec, std::move(pending), blocks);
			else if (admitted.late)
				RunLate(*admitted.late, std::move(pending), blocks);
			else
				AddToBatch(*admitted.spec, std::move(pending), blocks);
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
			if (const auto world = WorldStepScope(); world != WorldScope::None) {
				if (a_x == 0 || a_y == 0 || a_z == 0)
					return true;  // nothing to dispatch: the engine's call would do nothing either
				return TakeWorldStep(world, scope.effect, true, { a_x, a_y, a_z });
			}
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
			// A declared step of a post-chain part that draws from the table goes to its row's pass, or draws nothing (a tap error).
			const auto take = TakePost(key, CheckRow(key));
			if (take.post && !take.spec)
				return true;
			Admission admitted{};
			if (!take.post) {
				admitted = AdmitShape(key, scope.effect);
				if (!admitted.spec)
					return false;
			}
			// A flow copy's source, a loaded texture (an ORG engine texture, or DXVK's imported), read by its heap index from the latch.
			// It is a flow texture BGSAutoWater loaded from a file and never writes: the immutable class the graph exempts from
			// declarations (org-native-frame-graph Phase 2), so a bindless index is its read.
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
			Pending pending{ .at = where, .compute = true, .groups = { a_x, a_y, a_z }, .source = std::move(sourceBinding.owner) };
			if (take.spec)
				AddToPost(*take.client, *take.spec, std::move(pending), blocks);
			else if (admitted.late)
				RunLate(*admitted.late, std::move(pending), blocks);
			else
				AddToBatch(*admitted.spec, std::move(pending), blocks);
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
			// The manager's rectangle (+0x10 left, +0x14 right, +0x18 top, +0x1C bottom; menu 3D sets it) is not a refusal: Render
			// turns the raster state's scissor on and sets the rectangle as the scissor through 75564 (SE 0x14130ce28 to 0x14130ce71,
			// read in the binary), and the draw's shape takes it (ReadShape, the Scissor hook).
			// +0x58 (RUNTIME_DATA::unk58, SE and AE; VR's layout differs): RenderEffect draws the dynamic-resolution quad (+0x50)
			// instead of the full one (+0x48) when it is set (SE 0x141297b40, AE 0x141481ff0).
			// A geometry effect (the ripple makers) draws its own shape, whatever quad the manager would use.
			if (a_geometryEffect)
				return {};
			// Render draws its geometry argument (AE 0x1414f88b0): the manager's quad (+0x48, +0x50 for dynamic resolution) for an
			// effect, or the ripple makers' own quads in the water's displacement update. The graph's fullscreen triangle is the first.
			// The dynamic-resolution quad is refused by itself, not by the manager's flag: an effect drawn with the full quad while the
			// flag is set (the upsample) is taken.
			const auto* base = reinterpret_cast<const std::byte*>(manager);
			const auto* full = *reinterpret_cast<void* const*>(base + 0x48);
			const auto* dynamic = *reinterpret_cast<void* const*>(base + 0x50);
			if (a_geometry == full)
				return {};
			if (a_geometry == dynamic)
				return "dynamic resolution's quad";
			return "a geometry other than the full-screen quad";
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
		 * @brief A scope begins (ScopeKind): its draws' passes are ordered by their place in it, and its batch runs at its end at the
		 * latest. Nested scopes are part of the outer one.
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
			const auto id = kEffects[static_cast<std::size_t>(EffectOf(a_shader))].id;
			const bool flowCopy = id == Effects::ISCopySubRegionCS && FlowCopyBound();
			EnterScope(flowCopy ? ScopeKind::FlowMap : ScopeKind::Effect);
			if (state.scopeDepth == 0)
				return false;
			// The RenderWorld scopes drawn as CS passes: the snow pass (or its copy, between RenderSSAO's two calls) and the water
			// blend, each its own call's scope.
			const WorldScope world = id == Effects::ISWaterBlend                                                                 ? WorldScope::WaterBlend :
			                         id == Effects::ISSnowSSS || (id == Effects::ISCopySubRegionCS && !flowCopy && state.snowWindow) ? WorldScope::Snow :
			                                                                                                                  WorldScope::None;
			if (world == WorldScope::Snow)
				state.snowWindow = false;
			{
				const WorldScopeGuard guard(world);
				a_call();
				LeaveScope();
			}
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
				Frame::PostChain::NoteShader(a_effect);
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
				Frame::PostChain::NoteShader(a_shader);
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
				Frame::PostChain::NoteShader(a_shader);
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
				// The world chain's composites drawing on today's path: the post composites segment ends here, with the volumetric lighting.
				if (auto& state = IS(); !state.compositesCut.empty()) {
					(void)FrameAnchors::CutSegment(static_cast<std::uint32_t>(RenderGraphRuntime::Segment::DeclaredPostComposites), state.compositesCut);
					state.compositesCut.clear();
				}
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
				// Main::RenderWorld (100424 / 107142) sets this flag to 0 for its first call (the camera depth, its levels, the raw AO and
				// its blur) and to 1 for the second (the composite); the snow pass or its copy runs between them.
				static REL::Relocation<const bool*> composite{ REL::RelocationID(528126, 415071) };
				const bool second = *composite.get();
				EnterScope(ScopeKind::SSAO);
				IS().snowWindow = false;
				std::uint64_t result = 0;
				{
					const WorldScopeGuard guard(second ? WorldScope::SsaoComposite : WorldScope::Ssao);
					result = func(a_helper, a_source, a_destination);
					LeaveScope();
				}
				IS().snowWindow = !second;
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
				const WorldScopeGuard guard(WorldScope::Reflections);
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

		/**
		 * @brief The renderer's scissor setter (75564 / 77365: (renderer, x, y, width, height), which calls RSSetScissorRects with
		 * (x, y, x + width, y + height); read in the SE binary at 0x140d70100). The rectangle it sets is kept for the draws whose raster
		 * state scissors (ReadShape): the D3D11 context's state is not read, since it is stale under the flush latch.
		 * LegacyGraphicsCompatibility patches two calls to it (bounds to extents); this sees what they pass on.
		 */
		struct Scissor
		{
			static void thunk(void* a_renderer, std::int32_t a_x, std::int32_t a_y, std::int32_t a_width, std::int32_t a_height)
			{
				auto& state = IS();
				state.scissor = { a_x, a_y, a_x + a_width, a_y + a_height };
				state.scissorKnown = true;
				func(a_renderer, a_x, a_y, a_width, a_height);
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
					// A geometry effect may draw several shapes in one call (the underwater mask): each is taken, a run of its own when
					// its pass is already in the batch (AdmitShape).
					if ((quad || geometry) && state.scopeDepth > 0 && state.current && state.current->eligible && !state.current->compute &&
						(!state.current->drew || geometry)) {
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
		stl::detour_thunk<Scissor>(REL::RelocationID(75564, 77365));
		auto& state = IS();
		state.installed = ShaderRender::func.address() && HDRRender::func.address() && PreTonemapComposites::func.address() && ExplicitRender::func.address() &&
		                  DispatchComputeShader::func.address() && RenderSSAO::func.address() && RenderReflections::func.address() && WaterDisplacement::func.address() &&
		                  DrawTriShape::func.address() && DispatchCSShader::func.address() && Scissor::func.address();
		if (state.installed)
			logger::info(
				"[ORG] ImageSpace: hooks installed (BSImagespaceShader::Render and its explicit-target form, DispatchComputeShader, the HDR effect, the pre-tonemap "
				"composites, RenderSSAO, the reflections pass, the water's displacement update, DrawTriShape, DispatchCSShader, the scissor setter)");
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
		state.latch = org::LatchBlock::Create("ImageSpace::ConstantsAt", kGateEntries * kLatchStride);
		state.heldSources.Clear();
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
		if (const auto copy = EffectNamed("ISCopySubRegionCS"); copy >= 0 && state.enabled[static_cast<std::size_t>(copy)]) {
			MakeFlowMap();
			PublishFlowMap();
		}
		// The session's shapes, registered once: the manifest's, the flow copy's and the shapes they imply. Their programs are queued
		// now when the engine's effects exist (else at the first ImageSpace call); each pass takes its own at the graph's first build.
		ReadManifest();
		RegisterClient(ShapesToRegister());
		QueuePrograms();
		// The post segments draw from the pass-state table when they exist (CS_ORG_SEGMENT_POST_DRAW) and the post description runs.
		const auto epochs = FrameAnchors::SegmentEpochs();
		if (Frame::PostChain::On() && std::ranges::find(epochs, RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::DeclaredPostEnd)) != epochs.end()) {
			for (std::size_t p = 0; p < state.post.size(); ++p) {
				auto& client = state.post[p];
				client.id = kPostIds[p];
				client.gate = RunGate::Create(client.id, kGateEntries);
				client.latch = org::LatchBlock::Create(fmt::format("{}::ConstantsAt", client.id), kGateEntries * kLatchStride);
			}
			state.postOn = std::ranges::all_of(state.post, [](const PostClient& a_client) { return a_client.gate && a_client.latch; });
			RegisterPost();
			if (state.postOn)
				Frame::PostChain::SetPartHook(OnPostPart);
			// The RenderWorld scopes are CS passes of the deferred+water stage, registered at the first world frame (CheckWorldSteps).
			World().on = state.postOn && std::ranges::find(epochs, RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::DeclaredDeferred)) != epochs.end();
		}
	}

	std::string Status()
	{
		const auto& state = IS();
		return fmt::format("ImageSpace: {}, {} passes, {} built on first sight, {} registrations, {} shapes in the manifest ({} seen this session), {} draws taken in "
						   "{} batches, {} left to the engine, {} missed",
			state.on ? "on" : "off", state.specs.size(), state.late.size(), state.registrations, state.manifest.size(), state.seen.size(), state.taken, state.batches,
			state.engineDrawn, state.lost);
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
