#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/TonemapAnchor.h"
#include "RenderGraph/Switches.h"

bool RenderGraph::Engine::ImageSpace::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#	include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"
#	include "Features/HDRDisplay.h"
#	include "Features/HDRDisplay/ORGHDRDisplay.h"
#	include "Features/IBL/ORGIBL.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Constants/EngineConstants.h"
#	include "RenderGraph/Engine/Pipeline/Descriptions.h"
#	include "RenderGraph/Engine/Pipeline/StateConversion.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/FrameResources.h"
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
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
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

#	include <DirectXPackedVector.h>
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

		// Gate entries (one per pass, numbered by the shapes' place in the frame).
		constexpr std::uint32_t kGateEntries = 256;
		// Texture slots a shape names at most (t0..t15, the engine's PS slots): ISAlphaBlend reads all sixteen, ISLightingComposite eight.
		constexpr std::uint32_t kMaxSlots = 16;

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
		/**
		 * @brief HDR Display's float scene texture (HDRDisplay::PostChainTarget): the post chain's final target while HDR is on, as
		 * RedirectFramebuffer makes it for the engine's own chain.
		 */
		constexpr std::int32_t kHdrSceneOutput = 0x2000000;
		constexpr std::int32_t kKindBits = kDepthInput | kCubeInput | kFlowMapOutput | kLatchSourceInput | kDefaultTextureInput | kHdrSceneOutput;
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
			std::string_view effectName;       // R4's enum name
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

		/**
		 * @brief The description of a packed engine blend state: read from the engine's table when it has the entry (and kept), else
		 * the one kept from an earlier read this session. False when neither has it.
		 */
		bool BlendDescOf(std::uint32_t a_blend, D3D11_BLEND_DESC& a_out);

		/** @brief As BlendDescOf, for the engine's depth-stencil state at a table index (depth mode * kStencilModes + stencil mode). */
		bool DepthDescOf(std::uint32_t a_index, D3D11_DEPTH_STENCIL_DESC& a_out);

		/** @brief Every slot unread (-1). */
		constexpr std::array<std::int32_t, kMaxSlots> NoInputs()
		{
			std::array<std::int32_t, kMaxSlots> inputs{};
			inputs.fill(-1);
			return inputs;
		}

		/** @brief The engine's depth-stencil table's shape (RE::BSGraphics, p4.6-shaders-states.md "State tables"). */
		constexpr std::uint32_t kDepthModes = 6;
		/** @brief The engine's blend state object of a packed blend (its table entry now), or null. */
		ID3D11BlendState* BlendStateOf(std::uint32_t a_blend)
		{
			if (a_blend == kNoBlend)
				return nullptr;
			return Pipeline::EngineBlendState(a_blend & 0xFF, (a_blend >> 8) & 0xFF, (a_blend >> 16) & 0xFF, (a_blend >> 24) & 0xFF);
		}

		/** @brief Whether a depth-stencil state writes depth or stencil (the pass then declares its target read-write). */
		bool DepthWrites(const D3D11_DEPTH_STENCIL_DESC& a_desc)
		{
			const auto writes = [](const D3D11_DEPTH_STENCILOP_DESC& a_face) {
				return a_face.StencilFailOp != D3D11_STENCIL_OP_KEEP || a_face.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP || a_face.StencilPassOp != D3D11_STENCIL_OP_KEEP;
			};
			return (a_desc.DepthEnable && a_desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ||
			       (a_desc.StencilEnable && a_desc.StencilWriteMask != 0 && (writes(a_desc.FrontFace) || writes(a_desc.BackFace)));
		}

		constexpr std::uint32_t kStencilModes = 40;

		/**
		 * @brief What one pass is made for, by value: the effect, its outputs and inputs, its samplers,
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
		 * @brief One of the CS stages' ImageSpace clients (the post chain's three, the RenderWorld scopes' two): its steps' passes, with
		 * its own gate and latch (a stage closes its clients' gates when it begins, and the post stages' runs are different runs).
		 */
		struct PostClient
		{
			const char* id = nullptr;
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> latch;
			std::vector<std::shared_ptr<const Spec>> specs;
			std::vector<Pending> batch;  // the draws its next tap commits
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

		struct ImageSpaceState
		{
			bool installed = false;
			bool reserved = false;
			bool on = false;  // set up; off again for the session after a failure
			bool flushing = false;
			// The draw DrawTriShape is about to make, for a geometry effect's TakeDraw: the shape's renderer data and its index range.
			const void* drawShape = nullptr;
			std::uint32_t drawStart = 0, drawTriangles = 0;
			std::array<bool, kEffects.size()> enabled{};
			std::unordered_map<const void*, std::int32_t> effectOf;  // engine shader -> kEffects index (filled from the manager's effects)
			// By run: the sources a run's latched draws read by heap index (Pending::source), held until the run's lease index runs
			// again (its last run retired before the index was opened again). Several commits into one run (regions, a stage's taps)
			// add to its list. A stage's commit moves its staging run's list onto the run that submits it (RunSlotsRegistry::MoveAll),
			// so a staging run, which names the same run every frame, holds only the frame's own sources.
			RunSlots<std::vector<std::shared_ptr<const void>>> heldSources;
			std::map<std::pair<std::uint32_t, std::uint32_t>, org::OwnedDescriptorBinding> samplers;  // (address, filter)
			org::OwnedDescriptorBinding defaultSampler;  // D3D11's default sampler: the SAO composite's s9 (FeatureBinds)
			// A 1x1x1 volume of (0, 0, 0, 1) by heap index: what the SAO composite reads for Exponential Height Fog's volumes before the fog
			// publishes them (D3D11 binds none then). Made at the first build that needs it.
			winrt::com_ptr<ID3D11ShaderResourceView> noFogView;
			std::uint32_t noFogIndex = UINT32_MAX;
			std::shared_ptr<const void> noFogOwner;
			bool noFogTried = false;
			// Each engine blend and depth-stencil state's description as last read from its table, for a build when the entry is empty.
			std::map<std::uint32_t, D3D11_BLEND_DESC> blendDescs;
			std::map<std::uint32_t, D3D11_DEPTH_STENCIL_DESC> depthDescs;
			// The engine objects the steps' imports were made from (a resize check).
			std::map<std::int32_t, const void*> importedObjects;
			// The flow map texture published under kFlowMapId, null before Setup publishes it.
			const ID3D11Texture2D* publishedFlowMap = nullptr;
			// The frame and its batches.
			std::uint32_t scope = 0;                                   // the current scope (ScopeKind)
			std::uint32_t drawIndex = 0;                               // eligible draws and dispatches so far in the current scope
			std::uint32_t frame = UINT32_MAX;
			std::uint32_t scopeDepth = 0;
			std::optional<EffectScope> current;
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
			// The CS stages' ImageSpace clients (the post chain's, the RenderWorld scopes'): on for the session once their gates and latches
			// exist; the constant chunks their draws read, held for the frame.
			bool postOn = false;
			std::array<PostClient, 9> post{};
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
			if (a_name & kHdrSceneOutput)
				return "hdr-scene";
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

		/** @brief A kEffects index by its name, or -1. */
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
		 * @brief The flow map the flow copies write, or null. Unified Water puts a loaded flow map without a UAV in the slot and removes
		 * the copies, so a flow map without a UAV has none.
		 */
		ID3D11Texture2D* FlowCopyTexture()
		{
			const auto* map = FlowMap();
			return map && map->UAV && map->texture ? static_cast<ID3D11Texture2D*>(map->texture) : nullptr;
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
			if (!map || !import || !PublishedResources::PublishImport(kFlowMapId, import, Tripwire::Unwrap(map->resourceView))) {
				logger::warn("[ORG] ImageSpace: the flow map is not published; the water's draws import its view");
				return;
			}
			IS().publishedFlowMap = FlowMapTexture();
		}

		/** @brief Publishes the flow map again when another texture takes its slot after Setup, as Unified Water's flow map does. */
		void CheckFlowMap()
		{
			auto& state = IS();
			const auto* texture = FlowMapTexture();
			if (!state.publishedFlowMap || !texture || texture == state.publishedFlowMap)
				return;
			logger::info("[ORG] ImageSpace: another texture took the flow map's slot; it is published in its place{}",
				FlowCopyTexture() ? "" : " (it has no UAV, so there are no flow copies)");
			state.publishedFlowMap = texture;
			PublishFlowMap();
		}

		/** @brief The graph's import of a target as ShapeKey names it (a render target, a depth target, a cubemap or the flow map; any mip is its texture's). */
		FrameAnchors::Texture ImportOf(std::int32_t a_name)
		{
			if (a_name < 0 || (a_name & kLatchSourceInput))
				return nullptr;
			if (a_name & kFlowMapOutput)
				return FlowMapImport();
			if (a_name & kHdrSceneOutput)
				return ORGHDRDisplay::Get().GraphOf(globals::features::hdrDisplay.PostChainTarget());
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
			if (a_name & kHdrSceneOutput)
				return globals::features::hdrDisplay.PostChainTarget();
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

		/** @brief The format of an output's render target view (the program's target format), from the target or the texture it is a mip of. */
		rhi::Format RtvFormatOf(std::int32_t a_name, const FrameAnchors::Texture& a_import)
		{
			if (a_name >= 0 && (a_name & kHdrSceneOutput))
				return a_import ? a_import.Format() : rhi::Format::Unknown;
			const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			if (auto* rtv = targets[IndexOfName(a_name)].RTV) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				rtv->GetDesc(&view);
				return rhi::helpers::ToRHI(view.Format);
			}
			return a_import ? a_import.Format() : rhi::Format::Unknown;
		}

		/** @brief Whether a_view is the flow map's UAV (either may be a tripwire handle). */
		bool IsFlowMapUav(const ID3D11UnorderedAccessView* a_view)
		{
			const auto* map = FlowMap();
			return a_view && map && map->UAV && Tripwire::Unwrap(a_view) == Tripwire::Unwrap(static_cast<const ID3D11UnorderedAccessView*>(map->UAV));
		}

		/** @brief Whether the shadow state's compute output is the flow map now (a flow copy is about to dispatch). */
		bool FlowCopyBound() { return IsFlowMapUav(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().CSUAV[0]); }

		/** @brief ShapeKey::viewport of a viewport. */
		std::array<std::uint32_t, 6> ViewportBits(const rhi::Viewport& a_viewport)
		{
			return { std::bit_cast<std::uint32_t>(a_viewport.x), std::bit_cast<std::uint32_t>(a_viewport.y), std::bit_cast<std::uint32_t>(a_viewport.width),
				std::bit_cast<std::uint32_t>(a_viewport.height), std::bit_cast<std::uint32_t>(a_viewport.minDepth), std::bit_cast<std::uint32_t>(a_viewport.maxDepth) };
		}

		/** @brief The engine's sampler table's shape: 6 address modes by 5 filter modes (p4.6-shaders-states.md "State tables"). */
		constexpr std::uint32_t kSamplerAddressModes = 6;
		constexpr std::uint32_t kSamplerFilterModes = 5;

		/**
		 * @brief A raster program's key: the effect, the outputs' formats, the packed blend state, the depth-stencil format (Unknown:
		 * none) and the depth-stencil table index (depth mode * kStencilModes + stencil mode).
		 */
		using ProgramKey = std::tuple<std::size_t, rhi::Format, rhi::Format, std::uint32_t, rhi::Format, std::uint32_t>;

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

		/** @brief LiveDefinesOf as the program builds take them. */
		std::vector<std::pair<std::wstring, std::wstring>> DefinesOf(std::size_t a_effect) { return AnchorPasses::Defines(LiveDefinesOf(a_effect)); }

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
			const auto defines = LiveDefinesOf(a_effect);
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
		 * @brief The ORG depth-stencil state of a ProgramKey's table index, and whether it writes; false when the engine's table
		 * has not had the entry yet.
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
		 * @brief The graph pass of a spec, made at a build of the anchor's graph (render thread): its program taken now (built, waited
		 * for, or built here and logged), then the pass AnchorPasses makes for its draw or dispatch. Null leaves it out of that build
		 * (logged): its shape's draws are refused by the commit's caller, since the gate entry stays closed and Scheduled still holds.
		 */
		std::shared_ptr<org::RenderPass> MakePass(const std::shared_ptr<const Spec>& a_spec, const std::shared_ptr<const org::LatchBlock>& a_latch,
			const std::shared_ptr<const RunGate>& a_gate, const char* a_clientId)
		{
			const auto& spec = *a_spec;
			const auto& effect = kEffects[spec.key.effect];
			if (!ShaderOf(spec.key.effect)) {
				logger::info("[ORG] ImageSpace: {}: the engine has not made its effect yet; its pass is left out of this build", spec.name);
				return nullptr;
			}
			// The engine's blend and depth-stencil tables are read here, at the graph's build, not at Setup (they are filled after it).
			// The samplers are made from their modes alone (EngineSamplerDesc).
			if (!spec.key.compute) {
				std::string error;
				if (!StatesKnown(spec, error)) {
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

		void CheckWorldSteps();
		void CheckChainSteps();
		void CheckStartSteps();
		void CheckMenuSteps();
		void ReregisterSteps();

		/**
		 * @brief Registers a stage's steps again at the next frame boundary, once while one is pending. A frame keeps the step layout
		 * its schedule was described with, so a new size or setting seen during a frame takes effect at the next Begin.
		 */
		void RegisterAtBoundary(bool& a_pending, bool (*a_ready)(), void (*a_register)())
		{
			if (std::exchange(a_pending, true))
				return;
			Frame::Driver::AtNextBegin([&a_pending, a_ready, a_register] {
				a_pending = false;
				if (a_ready())
					a_register();
			});
		}

		/** @brief Whether the engine has what the steps' registrations read, kMAIN and the ImageSpace manager. */
		bool StepTargetsReady()
		{
			return IS().on && RE::ImageSpaceManager::GetSingleton() && globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN].texture;
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

		/** @brief Whether the RenderWorld scopes are the deferred+water stage's passes (WorldSteps.inl). */
		bool WorldStepsOn();
		bool ChainStepsOn();

		/** @brief Once a frame: when the engine made a registered target again (a resize), the stages' steps register again on the new imports. */
		void CheckImports()
		{
			auto& state = IS();
			bool changed = false;
			// HDR's scene texture comes and goes with HDR. The chain checks it itself (CheckChainSteps).
			for (const auto& [name, object] : state.importedObjects)
				if (!(name & kHdrSceneOutput) && EngineObjectOf(name) != object) {
					changed = true;
					break;
				}
			if (!changed)
				return;
			logger::info("[ORG] ImageSpace: the engine made its targets again; the stages' steps register again on the new imports");
			ReregisterSteps();
		}

		bool Usable()
		{
			const auto& state = IS();
			return state.on && !state.flushing && FrameAnchors::IsRenderOwner() && RenderGraphRuntime::Get().IsActive();
		}

		/** @brief Taps the ImageSpace anchor for a_count withheld step draws and counts them. A tap that did not draw them is an error. */
		void RunAnchor(std::size_t a_count)
		{
			auto& state = IS();
			const auto count = a_count;
			state.executed = false;
			state.flushing = true;
			const auto result = FrameAnchors::Run(Anchor::ImageSpace);
			state.flushing = false;
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
			if (post == 0 || state.flushing)
				return;
			RunAnchor(post);
			for (auto& client : state.post)
				client.batch.clear();
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

		/** @brief Once a frame, on the first ImageSpace call of the render owner: the flow map, a resize, the steps' registrations. */
		void BeginFrame()
		{
			auto& state = IS();
			const auto frame = globals::state ? globals::state->frameCount : 0u;
			if (frame == state.frame)
				return;
			Flush();
			state.frame = frame;
			// Last frame's post segments were submitted at their end taps: their draws' constant chunks are no longer read from here.
			state.postKeep.clear();
			CheckFlowMap();
			CheckImports();
			CheckWorldSteps();
			CheckChainSteps();
			CheckStartSteps();
			CheckMenuSteps();
			LogEngineDraws();
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
		 * @brief The CS stages' ImageSpace clients: the post chain's three (ChainSteps.inl: the composites, the slots before the tonemap and
		 * the HDR effect up to its bloom, then the tonemap and after, either side of the post stage's Tonemap step; all three in the post
		 * stage) and the deferred+water stage's two (WorldSteps.inl).
		 */
		enum class PostPart : std::uint8_t
		{
			Composites,
			Hdr,
			Final,
			WorldScopes,  // the deferred+water stage's RenderWorld scopes: the reflections, the SAO, the snow pass or its copy
			WorldWater,   // the deferred+water stage's water blend, after the water's draws
			Start,        // the start stage's water effects: the displacement simulation and the flow copies (StartSteps.inl)
			Menu,         // a menu scene's composites (the loading screen, the menus, an item over the live world): the UI stage's (MenuSteps.inl)
			Frozen,       // a frozen world's composite, first in the UI stage (MenuSteps.inl)
			FaceTint,     // the player's face tint at a load (MenuSteps.inl): the start stage's in a world frame, the UI stage's at a loading screen
			Count
		};
		constexpr std::array<const char*, static_cast<std::size_t>(PostPart::Count)> kPostIds{ "cs.engine.imagespace.post.composites", "cs.engine.imagespace.post.hdr",
			"cs.engine.imagespace.post.final", "cs.engine.imagespace.world.scopes", "cs.engine.imagespace.world.water", "cs.engine.imagespace.start",
			"cs.engine.imagespace.menu", "cs.engine.imagespace.frozen", "cs.engine.imagespace.facetint" };
		static_assert(sizeof(IS().post) / sizeof(PostClient) == static_cast<std::size_t>(PostPart::Count), "a post client per part");

#	include "RenderGraph/Engine/ImageSpace/WorldSteps.inl"

		bool WorldStepsOn() { return World().on; }
#	include "RenderGraph/Engine/ImageSpace/ChainSteps.inl"
		bool ChainStepsOn() { return Chain().on; }
#	include "RenderGraph/Engine/ImageSpace/StartSteps.inl"
#	include "RenderGraph/Engine/ImageSpace/MenuSteps.inl"

		/** @brief The engine made its targets again: the stages' steps register again at the next frame, against the new imports. */
		void ReregisterSteps()
		{
			World().registered = false;
			Chain().registered = false;
			Start().registered = false;
			Menu().registered = false;
		}

		/**
		 * @brief An ImageSpace draw or dispatch with no CS pass in native mode with CS's stages: a mismatch (NoPass; fatal in development
		 * builds, logged once in release). It draws nothing. The message names the shader and, for the diagnosis only, what the engine bound.
		 */
		void NoPassDraw(const RE::BSImagespaceShader* a_shader, bool a_compute, std::string_view a_why)
		{
			const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			std::string inputs;
			for (std::uint32_t slot = 0; slot < kMaxSlots; ++slot) {
				auto* view = a_compute ? shadow.CSTexture[slot] : shadow.PSTexture[slot];
				if (!view)
					continue;
				const auto input = InputOfView(view);
				inputs += fmt::format("{}t{} {}", inputs.empty() ? "" : ", ", slot, input >= 0 ? NameForLog(input) : std::string("a loaded texture"));
			}
			const char* name = a_shader && a_shader->name ? a_shader->name : "an unnamed ImageSpace shader";
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, Frame::SegmentId::Count,
				fmt::format("{} {} has no CS pass ({}; {} rt{}, reads {}); it draws nothing", name, a_compute ? "dispatch" : "draw", a_why, a_compute ? "u0 of" : "into",
					a_compute ? TargetOfUav(shadow.CSUAV[0]) : static_cast<std::int32_t>(shadow.renderTargets[0]), inputs.empty() ? std::string("nothing") : inputs));
		}

		/** @brief The engine's draw of the current effect: a CS step's (true; the caller skips the engine's draw) or NoPass. */
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
			// A RenderWorld scope's, the world post chain's, the start stage's or a menu composite's step: a CS pass; the hook only stages
			// its constants.
			if (StartStepScope() != WorldScope::None)
				return TakeStartStep(scope.effect, false, {});
			if (const auto world = WorldStepScope(); world != WorldScope::None)
				return TakeWorldStep(world, scope.effect, false, {});
			if (ChainStepNow())
				return TakeChainStep(scope.effect, false, {});
			if (MenuStepNow(scope.effect))
				return TakeMenuStep(scope.effect, false, {});
			NoPassDraw(scope.shader, false, "outside every CS step");
			return true;
		}

		/** @brief The engine's dispatch of the current compute effect: a CS step's (true; the caller skips the engine's dispatch) or NoPass. */
		bool TakeDispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
		{
			auto& state = IS();
			auto& scope = *state.current;
			scope.drew = true;
			++state.drawIndex;
			if (a_x == 0 || a_y == 0 || a_z == 0)
				return true;  // nothing to dispatch: the engine's call would do nothing either
			if (StartStepScope() != WorldScope::None)
				return TakeStartStep(scope.effect, true, { a_x, a_y, a_z });
			if (const auto world = WorldStepScope(); world != WorldScope::None)
				return TakeWorldStep(world, scope.effect, true, { a_x, a_y, a_z });
			if (ChainStepNow())
				return TakeChainStep(scope.effect, true, { a_x, a_y, a_z });
			if (MenuStepNow(scope.effect))
				return TakeMenuStep(scope.effect, true, { a_x, a_y, a_z });
			NoPassDraw(scope.shader, true, "outside every CS step");
			return true;
		}

		/** @brief Why an effect's Render of a_geometry cannot have its draw taken, or empty. */
		std::string_view Ineligible(const void* a_geometry, bool a_geometryEffect)
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active (CS_ORG_ENGINE_CONSTANTS=1)";
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager)
				return "no ImageSpaceManager";
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

		/**
		 * @brief a_call inside the scope the effect needs: the enclosing scope, or one of its own for a call outside every scope
		 * (ScopeKind::FlowMap for a flow copy, whose target the shadow state holds before the dispatch; else ScopeKind::Effect), so its
		 * draw reaches a step or is a mismatch. False when no scope could be entered (a_call not run).
		 */
		template <class Call>
		bool InScope(const RE::BSImagespaceShader* a_shader, Call&& a_call)
		{
			auto& state = IS();
			if (state.scopeDepth > 0) {
				a_call();
				return true;
			}
			const auto index = EffectOf(a_shader);
			const auto id = index >= 0 ? kEffects[static_cast<std::size_t>(index)].id : Effects::ISCopy;
			const bool flowCopy = index >= 0 && id == Effects::ISCopySubRegionCS && FlowCopyBound();
			EnterScope(flowCopy ? ScopeKind::FlowMap : ScopeKind::Effect);
			if (state.scopeDepth == 0)
				return false;
			const FlowCopyGuard flow(flowCopy);
			// The RenderWorld scopes drawn as CS passes: the snow pass (or its copy, between RenderSSAO's two calls) and the water
			// blend, each its own call's scope.
			const WorldScope world = index < 0                     ? WorldScope::None :
			                         id == Effects::ISWaterBlend                                                                 ? WorldScope::WaterBlend :
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
				{
					const ChainHdrGuard chain;
					const MenuHdrGuard menu;
					func(a_effect, a_quad, a_param);
					LeaveScope();
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief ImageSpaceEffectFullScreenBlur's and ImageSpaceEffectGetHit's Render (their vtables' slot 1; the get-hit effect uses the base
		 * ImageSpaceEffect::Render): their draws inside the world chain are the slot's inner steps (ChainSlotGuard).
		 */
		template <std::int32_t Slot>
		struct SlotEffectRender
		{
			static void thunk(void* a_effect, void* a_quad, void* a_param)
			{
				const ChainSlotGuard chain(Slot);
				func(a_effect, a_quad, a_param);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceManager::RenderPreTonemapComposites (99024 / 105675): SIL, the lens flares, the VL / lens flare composite. */
		struct PreTonemapComposites
		{
			static void thunk(void* a_manager, std::uintptr_t a_source, std::uintptr_t a_destination)
			{
				EnterScope(ScopeKind::Composites);
				{
					const ChainCompositesGuard chain;
					func(a_manager, a_source, a_destination);
					LeaveScope();
				}
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
				{
					const WaterUpdateGuard water;
					func(a_system);
					LeaveScope();
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief ImageSpaceManager::RenderEffect (99025 / 105676: manager, effect index, source, destination, a parameter): its source and
		 * destination are the variant of a copy outside the world chain (MenuSteps.inl).
		 */
		struct RenderEffect
		{
			static void thunk(void* a_manager, std::uint32_t a_effect, std::uint64_t a_source, std::uint64_t a_destination, std::uint64_t a_param)
			{
				const MenuEffectCall call(a_effect, static_cast<std::int32_t>(a_source), static_cast<std::int32_t>(a_destination));
				func(a_manager, a_effect, a_source, a_destination, a_param);
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
					} else if ((quad || geometry) && state.current && !state.current->compute) {
						// An ImageSpace effect's draw no step takes (an effect CS does not cover, or one it cannot take): a mismatch.
						NoPassDraw(state.current->shader, false, state.current->covered ? state.lastRefusal.empty() ? "not taken" : state.lastRefusal.c_str() : "an effect CS does not cover");
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
					} else if (state.current && state.current->compute) {
						NoPassDraw(state.current->shader, true, state.current->covered ? "not taken" : "an effect CS does not cover");
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
		stl::detour_thunk<RenderEffect>(REL::RelocationID(99025, 105676));
		auto& state = IS();
		state.installed = ShaderRender::func.address() && HDRRender::func.address() && PreTonemapComposites::func.address() && ExplicitRender::func.address() &&
		                  DispatchComputeShader::func.address() && RenderSSAO::func.address() && RenderReflections::func.address() && WaterDisplacement::func.address() &&
		                  DrawTriShape::func.address() && DispatchCSShader::func.address() && RenderEffect::func.address();
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
		state.heldSources.Clear();
		SelectEffects();
		for (std::size_t i = 0; i < kEffects.size(); ++i) {
			state.fallbacks[i] = std::make_unique<Util::ReasonLog>(fmt::format("[ORG] ImageSpace: {} stays with the engine", kEffects[i].effectName));
			state.failures[i] = std::make_unique<Util::ReasonLog>(fmt::format("[ORG] ImageSpace: {} failed on the graph and stays with the engine", kEffects[i].effectName),
				spdlog::level::err);
		}
		// The full-screen blur's and the get-hit effect's Render, wrapped here rather than at Install: Legacy Graphics Compatibility checks
		// the full-screen blur's original vfunc when it patches it at load, and this wraps its patch.
		static bool slotHooks = [] {
			REL::Relocation<std::uintptr_t> fullBlur{ RE::VTABLE_ImageSpaceEffectFullScreenBlur[0] };
			REL::Relocation<std::uintptr_t> getHit{ RE::VTABLE_ImageSpaceEffectGetHit[0] };
			SlotEffectRender<6>::func = fullBlur.write_vfunc(1, SlotEffectRender<6>::thunk);
			SlotEffectRender<7>::func = getHit.write_vfunc(1, SlotEffectRender<7>::thunk);
			return true;
		}();
		(void)slotHooks;
		// Every ImageSpace draw is a step of a CS stage (the post chain and the RenderWorld scopes registered at the first world frame,
		// the water effects at the start stage's, the menus' composites now), so every stage with ImageSpace steps must run.
		const auto epochs = FrameAnchors::SegmentEpochs();
		const auto runs = [&](Frame::SegmentId a_stage) {
			return std::ranges::find(epochs, RenderGraphRuntime::EpochOf(RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(a_stage)))) != epochs.end();
		};
		if (!Frame::PostChain::On() || !runs(Frame::SegmentId::Post) || !runs(Frame::SegmentId::Deferred) || !runs(Frame::SegmentId::Start) || !runs(Frame::SegmentId::Ui))
			return off("not every CS stage with ImageSpace steps runs in this mode");
		for (std::size_t p = 0; p < state.post.size(); ++p) {
			auto& client = state.post[p];
			client.id = kPostIds[p];
			client.gate = RunGate::Create(client.id, kGateEntries);
			client.latch = org::LatchBlock::Create(fmt::format("{}::ConstantsAt", client.id), kGateEntries * kLatchStride);
		}
		if (!std::ranges::all_of(state.post, [](const PostClient& a_client) { return a_client.gate && a_client.latch; }))
			return off("a step client's run gate or latch could not be made");
		state.postOn = true;
		Chain().on = World().on = Start().on = Menu().on = true;
		state.on = true;
		std::string selected;
		for (std::size_t i = 0; i < kEffects.size(); ++i)
			if (state.enabled[i])
				selected += fmt::format("{}{}", selected.empty() ? "" : ",", kEffects[i].effectName);
		logger::info("[ORG] ImageSpace: on (CS_ORG_IMAGESPACE); effects {}; every ImageSpace draw is a CS step's pass", selected.empty() ? "none" : selected);
		// The water's flow map, made now if the engine has not yet (it does at the first cell with flowing water), so the flow copies'
		// pass names it from this registration.
		if (const auto copy = EffectNamed("ISCopySubRegionCS"); copy >= 0 && state.enabled[static_cast<std::size_t>(copy)]) {
			MakeFlowMap();
			PublishFlowMap();
		}
		if (globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN].texture && RE::ImageSpaceManager::GetSingleton())
			RegisterMenuSteps();
	}

	void OnEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final)
	{
		auto& chain = Chain();
		if (!chain.on || !chain.registered) {
			chain.inEndOfFrame = false;
			return;
		}
		if (a_begin) {
			// The draws before this belong to the stage's earlier steps (the Upscale anchor's): their batch runs first.
			if (Usable())
				Flush();
			CheckEndOfFrame(a_source, a_final);
			chain.copyDrawn = false;
			chain.scratchWritten = false;
			chain.taaRouted = false;
			chain.inEndOfFrame = true;
		} else {
			chain.inEndOfFrame = false;
			OpenThrough();
			if (std::exchange(chain.captureFrame, false))
				OpenCapture();
		}
	}

	void OnFrameBegin()
	{
		// The menu composites register at the first frame of the session (the startup logo's), so their build is ready for the main menu.
		if (Usable())
			CheckMenuSteps();
	}

	void OnMenuEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final)
	{
		auto& menu = Menu();
		if (!menu.on) {
			menu.inEndOfFrame = false;
			return;
		}
		menu.inEndOfFrame = a_begin;
		menu.source = a_begin ? a_source : -1;
		menu.final = a_begin ? a_final : -1;
	}

	std::string_view AdaptedLuminanceOf(const ID3D11ShaderResourceView* a_view)
	{
		return AdaptedLuminanceIdOf(a_view);
	}

	std::span<const std::string_view> AdaptedLuminanceIds()
	{
		return kAdaptedIds;
	}

	bool ChainTonemapTargets(TonemapTargets& a_out)
	{
		const auto& chain = Chain();
		if (!chain.on || !chain.registered || !chain.inEndOfFrame || !chain.inHdr)
			return false;
		a_out = {};
		a_out.input = ImportOf(ImportOfName(RE::RENDER_TARGET::kMAIN));
		a_out.output = ImportOf(ImportOfName(chain.final));
		a_out.inputFormat = RtvFormatOf(RE::RENDER_TARGET::kMAIN, a_out.input);
		a_out.outputFormat = RtvFormatOf(chain.final, a_out.output);
		a_out.chain = true;
		return a_out.input && a_out.output;
	}

	std::string Status()
	{
		const auto& state = IS();
		return fmt::format("ImageSpace: {}, {} draws taken in {} batches, {} left to the engine, {} missed", state.on ? "on" : "off", state.taken, state.batches,
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
	void OnEndOfFrame(bool, std::int32_t, std::int32_t) {}
	void OnMenuEndOfFrame(bool, std::int32_t, std::int32_t) {}
	void OnFrameBegin() {}
	std::string_view AdaptedLuminanceOf(const ID3D11ShaderResourceView*) { return {}; }
	std::span<const std::string_view> AdaptedLuminanceIds() { return {}; }
	bool ChainTonemapTargets(TonemapTargets&) { return false; }
	std::string Status() { return "ImageSpace: no render graph in this build"; }
}

#endif
