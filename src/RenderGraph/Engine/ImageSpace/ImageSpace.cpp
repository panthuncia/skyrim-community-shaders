#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/TonemapAnchor.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Switches.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DynamicCubemaps.h"
#include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"
#include "Features/HDRDisplay.h"
#include "Features/HDRDisplay/ORGHDRDisplay.h"
#include "Features/IBL/ORGIBL.h"
#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/UnifiedWater.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Pipeline/StateConversion.h"
#include "RenderGraph/Engine/Skeleton/SkeletonSites.h"
#include "ShaderTools/LegacyGraphicsCompatibility.h"
#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/FrameResources.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/ProgramSet.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Logging.h"
#include "Utils/SharedShaderStore.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace RenderGraph::Engine::ImageSpace
{
	namespace
	{
		using FrameAnchors::Anchor;
		namespace Constants = RenderGraph::Engine::Constants;
		namespace Pipeline = RenderGraph::Engine::Pipeline;
		using Effects = RE::ImageSpaceManager;

		// One gate entry per pass.
		constexpr std::uint32_t kGateEntries = 256;
		// ISAlphaBlend reads all sixteen PS slots.
		constexpr std::uint32_t kMaxSlots = 16;
		// GpuTextures' log tag for ImageSpace's imports.
		constexpr std::uint32_t kImageSpaceTag = ::DCLF::GpuTextures::Tag(::DCLF::GpuTextures::Source::FrameSlot, 0);

		/** @brief Bits on a ShapeKey input that name a depth SRV or a cubemap instead of a render target. */
		constexpr std::int32_t kDepthInput = 0x10000;
		constexpr std::int32_t kCubeInput = 0x20000;
		/** @brief Names outside the engine's target tables: the water flow map, and a source read by the latched heap index. */
		constexpr std::int32_t kFlowMapOutput = 0x40000;
		constexpr std::int32_t kLatchSourceInput = 0x80000;
		/** @brief One of the engine's default textures, by DefaultTextureOf's index. ISTemporalAA's t5 gets one when Upscaling turns vanilla TAA off. */
		constexpr std::int32_t kDefaultTextureInput = 0x1000000;  // above the mip bits (kMipShift)
		/** @brief HDR Display's float scene texture, the post chain's final target while HDR is on. */
		constexpr std::int32_t kHdrSceneOutput = 0x2000000;
		constexpr std::int32_t kKindBits = kDepthInput | kCubeInput | kFlowMapOutput | kLatchSourceInput | kDefaultTextureInput | kHdrSceneOutput;
		constexpr std::int32_t kInputIndexMask = 0xFFFF;
		/** @brief A render target that views one mip of another target's texture is named by that target plus mip + 1 in these bits. */
		constexpr std::int32_t kMipShift = 20;
		constexpr std::int32_t kMipBits = 0xF << kMipShift;
		constexpr std::uint32_t kMaxMip = 14;

		constexpr std::int32_t WithMip(std::int32_t a_target, std::uint32_t a_mip)
		{
			return a_target | static_cast<std::int32_t>((a_mip + 1) << kMipShift);
		}

		constexpr std::uint32_t IndexOfName(std::int32_t a_name) { return static_cast<std::uint32_t>(a_name & kInputIndexMask); }

		constexpr std::int32_t ImportOfName(std::int32_t a_name) { return a_name < 0 ? a_name : (a_name & ~kMipBits); }

		/** @brief The mip a name views, or AnchorPasses::kAllMips for the target's own view. */
		constexpr std::uint32_t MipOfName(std::int32_t a_name)
		{
			const auto bits = a_name < 0 ? 0u : static_cast<std::uint32_t>((a_name & kMipBits) >> kMipShift);
			return bits ? bits - 1 : AnchorPasses::kAllMips;
		}

		struct EffectInfo
		{
			Effects::ImageSpaceEffectEnum id;  // index in ImageSpaceManager::effects
			std::string_view effectName;
			const char* source;                // under Data/Shaders
			std::uint32_t slots;               // SRV count in the source's ORG resource list
			std::uint32_t reads;               // bit per texture slot the shader reads with these defines
			std::uint32_t samplers;            // read slots below this count have a sampler
			std::uint32_t cubes = 0;           // bit per slot CS binds the reflection cubemap to itself
			bool compute = false;              // UAVs from u0, constants at CS b0
			std::uint32_t targets = 1;         // render targets, or a dispatch's UAVs
			// SAO composite only. Adds s9 and the six feature resources CS headers read.
			bool features = false;
			// The effect keeps the viewport the previous effect left, so each shape implies the full-target one.
			bool keepsViewport = false;
			// Draws its own geometry, not the quad. Its vertex stage pulls it by device address from the latch.
			bool geometry = false;
			bool vertexStage = false;
			// The vertex shader writes POSITION, not SV_Position, so the engine's draw rasterizes nothing.
			// We apply the engine state and draw nothing.
			bool rastersNothing = false;
			// Bit per slot that holds a loaded texture, not an engine target. Read by the heap index in the draw's latch.
			std::uint32_t latched = 0;
		};

		constexpr std::uint32_t kFeatureWords = 7;

		// Read masks follow each source's code for the effect's defines.
		constexpr auto kEffects = std::to_array<EffectInfo>({
			{ Effects::ISHDRDownSample16, "ISHDRDownSample16", "ISHDR.hlsl", 3, 0b001, 3 },
			// Also drawn outside the HDR effect into kLDR_DOWNSAMPLE0.
			{ Effects::ISHDRDownSample4, "ISHDRDownSample4", "ISHDR.hlsl", 3, 0b001, 3, 0, false },
			{ Effects::ISHDRDownSample16Lum, "ISHDRDownSample16Lum", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample4RGB2Lum, "ISHDRDownSample4RGB2Lum", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample4LumClamp, "ISHDRDownSample4LumClamp", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample16LumClamp, "ISHDRDownSample16LumClamp", "ISHDR.hlsl", 3, 0b001, 3 },
			{ Effects::ISHDRDownSample4LightAdapt, "ISHDRDownSample4LightAdapt", "ISHDR.hlsl", 3, 0b011, 3 },
			{ Effects::ISHDRDownSample16LightAdapt, "ISHDRDownSample16LightAdapt", "ISHDR.hlsl", 3, 0b011, 3 },
			{ Effects::ISBrightPassBlur3, "ISBrightPassBlur3", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur5, "ISBrightPassBlur5", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur7, "ISBrightPassBlur7", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur9, "ISBrightPassBlur9", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur11, "ISBrightPassBlur11", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur13, "ISBrightPassBlur13", "ISBlur.hlsl", 2, 0b11, 2 },
			{ Effects::ISBrightPassBlur15, "ISBrightPassBlur15", "ISBlur.hlsl", 2, 0b11, 2 },
			// Blur composites: t0 only, without BRIGHTPASS.
			{ Effects::ISBlur3, "ISBlur3", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISBlur5, "ISBlur5", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISBlur7, "ISBlur7", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISBlur9, "ISBlur9", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISBlur11, "ISBlur11", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISBlur13, "ISBlur13", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISBlur15, "ISBlur15", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISNonHDRBlur3, "ISNonHDRBlur3", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISNonHDRBlur5, "ISNonHDRBlur5", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISNonHDRBlur7, "ISNonHDRBlur7", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISNonHDRBlur9, "ISNonHDRBlur9", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISNonHDRBlur11, "ISNonHDRBlur11", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISNonHDRBlur13, "ISNonHDRBlur13", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISNonHDRBlur15, "ISNonHDRBlur15", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			// HDR tonemap: t0 bloom, t1 scene, t2 luminance. A passthrough with Post Processing on.
			{ Effects::ISHDRTonemapBlendCinematic, "ISHDRTonemapBlendCinematic", "ISHDR.hlsl", 3, 0b111, 3 },
			{ Effects::ISHDRTonemapBlendCinematicFade, "ISHDRTonemapBlendCinematicFade", "ISHDR.hlsl", 3, 0b111, 3 },
			{ Effects::ISCopy, "ISCopy", "ISCopy.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISCopyDynamicFetchDisabled, "ISCopyDynamicFetchDisabled", "ISCopy.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISCopyCustomViewport, "ISCopyCustomViewport", "ISCopy.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISCopyGrayScale, "ISCopyGrayScale", "ISCopy.hlsl", 1, 0b1, 1, 0, false },
			// These copies change coordinates in the vertex shader. ISCopy.hlsl's ORG vertex stage does the same.
			{ Effects::ISCopyScaleBias, "ISCopyScaleBias", "ISCopy.hlsl", 1, 0b1, 1, 0, false, 1, false, false, false, true },
			{ Effects::ISCopyTextureMask, "ISCopyTextureMask", "ISCopy.hlsl", 1, 0b1, 1, 0, false, 1, false, false, false, true },
			// t0 image, t1 its blurred copy.
			{ Effects::ISDoubleVision, "ISDoubleVision", "ISDoubleVision.hlsl", 2, 0b11, 2, 0, false },
			// Water displacement simulation. The ripple makers draw one quad per ripple.
			{ Effects::ISWaterDisplacementClearSimulation, "ISWaterDisplacementClearSimulation", "ISWaterDisplacement.hlsl", 0, 0, 0 },
			{ Effects::ISWaterDisplacementTexOffset, "ISWaterDisplacementTexOffset", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterWadingHeightmap, "ISWaterWadingHeightmap", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterRainHeightmap, "ISWaterRainHeightmap", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterBlendHeightmaps, "ISWaterBlendHeightmaps", "ISWaterDisplacement.hlsl", 2, 0b11, 2 },
			{ Effects::ISWaterSmoothHeightmap, "ISWaterSmoothHeightmap", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterDisplacementNormals, "ISWaterDisplacementNormals", "ISWaterDisplacement.hlsl", 1, 0b1, 1 },
			{ Effects::ISWaterDisplacementWadingRipple, "ISWaterDisplacementWadingRipple", "ISWaterDisplacement.hlsl", 0, 0, 0, 0, false, 1, false,
				false, true },
			{ Effects::ISWaterDisplacementRainRipple, "ISWaterDisplacementRainRipple", "ISWaterDisplacement.hlsl", 0, 0, 0, 0, false, 1, false,
				false, true },
			{ Effects::ISCompositeVolumetricLighting, "ISCompositeVolumetricLighting", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b01, 2 },
			{ Effects::ISCompositeLensFlare, "ISCompositeLensFlare", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b10, 2 },
			{ Effects::ISCompositeLensFlareVolumetricLighting, "ISCompositeLensFlareVolumetricLighting", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b11, 2 },
			// Image-based lens flares: downsample with jitter compensation, blur, then the flares.
			{ Effects::ISDownsample, "ISDownsample", "ISDownsample.hlsl", 3, 0b111, 3, 0, false },
			{ Effects::ISBlur, "ISBlur", "ISBlur.hlsl", 2, 0b01, 2, 0, false },
			{ Effects::ISIBLensFlares, "ISIBLensFlares", "ISIBLensFlare.hlsl", 1, 0b1, 1, 0, false },
			// The compute downsamples. ISDownsampleCS is the image-based flares' first downsample, with jitter compensation (t1 last
			// frame's result, t2 motion vectors). ISDownsampleIgnoreBrightestCS is the SSR mask's at reflection divider 4, without it.
			{ Effects::ISDownsampleCS, "ISDownsampleCS", "ISDownsampleCS.hlsl", 3, 0b111, 3, 0, true },
			{ Effects::ISDownsampleIgnoreBrightestCS, "ISDownsampleIgnoreBrightestCS", "ISDownsampleCS.hlsl", 3, 0b001, 3, 0, true },
			// Lens flares (SE 100274, in RenderWorldPostProcess). The visibility pass reads depth and writes a texel per light into
			// kLENSFLAREVIS. Each sprite draws the engine's sprite quad: t0 is its loaded texture, read by heap index, t1 the visibility.
			{ Effects::ISLensFlareVisibility, "ISLensFlareVisibility", "LensFlare.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISLensFlare, "ISLensFlare", "ISLensFlare.hlsl", 2, 0b11, 1, 0, false, 1, false, false, true, true, false, 0b01 },
			// t0 source, t1 indirect lighting.
			{ Effects::ISSILComposite, "ISSILComposite", "ISSILComposite.hlsl", 2, 0b11, 2 },
			// t0 depth, t1 normals, t2 motion vectors, t3 emissive, t4 accumulated indirect lighting.
			{ Effects::ISSILRawInd, "ISSILRawInd", "ISSILRawInd.hlsl", 5, 0b11111, 5 },
			{ Effects::ISSAOCameraZ, "ISSAOCameraZ", "ISSAOCameraZ.hlsl", 1, 0b1, 1 },
			{ Effects::ISMinify, "ISMinify", "ISSAOMinify.hlsl", 1, 0b1, 1 },
			{ Effects::ISMinifyContrast, "ISMinifyContrast", "ISSAOMinify.hlsl", 1, 0b1, 1 },
			{ Effects::ISSAORawAO, "ISSAORawAO", "ISSAORawAO.hlsl", 4, 0b1111, 4 },
			{ Effects::ISSAORawAONoTemporal, "ISSAORawAONoTemporal", "ISSAORawAO.hlsl", 4, 0b0011, 4 },
			{ Effects::ISSAOBlurH, "ISSAOBlurH", "ISSAOBlur.hlsl", 1, 0b1, 1 },
			{ Effects::ISSAOBlurV, "ISSAOBlurV", "ISSAOBlur.hlsl", 1, 0b1, 1 },
			// t0 source, t1 kSAO, t2 depth, t3 SSR mask, t4 snow specular alpha, t5 shadow mask, t6 reflections.
			// CS's IBL and fog headers also read their features' outputs.
			{ Effects::ISSAOCompositeSAO, "ISSAOCompositeSAO", "ISSAOComposite.hlsl", 7, 0b1111111, 7, 0, false, 1, true },
			{ Effects::ISSAOCompositeFog, "ISSAOCompositeFog", "ISSAOComposite.hlsl", 7, 0b1111111, 7, 0, false, 1, true },
			{ Effects::ISSAOCompositeSAOFog, "ISSAOCompositeSAOFog", "ISSAOComposite.hlsl", 7, 0b1111111, 7, 0, false, 1, true },
			// The compute SAO (Main::Draw, SE 100526 / AE 107252). Depth to the first three camera Z mips (u0 to u2), the raw AO from
			// t0 camera Z, t1 the other history and t2 motion vectors, then the two blur passes.
			{ Effects::ISSAOCameraZAndMipsCS, "ISSAOCameraZAndMipsCS", "ISSAOCameraZAndMipsCS.hlsl", 1, 0b1, 0, 0, true, 3 },
			{ Effects::ISSAORawAOCS, "ISSAORawAOCS", "ISSAORawAOCS.hlsl", 3, 0b111, 3, 0, true },
			{ Effects::ISSAOBlurHCS, "ISSAOBlurHCS", "ISSAOBlurCS.hlsl", 1, 0b1, 0, 0, true },
			{ Effects::ISSAOBlurVCS, "ISSAOBlurVCS", "ISSAOBlurCS.hlsl", 1, 0b1, 0, 0, true },
			// SSR: mask downsample, ray march (t4 is the cubemap CS binds), then the compute blur.
			{ Effects::ISDownsampleIgnoreBrightest, "ISDownsampleIgnoreBrightest", "ISDownsample.hlsl", 3, 0b001, 3 },
			{ Effects::ISReflectionsRayTracing, "ISReflectionsRayTracing", "ISReflectionsRayTracing.hlsl", 5, 0b11111, 4, 0b10000 },
			{ Effects::ISReflectionBlurHCS, "ISReflectionBlurHCS", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true },
			{ Effects::ISReflectionBlurVCS, "ISReflectionBlurVCS", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true },
			// End-of-frame chain. DoF: t0 image, t1 blurred, t2 depth, t3 average depth or t4 mask with MASKED.
			{ Effects::ISDepthOfField, "ISDepthOfField", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false },
			{ Effects::ISDepthOfFieldFogged, "ISDepthOfFieldFogged", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false },
			{ Effects::ISDepthOfFieldMaskedFogged, "ISDepthOfFieldMaskedFogged", "ISDepthOfField.hlsl", 5, 0b10111, 5, 0, false },
			{ Effects::ISDistantBlur, "ISDistantBlur", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false, 1, false, true },
			{ Effects::ISDistantBlurFogged, "ISDistantBlurFogged", "ISDepthOfField.hlsl", 5, 0b01111, 5, 0, false, 1, false, true },
			{ Effects::ISDistantBlurMaskedFogged, "ISDistantBlurMaskedFogged", "ISDepthOfField.hlsl", 5, 0b10111, 5, 0, false, 1, false, true },
			{ Effects::ISRefraction, "ISRefraction", "ISRefraction.hlsl", 2, 0b11, 2, 0, false, 1, false, true },
			{ Effects::ISRadialBlur, "ISRadialBlur", "ISRadialBlur.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISRadialBlurMedium, "ISRadialBlurMedium", "ISRadialBlur.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISRadialBlurHigh, "ISRadialBlurHigh", "ISRadialBlur.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISFXAA, "ISFXAA", "ISFXAA.hlsl", 1, 0b1, 1, 0, false },
			// World map depth blur. NoSkyBlur leaves the sky sharp.
			{ Effects::ISWorldMap, "ISWorldMap", "ISWorldMap.hlsl", 2, 0b11, 2, 0, false },
			{ Effects::ISWorldMapNoSkyBlur, "ISWorldMapNoSkyBlur", "ISWorldMap.hlsl", 2, 0b11, 2, 0, false },
			// Reads depth at t1 (DEPTH_THRESHOLD).
			{ Effects::ISDepthOfFieldBlurHCS, "ISDepthOfFieldBlurHCS", "ISBlurCS.hlsl", 2, 0b11, 2, 0, true },
			{ Effects::ISDepthOfFieldBlurVCS, "ISDepthOfFieldBlurVCS", "ISBlurCS.hlsl", 2, 0b11, 2, 0, true },
			{ Effects::ISParallaxMaskBlurHCS, "ISParallaxMaskBlurHCS", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true },
			{ Effects::ISParallaxMaskBlurVCS, "ISParallaxMaskBlurVCS", "ISBlurCS.hlsl", 2, 0b01, 2, 0, true },
			// t0 to u0, no sampler. Main::RenderWaterEffects' flow copies read their source by heap index.
			{ Effects::ISCopySubRegionCS, "ISCopySubRegionCS", "ISCopySubRegionCS.hlsl", 1, 0b1, 0, 0, true },
			// t0 image, t1 water history, t2 motion vectors, t3 depth, t4 water mask. Writes kMAIN and the next history.
			{ Effects::ISWaterBlend, "ISWaterBlend", "ISWaterBlend.hlsl", 5, 0b11111, 5, 0, false, 2 },
			// Water.hlsl's UNDERWATERMASK form over the engine's own geometry.
			{ Effects::ISUnderwaterMask, "ISUnderwaterMask", "Water.hlsl", 0, 0, 0, 0, false, 1, false, false, true },
			// t0 image, t1 history, t2 motion vectors, t3 depth, t4 TAA mask, t5 alpha. Writes the image and the next history.
			{ Effects::ISTemporalAA, "ISTemporalAA", "ISTemporalAA.hlsl", 6, 0b111111, 6, 0, false, 2 },
			// Vanilla's UI form (ISTemporalAAForms.hlsli), in a menu's 3D scene: t0 the scene, t1 the UI history, t2 motion vectors,
			// t3 depth, t4 the mask ISSimpleColor wrote. Writes the image and the next UI history.
			{ Effects::ISTemporalAA_UI, "ISTemporalAA_UI", "ISTemporalAA.hlsl", 5, 0b11111, 5, 0, false, 2 },
			// Vanilla's water form, in the world TAA's place with bTAAWater: the world form's slots, then t5 the normals (water in z)
			// and t6 the water history. Writes the image, the next history and the next water history.
			{ Effects::ISTemporalAA_Water, "ISTemporalAA_Water", "ISTemporalAA.hlsl", 7, 0x7F, 7, 0, false, 3 },
			{ Effects::ISLocalMap, "ISLocalMap", "ISLocalMap.hlsl", 1, 0b1, 1, 0, false },
			// t0 depth, t1 snow diffuse, t2 snow alpha and specular.
			// t1 the snow's diffuse, t2 its alpha and specular.
			{ Effects::ISSnowSSS, "ISSnowSSS", "ISSnowSSS.hlsl", 3, 0b111, 3, 0, false },
			{ Effects::ISSimpleColor, "ISSimpleColor", "ISSimpleColor.hlsl", 0, 0, 0, 0, false },
			// Sixteen loaded tint masks, read by heap index from the latch.
			{ Effects::ISAlphaBlend, "ISAlphaBlend", "ISAlphaBlend.hlsl", 16, 0xFFFF, 16, 0, false, 1, false, false, false, false, false, 0xFFFF },
			// t0 blurred reflections, t1 water mask, t2 image.
			{ Effects::ISApplyReflections, "ISApplyReflections", "ISApplyReflections.hlsl", 3, 0b111, 3, 0, false },
			// The directional form reads all eight slots. The plain form has no light, the menu form no SAO or shadow mask.
			{ Effects::ISLightingComposite, "ISLightingComposite", "ISLightingComposite.hlsl", 8, 0xFF, 8, 0, false },
			{ Effects::ISLightingCompositeNoDirectionalLight, "ISLightingCompositeNoDirectionalLight", "ISLightingComposite.hlsl", 8, 0b10011111, 8, 0,
				false},
			{ Effects::ISLightingCompositeMenu, "ISLightingCompositeMenu", "ISLightingComposite.hlsl", 8, 0b01110111, 8, 0, false },
			{ Effects::ISUpsampleDynamicResolution, "ISUpsampleDynamicResolution", "ISUpsampleDynamicResolution.hlsl", 1, 0b1, 1, 0, false },
			{ Effects::ISMap, "ISMap", "ISMap.hlsl", 0, 0, 0, 0, false, 1, false, false, false, false, true },
			{ Effects::ISNoiseScrollAndBlend, "ISNoiseScrollAndBlend", "ISNoise.hlsl", 0, 0, 0, 0, false, 1, false, false, false, false, true },
			{ Effects::ISNoiseNormalmap, "ISNoiseNormalmap", "ISNoise.hlsl", 0, 0, 0, 0, false, 1, false, false, false, false, true },
		});

		/** @brief An engine blend state packed by value from its four blend-table fields. kNoBlend for a dispatch. */
		constexpr std::uint32_t kNoBlend = UINT32_MAX;

		constexpr std::uint32_t PackBlend(std::uint32_t a_mode, std::uint32_t a_alphaToCoverage, std::uint32_t a_writeMode, std::uint32_t a_extra)
		{
			return (a_mode & 0xFF) | (a_alphaToCoverage & 0xFF) << 8 | (a_writeMode & 0xFF) << 16 | (a_extra & 0xFF) << 24;
		}

		/** @brief Reads a blend description from the engine's table, or from a copy kept earlier this session. */
		bool BlendDescOf(std::uint32_t a_blend, D3D11_BLEND_DESC& a_out);

		/** @brief As BlendDescOf, for a depth-stencil table index (depth mode * kStencilModes + stencil mode). */
		bool DepthDescOf(std::uint32_t a_index, D3D11_DEPTH_STENCIL_DESC& a_out);

		constexpr std::array<std::int32_t, kMaxSlots> NoInputs()
		{
			std::array<std::int32_t, kMaxSlots> inputs{};
			inputs.fill(-1);
			return inputs;
		}

		constexpr std::uint32_t kDepthModes = 6;
		ID3D11BlendState* BlendStateOf(std::uint32_t a_blend)
		{
			if (a_blend == kNoBlend)
				return nullptr;
			return Pipeline::EngineBlendState(a_blend & 0xFF, (a_blend >> 8) & 0xFF, (a_blend >> 16) & 0xFF, (a_blend >> 24) & 0xFF);
		}

		bool DepthWrites(const D3D11_DEPTH_STENCIL_DESC& a_desc)
		{
			const auto writes = [](const D3D11_DEPTH_STENCILOP_DESC& a_face) {
				return a_face.StencilFailOp != D3D11_STENCIL_OP_KEEP || a_face.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP || a_face.StencilPassOp != D3D11_STENCIL_OP_KEEP;
			};
			return (a_desc.DepthEnable && a_desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ||
			       (a_desc.StencilEnable && a_desc.StencilWriteMask != 0 && (writes(a_desc.FrontFace) || writes(a_desc.BackFace)));
		}

		constexpr std::uint32_t kStencilModes = 40;

		/** @brief What one pass is made for, by value. A compute shape's outputs are the targets behind its UAVs. */
		struct ShapeKey
		{
			std::uint32_t effect = 0;
			std::int32_t output = -1;
			std::int32_t output1 = -1;
			std::int32_t output2 = -1;  // a dispatch's third UAV
			std::array<std::int32_t, kMaxSlots> inputs = NoInputs();
			std::array<std::uint32_t, kMaxSlots> address{};
			std::array<std::uint32_t, kMaxSlots> filter{};
			std::uint32_t blend = kNoBlend;
			bool compute = false;
			// Viewport as float bits. Values within 1/64 of a pixel snap to it so dynamic resolution gives one shape.
			std::array<std::uint32_t, 6> viewport{};
			std::int32_t depth = -1;
			std::uint32_t depthMode = 0, stencilMode = 0, stencilRef = 0;
			bool scissored = false;
			std::array<std::int32_t, 4> scissor{};

			auto Tie() const
			{
				return std::tie(effect, output, output1, output2, inputs, address, filter, blend, compute, viewport, depth, depthMode, stencilMode, stencilRef, scissored, scissor);
			}
			bool operator<(const ShapeKey& a_other) const { return Tie() < a_other.Tie(); }
			bool operator==(const ShapeKey& a_other) const { return Tie() == a_other.Tie(); }
		};

		struct Spec
		{
			ShapeKey key;
			std::uint32_t entry = 0;  // also the latch region (entry * kLatchStride) and order key
			std::string name;
			FrameAnchors::Texture output;
			FrameAnchors::Texture output1;
			FrameAnchors::Texture output2;
			std::array<FrameAnchors::Texture, kMaxSlots> inputs;
			rhi::Format format = rhi::Format::Unknown;
			rhi::Format format1 = rhi::Format::Unknown;
			rhi::Format format2 = rhi::Format::Unknown;
			FrameAnchors::Texture depth;
			rhi::Format depthFormat = rhi::Format::Unknown;
			// The output's extent in this registration. A kept output's twin may hold the last version's until the revision adopts.
			std::uint32_t width = 0, height = 0;
			// The feature defines its program is built with, from its registration's key (kSsrPermutation), not the live settings.
			std::uint32_t permutation = 0;
			// A render-size step: the pass's render area is the whole output, and its vertex stage reads the viewport from the
			// latch (ConstantsAt::viewport), which the step's hook writes at each draw from the engine's ratios.
			bool latchViewport = false;
			// Its program, from the revision its build was made for (WantSpecProgram). Null when it has none.
			std::shared_ptr<const RasterProgram> raster;
			std::shared_ptr<const ComputeProgram> compute;
		};

		/** @brief Mirrors OrgEngineConstantsAt in Common/OrgEngineConstants.hlsli. */
		struct ConstantsAt
		{
			std::uint32_t srv = 0;  // PS b2, or a dispatch's CS b0
			std::uint32_t offset = 0;
			std::uint32_t frameSrv = 0;  // PS b12. Unused by a dispatch.
			std::uint32_t frameOffset = 0;
			std::uint32_t size = 0;  // rows past this read 0, as on D3D11
			std::uint32_t frameSize = 0;
			std::uint32_t source = UINT32_MAX;
			std::uint32_t padding = 0;
			// The draw's viewport in its render area, as fractions: scale x and y, then offset x and y. A pass made with the latch
			// viewport (Spec::latchViewport) draws its quad there, so a new ratio is a new value, not a new registration.
			std::array<float, 4> viewport{ 1.0f, 1.0f, 0.0f, 0.0f };
		};
		static_assert(sizeof(ConstantsAt) == 48);

		/** @brief Mirrors OrgEngineSourcesAt in Common/OrgEngineConstants.hlsli. */
		using SourcesAt = std::array<std::uint32_t, kMaxSlots>;
		static_assert(sizeof(SourcesAt) == 64);

		constexpr std::uint32_t kLatchStride = static_cast<std::uint32_t>(sizeof(ConstantsAt) + sizeof(SourcesAt));

		/** @brief One entry's whole latch region, as words. */
		using LatchWords = std::array<std::uint32_t, kLatchStride / 4>;

		struct Pending
		{
			std::uint32_t entry = 0;
			ConstantsAt at;
			bool compute = false;
			std::array<std::uint32_t, 3> groups{};
			std::shared_ptr<const void> source;  // held until the run's GPU work retires
			std::uint32_t vertices = 3;
			std::optional<SourcesAt> sources;
			// Written into the entry's latch region in place of at and sources. The lens flare sprites use it (ChainSteps.inl).
			std::optional<LatchWords> record;
			// False for an entry that only holds latch words another entry's pass reads. Its gate entry stays closed.
			bool open = true;
		};

		struct EffectScope
		{
			const RE::BSImagespaceShader* shader = nullptr;
			std::size_t effect = 0;
			bool covered = false;   // its draws are the graph's or refused, never vouched for
			bool eligible = false;
			bool drew = false;
			bool compute = false;
		};

		/** @brief One CS-stage ImageSpace client, with its own gate and latch. Its passes' specs are in each build's step table. */
		struct PostClient
		{
			const char* id = nullptr;
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> latch;
			std::vector<Pending> batch;
		};

		/** @brief Engine functions whose ImageSpace draws are batched. The batch runs at the scope's end at the latest. */
		enum class ScopeKind : std::uint32_t
		{
			Composites,   // 99024 / 105675
			HDR,          // 100465 / 107183
			SSAO,         // 100520 / 107245
			Reflections,  // 100428 / 107146
			Effect,
			FlowMap,
			Water,        // 31385 / 32176
			// The compute SAO (100526 / 107252), from Main::Draw after the world render.
			SaoCs,
			// A lens flare draw (SE 100274). Its batch stays open: the sprites share one pass, which opens once a run.
			LensFlare,
			Count
		};

		struct ImageSpaceState
		{
			bool installed = false;
			bool on = false;  // set by Setup, whose failures are fatal
			bool flushing = false;
			const void* drawShape = nullptr;
			std::uint32_t drawStart = 0, drawTriangles = 0;
			std::unordered_map<const void*, std::int32_t> effectOf;
			// Sources that latched draws read by heap index, held until the run's lease index runs again.
			// A stage's commit moves its staging run's list onto the run that submits it.
			RunSlots<std::vector<std::shared_ptr<const void>>> heldSources;
			std::map<std::pair<std::uint32_t, std::uint32_t>, org::OwnedDescriptorBinding> samplers;  // (address, filter)
			org::OwnedDescriptorBinding defaultSampler;  // D3D11's default sampler, for the SAO composite's s9
			// A 1x1x1 volume of (0, 0, 0, 1). The SAO composite reads it before the fog publishes its volumes, as D3D11 binds none then.
			winrt::com_ptr<ID3D11ShaderResourceView> noFogView;
			std::uint32_t noFogIndex = UINT32_MAX;
			std::shared_ptr<const void> noFogOwner;
			bool noFogTried = false;
			// Kept so a build still has the description when the engine's table entry is empty.
			std::map<std::uint32_t, D3D11_BLEND_DESC> blendDescs;
			std::map<std::uint32_t, D3D11_DEPTH_STENCIL_DESC> depthDescs;
			const ID3D11Texture2D* publishedFlowMap = nullptr;
			std::uint32_t scope = 0;
			std::uint32_t drawIndex = 0;
			std::uint32_t frame = UINT32_MAX;
			std::uint32_t scopeDepth = 0;
			std::optional<EffectScope> current;
			bool executed = false;
			// Innermost last.
			std::vector<const RE::BSImagespaceShader*> callShaders;
			std::string lastRefusal;
			// The engine has made its ImageSpace manager, effects and blend states. Until then each Begin raises the shape (RaiseShape).
			bool shapeMade = false;
			std::array<PostClient, 9> post{};
			// Between RenderSSAO's two calls, an own-scope copy-subregion call is the snow copy.
			bool snowWindow = false;
			std::vector<std::shared_ptr<const void>> postKeep;
			std::uint64_t taken = 0, refused = 0, batches = 0, lost = 0;
			// Refusals log at info, once per reason and effect.
			std::array<std::unique_ptr<Util::ReasonLog>, kEffects.size()> fallbacks;
		};

		ImageSpaceState& IS()
		{
			static ImageSpaceState state;
			return state;
		}

		/** @brief A covered effect's call CS does not take. Its quad draw is then a NoPass tap error and draws nothing (DrawTriShape). */
		void Refuse(std::string_view a_reason, std::size_t a_effect, std::string_view a_detail = {})
		{
			auto& state = IS();
			++state.refused;
			state.lastRefusal = a_reason;
			if (auto& log = state.fallbacks[a_effect])
				log->Report(a_reason, a_detail);
		}

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

		/** @brief The engine's shader object of a covered effect, or null before the manager made it. */
		const RE::BSImagespaceShader* ShaderOf(std::size_t a_effect)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			const std::uint32_t index = RE::ImageSpaceManager::GetCurrentIndex(kEffects[a_effect].id);
			// NiTArray::size() counts non-null entries and would cut off the last effects. free_idx() is one past the highest set index.
			if (!manager || index >= manager->effects.free_idx() || index >= manager->effects.capacity())
				return nullptr;
			return static_cast<const RE::BSImagespaceShader*>(manager->effects[static_cast<decltype(manager->effects)::size_type>(index)]);
		}

		void RaiseShape(std::string_view a_writer);

		std::int32_t EffectOf(const RE::BSImagespaceShader* a_shader)
		{
			auto& state = IS();
			// A miss rescans, since the manager makes some effects after the first lookups.
			if (const auto it = state.effectOf.find(a_shader); it != state.effectOf.end()) {
				if (ShaderOf(static_cast<std::size_t>(it->second)) == a_shader)
					return it->second;
				state.effectOf.erase(it);
			}
			for (std::size_t i = 0; i < kEffects.size(); ++i)
				if (ShaderOf(i) == a_shader) {
					state.effectOf.emplace(a_shader, static_cast<std::int32_t>(i));
					// The first sight of an effect object is the engine's creation event for it. Its pass needs a build with it.
					if (state.on)
						RaiseShape("ImageSpace: the engine made an effect");
					return static_cast<std::int32_t>(i);
				}
			return -1;
		}

		std::int32_t EffectNamed(std::string_view a_name)
		{
			for (std::size_t i = 0; i < kEffects.size(); ++i)
				if (kEffects[i].effectName == a_name)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/** @brief The engine target a field handle is (Targets::Identify). Empty for anything else: a loaded texture, a CS texture. */
		std::optional<Targets::TargetRef> IdentifyField(const ID3D11View* a_view)
		{
			if (!a_view || !Tripwire::IsHandle(a_view))
				return std::nullopt;
			return Targets::Identify(a_view);
		}

		/** @brief BSGraphics::State's black, white, grey and flat normal map. */
		constexpr std::uint32_t kDefaultTextures = 4;

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

		/**
		* @brief The engine target an SRV reads, as an input name, or -1. A mip view target (a field view of one mip of another target)
		* is named by its source with the mip. The default textures are not targets and are matched by pointer.
		*/
		std::int32_t InputOfView(ID3D11ShaderResourceView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return -1;
			if (const auto target = IdentifyField(a_view)) {
				if (target->kind == Targets::Kind::Depth)
					return target->view == Targets::View::Srv ? kDepthInput | static_cast<std::int32_t>(target->index) : -1;
				if (target->kind != Targets::Kind::Colour || target->view != Targets::View::Srv)
					return -1;
				// The source's own SRV reads every mip. A mip view target's SRV is another field naming the same source.
				const auto index = static_cast<std::int32_t>(target->index);
				if (renderer->GetRuntimeData().renderTargets[target->index].SRV == a_view)
					return index;
				return target->mip <= kMaxMip ? WithMip(index, target->mip) : -1;
			}
			for (std::uint32_t i = 0; i < kDefaultTextures; ++i)
				if (auto* texture = DefaultTextureOf(i); texture && texture->resourceView == a_view)
					return kDefaultTextureInput | static_cast<std::int32_t>(i);
			return -1;
		}

		/** @brief The colour target a UAV writes, as an input name, or -1. For the log. */
		std::int32_t TargetOfUav(const ID3D11UnorderedAccessView* a_view)
		{
			const auto target = IdentifyField(a_view);
			if (!target || target->kind != Targets::Kind::Colour || target->view != Targets::View::Uav)
				return -1;
			const auto index = static_cast<std::int32_t>(target->index);
			return target->mip != 0 && target->mip <= kMaxMip ? WithMip(index, target->mip) : index;
		}

		/** @brief The water's flow map (SE 527694 / AE 414616, the NiSourceTexture "FlowMap"). Null before it is made. */
		RE::NiSourceTexture*& FlowMapSlot()
		{
			static REL::Relocation<RE::NiSourceTexture**> slot{ REL::RelocationID(527694, 414616) };
			return *slot.get();
		}

		RE::BSGraphics::Texture* FlowMap()
		{
			auto* texture = FlowMapSlot();
			return texture ? texture->rendererTexture : nullptr;
		}

		/** @brief The flow map the flow copies write, or null. Unified Water installs one without a UAV and removes the copies. */
		ID3D11Texture2D* FlowCopyTexture()
		{
			const auto* map = FlowMap();
			return map && map->UAV && map->texture ? static_cast<ID3D11Texture2D*>(map->texture) : nullptr;
		}

		/**
		* @brief Makes the water flow map at Setup, the way the engine does at the first cell with flowing water (SE 31234).
		* The engine only makes it while the slot is empty, so it then uses ours and the pass can name it at registration. Main thread.
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

		ID3D11Texture2D* FlowMapTexture()
		{
			auto* map = FlowMap();
			return map && map->texture ? static_cast<ID3D11Texture2D*>(map->texture) : nullptr;
		}

		FrameAnchors::Texture FlowMapImport()
		{
			auto* map = FlowMap();
			return map ? FrameAnchors::ImportTexture(kFlowMapId, { .texture = FlowMapTexture(), .srv = map->resourceView, .uav = map->UAV }) : nullptr;
		}

		/** @brief Publishes the flow map's import with the view the water draws find at t8, so they read it by id. Main thread. */
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

		/** @brief The HDR scene texture of the chain's registration, null while HDR is off (ChainSteps.inl). */
		ID3D11Texture2D* ChainHdrTarget();

		/** @brief The graph's import of a ShapeKey name. Any mip resolves to its texture. */
		FrameAnchors::Texture ImportOf(std::int32_t a_name)
		{
			if (a_name < 0 || (a_name & kLatchSourceInput))
				return nullptr;
			if (a_name & kFlowMapOutput)
				return FlowMapImport();
			if (a_name & kHdrSceneOutput)
				return ORGHDRDisplay::Get().GraphOf(ChainHdrTarget());
			const auto index = IndexOfName(a_name);
			if (a_name & kDefaultTextureInput) {
				auto* texture = DefaultTextureOf(index);
				return texture ? FrameAnchors::ImportTexture(fmt::format("cs.d3d11.engine.default-texture.{}", index), { .texture = static_cast<ID3D11Texture2D*>(texture->texture), .srv = texture->resourceView, .immutable = true }) : nullptr;
			}
			if (a_name & kDepthInput)
				return FrameAnchors::EngineTexture(FrameAnchors::Depth(index));
			if (a_name & kCubeInput)
				return FrameAnchors::EngineTexture(FrameAnchors::Cubemap(index));
			return FrameAnchors::EngineTexture(FrameAnchors::Colour(index));
		}

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

		/** @brief Either view may be a tripwire handle. */
		bool IsFlowMapUav(const ID3D11UnorderedAccessView* a_view)
		{
			const auto* map = FlowMap();
			return a_view && map && map->UAV && Tripwire::Unwrap(a_view) == Tripwire::Unwrap(static_cast<const ID3D11UnorderedAccessView*>(map->UAV));
		}

		bool FlowCopyBound() { return IsFlowMapUav(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().CSUAV[0]); }

		std::array<std::uint32_t, 6> ViewportBits(const rhi::Viewport& a_viewport)
		{
			return { std::bit_cast<std::uint32_t>(a_viewport.x), std::bit_cast<std::uint32_t>(a_viewport.y), std::bit_cast<std::uint32_t>(a_viewport.width),
				std::bit_cast<std::uint32_t>(a_viewport.height), std::bit_cast<std::uint32_t>(a_viewport.minDepth), std::bit_cast<std::uint32_t>(a_viewport.maxDepth) };
		}

		/** @brief The engine's sampler table: 6 address modes by 5 filter modes. */
		constexpr std::uint32_t kSamplerAddressModes = 6;
		constexpr std::uint32_t kSamplerFilterModes = 5;

		/**
		* @brief Effect, the first two output formats, packed blend, depth-stencil format (Unknown for none), depth-stencil table index,
		* the feature define permutation (Spec::permutation), whether the vertex stage takes its viewport from the latch, and the
		* third output's format (Unknown for none).
		*/
		using ProgramKey = std::tuple<std::size_t, rhi::Format, rhi::Format, std::uint32_t, rhi::Format, std::uint32_t, std::uint32_t, bool, rhi::Format>;

		/** @brief The fullscreen vertex stage that draws a quad in the viewport its latch holds (ConstantsAt::viewport). */
		constexpr const char* kLatchViewportVertexShader = "RenderGraph/FullscreenLatchViewportVS.hlsl";

		/** @brief ISReflectionsRayTracing with ENABLESSR (Dynamic Cubemaps' EnabledSSR). The only feature define an effect's key carries. */
		constexpr std::uint32_t kSsrPermutation = 1;

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
		* @brief The viewport's pixels inside the output, rounded outward. A viewport that differs from that area goes to the draw,
		* so TEXCOORD0 still spans the engine's viewport. False when nothing is inside.
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
		* @brief The engine's sampler for an address and filter mode, built as the renderer builds its table (SE 75831 / AE 77653).
		* Made from the modes alone, so a pass has its sampler before the engine fills its table.
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
			// CS caps the engine's anisotropy at 8 in EngineHooks.cpp. The engine asks for 16.
			constexpr UINT kAnisotropy = 8;
			a_out = {};
			a_out.Filter = kFilter[a_filter];
			a_out.AddressU = a_out.AddressW = kAddress[a_address][0];
			a_out.AddressV = kAddress[a_address][1];
			a_out.MipLODBias = 0.0f;
			a_out.MaxAnisotropy = a_filter >= 3 ? kAnisotropy : 1;
			// The engine leaves LESS set after the comparison sampler, and only that filter reads it.
			a_out.ComparisonFunc = a_filter == 4 ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_NEVER;
			a_out.MinLOD = -FLT_MAX;
			a_out.MaxLOD = a_filter <= 1 ? 0.0f : FLT_MAX;
			return true;
		}

		/** @brief Made on first use. UINT32_MAX on failure. */
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

		/** @brief The most targets an effect writes: ISTemporalAA_Water's three. */
		constexpr std::uint32_t kMaxTargets = 3;

		bool BlendFor(std::uint32_t a_blend, std::uint32_t a_targets, std::array<rhi::BlendAttachment, kMaxTargets>& a_out, std::string& a_error)
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
			for (std::uint32_t target = 1; target < kMaxTargets; ++target)
				a_out[target] = target < a_targets && desc.IndependentBlendEnable ? blend.attachments[target] : blend.attachments[0];
			return true;
		}

		/** @brief The defines ShaderCache uses for the effect's D3D11 build, without the stage define. */
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

		/**
		* @brief LiveDefinesOf with the feature defines a registration's key carries in place of the live ones. ENABLESSR comes from
		* a_permutation for the SSR raymarch, so its program is the one its revision's EnabledSSR selects.
		*/
		std::vector<std::pair<std::wstring, std::wstring>> DefinesOf(std::size_t a_effect, std::uint32_t a_permutation = 0)
		{
			auto defines = LiveDefinesOf(a_effect);
			if (kEffects[a_effect].id == Effects::ISReflectionsRayTracing) {
				std::erase_if(defines, [](const auto& a_define) { return a_define.first == "ENABLESSR"; });
				if (a_permutation & kSsrPermutation)
					defines.emplace_back("ENABLESSR", "");
			}
			return AnchorPasses::Defines(defines);
		}

		/** @brief Made on first use. UINT32_MAX if it could not be made. Render thread. */
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
			// Immutable and never written, so the graph exempts it from declarations.
			auto binding = ::DCLF::GpuTextures::Get().ResolveBinding(state.noFogView.get(), kImageSpaceTag);
			if (!binding) {
				logger::warn("[ORG] ImageSpace: the no-fog volume has no heap index");
				return UINT32_MAX;
			}
			state.noFogIndex = binding.index;
			state.noFogOwner = std::move(binding.owner);
			return state.noFogIndex;
		}

		/**
		* @brief Binds for an effect that reads CS's feature headers. Each included header reads its feature's published output,
		* which orders the pass after that feature. False with a_missing when the feature is not on the graph.
		*/
		bool FeatureBinds(std::size_t a_effect, std::vector<AnchorPasses::Bind>& a_out, std::string& a_missing)
		{
			auto& state = IS();
			const auto defines = LiveDefinesOf(a_effect);
			const auto has = [&](std::string_view a_name) { return std::ranges::any_of(defines, [&](const auto& a_define) { return a_define.first == a_name; }); };
			const bool fog = has("EXP_HEIGHT_FOG") && has("APPLY_FOG");
			// CS binds nothing at s9 here, so D3D11 reads its default linear clamp sampler.
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
				// IsDeclarable notes a refused id. Its later publish rebuilds every graph and this pass then reads it.
				if (PublishedResources::IsDeclarable(a_id)) {
					a_out.push_back(AnchorPasses::Bind::Published(a_id));
					return true;
				}
				// The fog publishes its volumes at its first graph frame, and never while volumetric fog is off.
				// Until then read the no-fog volume, as D3D11 reads a null view.
				if (a_fogVolume) {
					if (const auto index = NoFogVolume(); index != UINT32_MAX) {
						a_out.push_back(AnchorPasses::Bind::Handle(index));
						return true;
					}
				}
				a_missing = std::string(a_id);
				return false;
			};
			// The fog header reads Physical Sky's transmittance LUT for the directional light's inscattering.
			return read(has("IBL"), ORGIBL::kEnvId, false) && read(has("IBL"), ORGIBL::kSkyId, false) && read(fog, ORGExponentialHeightFog::kNearIntegratedId, true) &&
			       read(fog, ORGExponentialHeightFog::kFarIntegratedId, true) && read(fog && has("DYNAMIC_CUBEMAPS"), ORGDynamicCubemaps::kEnvBC6HId, false) &&
			       read(fog && has("PHYSICAL_SKY"), ORGPhysicalSky::kTransmittanceLutId, false);
		}

		/** @brief False when the engine's table has not had the entry yet. */
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

		/** @brief Needs the engine's effect object. Blend and depth-stencil must already be checked. */
		RasterProgram::Desc DescribeRaster(const ProgramKey& a_key)
		{
			const auto& [effectIndex, format, format1, blendBits, depthFormat, depthIndex, permutation, latchViewport, format2] = a_key;
			const auto& effect = kEffects[effectIndex];
			std::array<rhi::BlendAttachment, kMaxTargets> blend{};
			std::string error;
			BlendFor(blendBits, effect.targets, blend, error);
			RasterProgram::Desc desc{};
			desc.pixelSource = effect.source;
			if (effect.geometry || effect.vertexStage)
				desc.vertexSource = effect.source;
			// FXC-era sources use vector conditions in ?:.
			desc.languageVersion = L"2018";
			// Shared (4), SRVs, samplers, feature handles, latch (2), sources' latch (2).
			desc.constantWords = 4 + effect.slots - static_cast<std::uint32_t>(std::popcount(effect.latched)) + effect.samplers +
			                     (effect.features ? kFeatureWords : 0) + 2 + (effect.latched ? 2 : 0);
			desc.defines = DefinesOf(effectIndex, permutation);
			if (latchViewport && !effect.geometry && !effect.vertexStage) {
				// The latch's handle and offset are the push words before the sources' latch, as MakePass binds them.
				const auto word = desc.constantWords - 2 - (effect.latched ? 2 : 0);
				desc.vertexSource = kLatchViewportVertexShader;
				desc.defines.emplace_back(L"ORG_VIEWPORT_LATCH_WORD", std::to_wstring(word));
			}
			desc.targets = { { .format = format, .blend = blend[0] } };
			if (format1 != rhi::Format::Unknown)
				desc.targets.push_back({ .format = format1, .blend = blend[1] });
			if (format2 != rhi::Format::Unknown)
				desc.targets.push_back({ .format = format2, .blend = blend[2] });
			if (depthFormat != rhi::Format::Unknown) {
				desc.depthFormat = depthFormat;
				DepthStateFor(depthIndex, desc.depth);
			}
			return desc;
		}

		/** @brief A cube slot CS binds with its own sampler does not count. */
		bool SlotRead(const Spec& a_spec, std::uint32_t a_slot)
		{
			const auto& effect = kEffects[a_spec.key.effect];
			if (a_slot >= kMaxSlots || (effect.cubes & (1u << a_slot)))
				return false;
			const auto name = a_spec.key.inputs[a_slot];
			return a_spec.inputs[a_slot] || (name >= 0 && (name & kLatchSourceInput));
		}

		/** @brief DispatchComputeShader dispatches technique 0 (100952 / 107734). */
		ComputeProgram::Desc DescribeCompute(std::size_t a_effect)
		{
			const auto& effect = kEffects[a_effect];
			ComputeProgram::Desc desc{};
			desc.source = effect.source;
			// Shared (4), SRVs, samplers, UAVs, latch (2).
			desc.constantWords = 4 + effect.slots + effect.samplers + effect.targets + 2;
			desc.defines = DefinesOf(a_effect);
			return desc;
		}

		ProgramKey KeyOf(const Spec& a_spec)
		{
			const auto depthIndex = a_spec.depth ? a_spec.key.depthMode * kStencilModes + a_spec.key.stencilMode : 0u;
			return { a_spec.key.effect, a_spec.format, a_spec.format1, a_spec.key.blend, a_spec.depth ? a_spec.depthFormat : rhi::Format::Unknown, depthIndex,
				a_spec.permutation, a_spec.latchViewport, a_spec.format2 };
		}

		/** @brief Whether the blend and depth-stencil states are known yet. a_error says which is not. */
		bool StatesKnown(const Spec& a_spec, std::string& a_error)
		{
			std::array<rhi::BlendAttachment, kMaxTargets> blend{};
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
		* @brief The programs, by key. A step group build wants them as it makes its specs (WantSpecProgram). Made at the first step
		* group's setup, on the render owner.
		*/
		Frame::ProgramSet<ProgramKey, RasterProgram>& RasterPrograms()
		{
			static Frame::ProgramSet<ProgramKey, RasterProgram> programs{ "ImageSpace.Programs", [](const ProgramKey& a_key) { return DescribeRaster(a_key); } };
			return programs;
		}

		Frame::ProgramSet<std::size_t, ComputeProgram>& ComputePrograms()
		{
			static Frame::ProgramSet<std::size_t, ComputeProgram> programs{ "ImageSpace.ComputePrograms",
				[](const std::size_t& a_effect) { return DescribeCompute(a_effect); } };
			return programs;
		}

		/** @brief One texel load per pixel, by target format: the post chain's capture (ChainSteps.inl, CaptureProgram). */
		Frame::ProgramSet<rhi::Format, RasterProgram>& CapturePrograms()
		{
			struct CopyBlock
			{
				ShaderBindings::Handle colorInput;
			};
			static Frame::ProgramSet<rhi::Format, RasterProgram> programs{ "ImageSpace.CapturePrograms", [](const rhi::Format& a_format) {
				return RasterProgram::Desc{ .pixelSource = "Upscaling/CopyHudlessPS.hlsl",
					.constantWords = ShaderBindings::Words<CopyBlock>(),
					.targets = { { .format = a_format } } };
			} };
			return programs;
		}

		/** @brief What a step group's clients depend on, so a build that missed a program is made again once it is built. */
		Frame::Dependencies ProgramDependencies()
		{
			return Frame::DependsOn(RasterPrograms().Var(), ComputePrograms().Var(), CapturePrograms().Var());
		}

		/** @brief The revision a step group build reads its programs for. Set by StepGroup::For while it builds. Render thread. */
		const Frame::Inputs*& ProgramInputs()
		{
			static const Frame::Inputs* s_inputs = nullptr;
			return s_inputs;
		}

		/** @brief Render thread. Set while a step group is built and one of its programs is not in its revision (StepGroup::For). */
		bool& SpecProgramsMissing()
		{
			static bool s_missing = false;
			return s_missing;
		}

		/**
		* @brief a_key's program in the revision a step group build reads. One the revision has not built is wanted for the next
		* revision, and the build is dropped (SpecProgramsMissing). Outside a build (WantMenuPrograms) it is only wanted, so it
		* builds ahead. Render thread.
		*/
		template <class Key, class P>
		std::shared_ptr<const P> SpecProgram(Frame::ProgramSet<Key, P>& a_set, const std::type_identity_t<Key>& a_key)
		{
			const auto* in = ProgramInputs();
			std::shared_ptr<const P> program = in ? a_set.For(*in, a_key) : nullptr;
			if (!program) {
				a_set.Want(a_key);
				if (in)
					SpecProgramsMissing() = true;
			}
			return program;
		}

		/**
		* @brief Render thread, when a step's spec is made in a step group build. Sets its program from the build's revision. One the
		* revision has not built drops the build (SpecProgram): its clients keep their registrations, and the next revision makes it
		* again. A spec whose effect or states the engine has not made yet wants nothing: its pass is left out, as MakePass logs.
		*/
		void WantSpecProgram(Spec& a_spec)
		{
			if (!ShaderOf(a_spec.key.effect))
				return;
			if (a_spec.key.compute) {
				a_spec.compute = SpecProgram(ComputePrograms(), a_spec.key.effect);
				return;
			}
			std::string error;
			if (!StatesKnown(a_spec, error))
				return;
			a_spec.raster = SpecProgram(RasterPrograms(), KeyOf(a_spec));
		}

		/**
		* @brief Makes a spec's pass at a graph build on the render thread. Returns null to leave it out of that build.
		* The gate entry then stays closed, so its draws are refused.
		*/
		std::shared_ptr<org::RenderPass> MakePass(const std::shared_ptr<const Spec>& a_spec, const std::shared_ptr<const org::LatchBlock>& a_latch,
			const std::shared_ptr<const RunGate>& a_gate, const char* a_clientId)
		{
			const auto& spec = *a_spec;
			const auto& effect = kEffects[spec.key.effect];
			// The cases below that leave a pass out are states not reached yet. The engine shape counts the effects and blend states
			// the engine has made, and a publish rebuilds every graph, so a later build has the pass. Until then the engine cannot
			// draw the effect either, or the step's draw is a NoPass tap error.
			if (!ShaderOf(spec.key.effect)) {
				logger::info("[ORG] ImageSpace: {}: the engine has not made its effect yet; its pass is left out of this build", spec.name);
				return nullptr;
			}
			// The engine fills its blend and depth-stencil tables after Setup, so read them here.
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
					RenderGraph::Fatal("ImageSpace", fmt::format("{}: sampler {} (address {}, filter {}) could not be made", spec.name, slot,
															 spec.key.address[slot], spec.key.filter[slot]));
				}
			}
			// Never built here. The spec read it from its build's revision, and its clients registered once it was built.
			const std::shared_ptr<const RasterProgram> raster = spec.key.compute ? nullptr : spec.raster;
			const std::shared_ptr<const ComputeProgram> compute = spec.key.compute ? spec.compute : nullptr;
			if (!raster && !compute) {
				logger::error("[ORG] ImageSpace: {} has no program (its effect or states were not known when its spec was made); its pass is left out of the graph",
					spec.name);
				return nullptr;
			}
			// Resource list order: shared constants, t0.., s0.., u0.., constants location.
			std::vector<AnchorPasses::Bind> bindings;
			bindings.push_back(AnchorPasses::Bind::Shared());
			// A latched slot is read by heap index and has no SRV entry.
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
			if (effect.compute) {
				const std::array<std::pair<FrameAnchors::Texture, std::int32_t>, 3> uavs{ { { spec.output, spec.key.output }, { spec.output1, spec.key.output1 },
					{ spec.output2, spec.key.output2 } } };
				for (std::uint32_t u = 0; u < effect.targets && u < uavs.size(); ++u) {
					if (!uavs[u].first)
						RenderGraph::Fatal("ImageSpace", fmt::format("{}: its effect writes {} UAVs and its step names no u{}", spec.name, effect.targets, u));
					bindings.push_back(AnchorPasses::Bind::Uav(uavs[u].first, MipOfName(uavs[u].second)));
				}
			}
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
				// The engine's draw: it runs only when the engine makes it, whatever later draws read.
				dispatch.mayNotWrite = true;
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
				if (spec.output2)
					draw.extraTargets.push_back(spec.output2);
				const auto mip = MipOfName(spec.key.output);
				const auto shift = mip == AnchorPasses::kAllMips ? 0u : mip;
				bool exact = false;
				if (!RenderAreaOf(spec.key, (std::max)(spec.width >> shift, 1u), (std::max)(spec.height >> shift, 1u), draw.viewport, exact))
					RenderGraph::Fatal("ImageSpace", fmt::format("{}: its viewport is outside its output", spec.name));
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
				// The engine's draw: it runs only when the engine makes it, whatever later draws read.
				draw.mayNotWrite = true;
				builder.Add(std::move(draw));
			}
			auto passes = builder.Take();
			if (builder.Failed() || passes.empty() || !passes.front().make)
				RenderGraph::Fatal("ImageSpace", fmt::format("{} could not be made ({})", spec.name, builder.Failure()));
			return passes.front().make();
		}

		/**
		* @brief ImageSpace steps without the engine's draw (docs/development/plans/dclf-native-imagespace.md). CS_ORG_IMAGESPACE_NATIVE:
		* 0, the capture (the engine draws, its state is applied and its constants captured); 1, observe: as 0, and each covered draw's
		* rows are also computed from the engine's param and compared with the capture; 2 (default), draw: those draws are staged
		* from the computed rows, and the engine's Render returns at once.
		*/
		namespace NativeSteps
		{
			enum class Mode : std::uint8_t
			{
				Off,
				Observe,
				Draw,
				Hybrid,  // 3, a diagnostic: the computed rows are staged at the engine's draw, and the engine's Render still runs
			};

			Mode s_mode = Mode::Off;

			/** @brief The PerFrame bytes a step reads (FrameBuffer, c0-c44). */
			constexpr std::uint32_t kFrameBytes = static_cast<std::uint32_t>(sizeof(globals::game::frameBufferCached.data));

			/** @brief A draw's PerGeometry (PS b2) as the engine's SetupGeometry would write it, and which rows it wrote. */
			struct Rows
			{
				std::array<std::byte, 1024> bytes{};
				std::uint32_t size = 0;  // 0: the engine writes no PerGeometry rows for it
				std::array<std::uint8_t, 64> offsets{};  // each written row's offset in floats
				std::uint32_t count = 0;
			};

			struct State
			{
				bool armed = false;  // StageStep stages rows from here, not from a capture
				bool computeRows = false;  // with armed, a compute step's CS b0 rows are these (the native post chain), not the engine's buffer
				bool observing = false;  // StageStep compares the capture with rows
				bool observingTextures = false;  // the whole observed draw: the texture-list checks compare the bound views
				Rows rows;
				const RE::BSImagespaceShader* shader = nullptr;  // the step's shader, while armed or observing
				// A lens flare sprite's draw: its vertex rows, its quad and the texture at slot 0.
				struct Sprite
				{
					Rows vertex;
					const void* shape = nullptr;  // BSGraphics::TriShape (the geometry's renderer data)
					std::uint32_t triangles = 0;
					ID3D11ShaderResourceView* view = nullptr;  // a loaded texture, unwrapped
					std::int32_t target = -1;  // or an engine target
				} sprite;
				// The rows already in the ring: an explicit step's PS b2 as its setter bound it. Null to upload rows.
				Constants::Location rowsAt;
				std::size_t effect = 0;
				// The PerFrame bytes uploaded last, reused while they do not change.
				std::array<std::byte, kFrameBytes> frame{};
				Constants::Location frameAt;
				std::uint64_t frameAtFrame = ~0ull;
				std::uint64_t taken = 0, compared = 0, differed = 0, declined = 0;
			};

			State& Get()
			{
				static State state;
				return state;
			}

			/**
			* @brief A draw's engine targets from its effect's texture list, as base Render and the generic SetupGeometry bind them: the
			* output is texture 0 (render target 0), input slot i is texture i + 1. -1 for a loaded texture or none.
			*/
			void TextureTargets(const RE::BSImagespaceShader& a_shader, std::int32_t& a_input, std::int32_t& a_output)
			{
				const auto& textures = static_cast<const RE::ImageSpaceEffect&>(a_shader).effectTextures;
				const auto targetOf = [&](std::uint16_t a_index) {
					const auto* texture = a_index < textures.capacity() ? textures[a_index] : nullptr;
					return texture ? static_cast<std::int32_t>(texture->renderTarget) : -1;
				};
				a_output = targetOf(0);
				a_input = targetOf(1);
			}

			/** @brief Whether a_param is an ImageSpaceShaderParam (its vtable), whose constant groups the generic SetupGeometry reads. */
			bool IsShaderParam(const void* a_param)
			{
				static const auto vtable = RE::VTABLE_ImageSpaceShaderParam[0].address();
				return a_param && *static_cast<const std::uintptr_t*>(a_param) == vtable;
			}

			/**
			* @brief What input slot a_slot of a draw reads, from its effect's texture list as the generic SetupGeometry binds it (texture
			* a_slot + 1, 0x1414e8bc0): an engine target in a_target, else a loaded texture's view (unwrapped) in a_view. False for a depth
			* texture, which the list names by its own index.
			*/
			bool SlotTexture(const RE::BSImagespaceShader& a_shader, std::uint32_t a_slot, std::int32_t& a_target, ID3D11ShaderResourceView*& a_view)
			{
				a_target = -1;
				a_view = nullptr;
				const auto& textures = static_cast<const RE::ImageSpaceEffect&>(a_shader).effectTextures;
				const auto index = static_cast<std::uint16_t>(a_slot + 1);
				const auto* texture = index < textures.capacity() ? textures[index] : nullptr;
				if (!texture)
					return true;
				if (texture->renderTarget >= 0) {
					a_target = texture->renderTarget;
					return true;
				}
				if (*reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::byte*>(texture) + 0xC) >= 0)
					return false;
				if (texture->texture) {
					const auto* renderer = *reinterpret_cast<const std::byte* const*>(reinterpret_cast<const std::byte*>(texture->texture.get()) + 0x48);
					a_view = renderer ? Tripwire::Unwrap(*reinterpret_cast<ID3D11ShaderResourceView* const*>(renderer + 0x10)) : nullptr;
				}
				return true;
			}

			/** @brief The one pixel shader of an ImageSpace shader, or null. Its table places the param's rows. */
			const RE::BSGraphics::PixelShader* PixelShaderOf(const RE::BSImagespaceShader& a_shader)
			{
				const RE::BSGraphics::PixelShader* found = nullptr;
				for (const auto& entry : a_shader.pixelShaders) {
					if (found)
						return nullptr;
					found = entry;
				}
				return found;
			}

			/**
			* @brief The rows the generic SetupGeometry (AE 0x1414f8500) copies into PS b2: the param's pixel constants (+0x40), one float4 per
			* name the shader lists (+0x160), each at its offset in the pixel shader's table. None when the shader's +0x1A0 is set.
			*/
			const char* BuildRows(const RE::BSImagespaceShader& a_shader, const void* a_param, Rows& a_out)
			{
				a_out = {};
				Get().rowsAt = {};
				const auto* base = reinterpret_cast<const std::byte*>(&a_shader);
				if (*reinterpret_cast<const bool*>(base + 0x1A0))
					return nullptr;
				const auto count = *reinterpret_cast<const std::uint32_t*>(base + 0x160);
				if (count == 0)
					return nullptr;
				if (count > a_out.offsets.size())
					return "more pixel constants than a table holds";
				if (!IsShaderParam(a_param))
					return "a param that is not an ImageSpaceShaderParam";
				const auto* param = static_cast<const RE::ImageSpaceShaderParam*>(a_param);
				// Both groups count float4 rows.
				if (!param->pixelConstantGroup || param->pixelConstantGroupSize < count)
					return "a param with too few pixel constants";
				const auto* pixel = PixelShaderOf(a_shader);
				if (!pixel)
					return "not exactly one pixel shader";
				for (std::uint32_t row = 0; row < count; ++row) {
					const auto offset = static_cast<std::uint8_t>(pixel->constantTable[row]);
					const auto end = (offset + 4u) * 4u;
					if (end > a_out.bytes.size())
						return "a constant past the rows a step holds";
					std::memcpy(a_out.bytes.data() + offset * 4u, param->pixelConstantGroup + row * 4u, 16);
					a_out.offsets[row] = offset;
					a_out.size = (std::max)(a_out.size, (end + 15u) & ~15u);
				}
				a_out.count = count;
				return nullptr;
			}

			/**
			* @brief An explicit step's rows (the SAO family, SSR): its per-shader setter (ISMinify's at AE 0x14151f710 and the rest) maps an
			* engine pool buffer, copies its struct and binds it at PS b2 on the context before the explicit Render. The capture tracks that
			* bind and answers the map from CS memory, so the rows are read here without applying the engine's state.
			*/
			const char* BoundRows(Rows& a_out)
			{
				a_out = {};
				auto& native = Get();
				native.rowsAt = {};
				const auto use = Constants::Capture(Constants::StageBit(Constants::Stage::Pixel));
				if (!use.Valid())
					return "the capture is not active";
				const auto* slot = Constants::Find(use, Constants::Stage::Pixel, 2);
				if (!slot)
					return nullptr;
				const auto bytes = Constants::Bytes(*slot);
				const auto at = Constants::Where(*slot);
				if (!at.owner)
					return "bound rows the ring does not hold";
				// The ring holds them all; the copy is only what observe mode compares.
				std::memcpy(a_out.bytes.data(), bytes.data(), (std::min)(bytes.size(), a_out.bytes.size()));
				a_out.size = static_cast<std::uint32_t>(bytes.size());
				a_out.count = (std::min)(a_out.size / 16u, static_cast<std::uint32_t>(a_out.offsets.size()));
				for (std::uint32_t row = 0; row < a_out.count; ++row)
					a_out.offsets[row] = static_cast<std::uint8_t>(row * 4);
				native.rowsAt = at;
				return nullptr;
			}

			/**
			* @brief A lens flare sprite's draw as the generic SetupGeometry (AE 0x1414f8500) and RenderPassImmediately would make it: VS b2 row
			* table[0] = (0, 0, 0.5 / width, 0.5 / height) of the output at the dynamic-resolution ratio, rows 1 to n - 1 (n at +0x148) from the
			* param's vertex constants (+0x30); the quad's whole index range; slot 0 from texture 1 of the effect's list (0x1414e8bc0: an
			* engine target, else the loaded texture's view at +0x48 -> +0x10).
			*/
			const char* BuildSprite(const RE::BSImagespaceShader& a_shader, const void* a_param, const void* a_geometry, State::Sprite& a_out)
			{
				a_out = {};
				const auto* base = reinterpret_cast<const std::byte*>(&a_shader);
				const auto* param = static_cast<const RE::ImageSpaceShaderParam*>(a_param);
				const RE::BSGraphics::VertexShader* vertex = nullptr;
				for (const auto& entry : a_shader.vertexShaders) {
					if (vertex)
						return "not exactly one vertex shader";
					vertex = entry;
				}
				if (!vertex)
					return "no vertex shader";
				const auto& textures = static_cast<const RE::ImageSpaceEffect&>(a_shader).effectTextures;
				const auto* output = textures.capacity() > 0 ? textures[0] : nullptr;
				// The manager is the object at this address (CommonLib's GetSingleton reads it as a pointer).
				static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> managerAt{ REL::RelocationID(524970, 411451) };
				const auto* manager = managerAt.get();
				const auto* graphics = globals::game::graphicsState;
				if (!output || output->renderTarget < 0 || output->renderTarget >= RE::RENDER_TARGET::kTOTAL || !manager || !graphics)
					return "no output target";
				if (!*reinterpret_cast<const bool*>(base + 0x1A0)) {
					const auto count = *reinterpret_cast<const std::uint32_t*>(base + 0x148);
					if (count > vertex->constantTable.size())
						return "more vertex constants than a table holds";
					if (count > 1 && (!IsShaderParam(a_param) || !param->vertexConstantGroup || param->vertexConstantGroupSize < count - 1))
						return "a param with too few vertex constants";
					const auto& data = graphics->GetRuntimeData();
					const bool locked = data.dynamicResolutionLock != 0;
					const auto& target = manager->renderTargetData[output->renderTarget];
					const auto scaled = [](std::uint32_t a_size, float a_ratio) {
						return static_cast<float>(static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(a_size) * a_ratio)));
					};
					for (std::uint32_t row = 0; row < count; ++row) {
						const auto offset = static_cast<std::uint8_t>(vertex->constantTable[row]);
						if ((offset + 4u) * 4u > a_out.vertex.bytes.size())
							return "a vertex constant past the rows a step holds";
						float values[4]{};
						if (row == 0) {
							values[2] = 0.5f / scaled(target.width, locked ? 1.0f : data.dynamicResolutionWidthRatio);
							values[3] = 0.5f / scaled(target.height, locked ? 1.0f : data.dynamicResolutionHeightRatio);
						} else {
							std::memcpy(values, param->vertexConstantGroup + (row - 1) * 4u, 16);
						}
						std::memcpy(a_out.vertex.bytes.data() + offset * 4u, values, 16);
						a_out.vertex.offsets[row] = offset;
						a_out.vertex.size = (std::max)(a_out.vertex.size, ((offset + 4u) * 4u + 15u) & ~15u);
					}
					a_out.vertex.count = count;
				}
				const auto* geometry = static_cast<const RE::BSGeometry*>(a_geometry);
				if (!geometry || geometry->GetType().get() != RE::BSGeometry::Type::kTriShape)
					return "a sprite that is not a TriShape";
				a_out.shape = geometry->GetGeometryRuntimeData().rendererData;
				a_out.triangles = static_cast<const RE::BSTriShape*>(geometry)->GetTrishapeRuntimeData().triangleCount;
				const auto* input = textures.capacity() > 1 ? textures[1] : nullptr;
				if (input && input->renderTarget >= 0) {
					a_out.target = input->renderTarget;
				} else if (input && *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::byte*>(input) + 0xC) >= 0) {
					return "a depth texture at slot 0";
				} else if (input && input->texture) {
					const auto* renderer = *reinterpret_cast<const std::byte* const*>(reinterpret_cast<const std::byte*>(input->texture.get()) + 0x48);
					a_out.view = renderer ? Tripwire::Unwrap(*reinterpret_cast<ID3D11ShaderResourceView* const*>(renderer + 0x10)) : nullptr;
				}
				return nullptr;
			}

			/** @brief A geometry step's quad (a wading ripple): the TriShape's renderer data and its whole index range, as the dispatcher draws it. */
			const char* BuildShape(const void* a_geometry, State::Sprite& a_out)
			{
				a_out = {};
				const auto* geometry = static_cast<const RE::BSGeometry*>(a_geometry);
				if (!geometry || geometry->GetType().get() != RE::BSGeometry::Type::kTriShape)
					return "a geometry that is not a TriShape";
				a_out.shape = geometry->GetGeometryRuntimeData().rendererData;
				a_out.triangles = static_cast<const RE::BSTriShape*>(geometry)->GetTrishapeRuntimeData().triangleCount;
				return nullptr;
			}

			/** @brief The PerFrame bytes for a step: CS's FrameBuffer, uploaded again only when it changed. */
			Constants::Location FrameLocation()
			{
				auto& native = Get();
				const auto frame = Frame::Driver::Current().frame;
				const auto& now = globals::game::frameBufferCached.data;
				if (native.frameAt.owner && native.frameAtFrame == frame && std::memcmp(native.frame.data(), &now, kFrameBytes) == 0)
					return native.frameAt;
				std::memcpy(native.frame.data(), &now, kFrameBytes);
				native.frameAt = Constants::Upload(native.frame);
				native.frameAtFrame = frame;
				return native.frameAt;
			}

			Util::ReasonLog& Differences()
			{
				static Util::ReasonLog log("[ORG] ImageSpace native: a step's rows differ from the capture", spdlog::level::warn);
				return log;
			}

			/** @brief Observe mode: the captured PerGeometry and PerFrame against the computed ones. Only the rows the engine writes are compared. */
			void Compare(std::size_t a_effect, std::span<const std::byte> a_pixel, std::span<const std::byte> a_frame)
			{
				auto& native = Get();
				const auto& rows = native.rows;
				++native.compared;
				std::string differences;
				for (std::uint32_t row = 0; row < rows.count; ++row) {
					const auto at = rows.offsets[row] * 4u;
					for (std::uint32_t c = 0; c < 4; ++c) {
						float computed = 0.0f, captured = 0.0f;
						std::memcpy(&computed, rows.bytes.data() + at + c * 4, 4);
						if (at + c * 4 + 4 <= a_pixel.size())
							std::memcpy(&captured, a_pixel.data() + at + c * 4, 4);
						if (std::memcmp(&computed, &captured, 4) != 0 && differences.size() < 400)
							differences += fmt::format(" c{}.{} {} vs {}", at / 16, "xyzw"[c], computed, captured);
					}
				}
				for (std::uint32_t word = 0; word < kFrameBytes / 4; ++word) {
					float computed = 0.0f, captured = 0.0f;
					std::memcpy(&computed, native.frame.data() + word * 4, 4);
					if (word * 4 + 4 <= a_frame.size())
						std::memcpy(&captured, a_frame.data() + word * 4, 4);
					if (std::memcmp(&computed, &captured, 4) != 0 && differences.size() < 600)
						differences += fmt::format(" frame c{}.{} {} vs {}", word / 4, "xyzw"[word % 4], computed, captured);
				}
				if (!differences.empty()) {
					++native.differed;
					Differences().Report(kEffects[a_effect].effectName, fmt::format("computed vs captured:{}", differences));
				}
			}
		}

		/** @brief Writes each pending draw's constants location into the latch and opens its gate entry. */
		void CommitBatch(const std::vector<Pending>& a_batch, org::LatchBlock& a_latch, RunGate& a_gate)
		{
			auto& state = IS();
			const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
			// Batches of the same run share one list. A new run of the index drops the previous one's.
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
				if (pending.record) {
					a_latch.WriteValue(run, pending.entry * kLatchStride, *pending.record);
				} else {
					a_latch.WriteValue(run, pending.entry * kLatchStride, pending.at);
					if (pending.sources)
						a_latch.WriteValue(run, pending.entry * kLatchStride + static_cast<std::uint32_t>(sizeof(ConstantsAt)), *pending.sources);
				}
				if (!pending.open)
					continue;
				if (pending.compute)
					a_gate.OpenDispatch(pending.entry, pending.groups[0], pending.groups[1], pending.groups[2]);
				else
					a_gate.OpenDraw(pending.entry, pending.vertices);
			}
		}


		bool Usable()
		{
			const auto& state = IS();
			return state.on && !state.flushing && FrameAnchors::IsRenderOwner() && RenderGraphRuntime::Get().IsActive();
		}

		/** @brief Taps the anchor for a_count withheld step draws. A tap that did not draw them is an error. */
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

		/** @brief Once a frame, on the render owner's first ImageSpace call. */
		void BeginFrame()
		{
			auto& state = IS();
			const auto frame = globals::state ? globals::state->frameCount : 0u;
			if (frame == state.frame)
				return;
			Flush();
			state.frame = frame;
			if (NativeSteps::s_mode != NativeSteps::Mode::Off && frame % 1000 == 0) {
				const auto& native = NativeSteps::Get();
				logger::info("[ORG] ImageSpace native: {} steps staged natively, {} compared ({} differed), {} declined", native.taken, native.compared, native.differed,
					native.declined);
			}
			// Last frame's post segments were submitted at their end taps, so their constant chunks are no longer read.
			state.postKeep.clear();
		}

		/** @brief Values within 1/64 of a pixel snap to it, as ReadViewport snaps the engine's. */
		float SnapToPixel(float a_value)
		{
			const float rounded = std::round(a_value);
			return std::abs(a_value - rounded) <= 1.0f / 64.0f ? rounded : a_value;
		}

		/** @brief Snapped as ReadViewport snaps the engine's. */
		std::array<std::uint32_t, 6> OriginViewport(float a_width, float a_height)
		{
			return ViewportBits({ .width = SnapToPixel(a_width), .height = SnapToPixel(a_height), .minDepth = 0.0f, .maxDepth = 1.0f });
		}

		/**
		* @brief The latch viewport of a render-size step drawn now: the engine's dynamic-resolution ratios of the output's area,
		* snapped as OriginViewport snaps. The ratios apply whatever the lock, as the engine's RenderWorld effects take them.
		*/
		std::array<float, 4> RenderViewportOf(const Spec& a_spec)
		{
			const auto mip = MipOfName(a_spec.key.output);
			const auto shift = mip == AnchorPasses::kAllMips ? 0u : mip;
			const float width = static_cast<float>((std::max)(a_spec.width >> shift, 1u));
			const float height = static_cast<float>((std::max)(a_spec.height >> shift, 1u));
			const auto* graphics = globals::game::graphicsState;
			if (!graphics)
				return { 1.0f, 1.0f, 0.0f, 0.0f };
			const auto& data = graphics->GetRuntimeData();
			const float x = std::clamp(SnapToPixel(width * data.dynamicResolutionWidthRatio), 1.0f, width);
			const float y = std::clamp(SnapToPixel(height * data.dynamicResolutionHeightRatio), 1.0f, height);
			return { x / width, y / height, 0.0f, 0.0f };
		}

		/** @brief The CS stages' ImageSpace clients. */
		enum class PostPart : std::uint8_t
		{
			Composites,
			Hdr,
			Final,
			WorldScopes,  // reflections, SAO, the snow pass or its copy
			WorldWater,   // water blend, after the water draws
			Start,        // displacement simulation and flow copies
			Menu,         // menu scene composites
			Frozen,
			FaceTint,     // start stage in a world frame, UI stage at a loading screen
			Count
		};
		constexpr std::array<const char*, static_cast<std::size_t>(PostPart::Count)> kPostIds{ "cs.engine.imagespace.post.composites", "cs.engine.imagespace.post.hdr",
			"cs.engine.imagespace.post.final", "cs.engine.imagespace.world.scopes", "cs.engine.imagespace.world.water", "cs.engine.imagespace.start",
			"cs.engine.imagespace.menu", "cs.engine.imagespace.frozen", "cs.engine.imagespace.facetint" };
		static_assert(sizeof(IS().post) / sizeof(PostClient) == static_cast<std::size_t>(PostPart::Count), "a post client per part");

#include "RenderGraph/Engine/ImageSpace/WorldSteps.inl"

#include "RenderGraph/Engine/ImageSpace/ChainSteps.inl"
#include "RenderGraph/Engine/ImageSpace/StartSteps.inl"
#include "RenderGraph/Engine/ImageSpace/MenuSteps.inl"

		/** @brief The engine blend states the steps use. The engine fills its blend table after Setup. */
		constexpr std::array<std::uint32_t, 5> kShapeBlends{ PackBlend(0, 0, 2, 0), PackBlend(5, 0, 2, 0), PackBlend(1, 0, 2, 0), PackBlend(0, 0, 1, 0),
			PackBlend(0, 0, 5, 0) };

		/** @brief The water's part of the shape, which the water update sees: the displacement pair, the flow map and the heightmap bound. */
		void WaterShapeNow(Frame::Vars::ImageSpaceShape& a_shape)
		{
			a_shape.displacementSize = 0;
			if (auto* displacement = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kWATER_DISPLACEMENT].texture) {
				D3D11_TEXTURE2D_DESC desc{};
				displacement->GetDesc(&desc);
				a_shape.displacementSize = desc.Width;
			}
			a_shape.flowCopySize = 0;
			if (auto* flow = FlowCopyTexture()) {
				D3D11_TEXTURE2D_DESC desc{};
				flow->GetDesc(&desc);
				a_shape.flowCopySize = desc.Width;
			}
			a_shape.flowMap = reinterpret_cast<std::uint64_t>(FlowMapTexture());
			a_shape.heightmapSteps = HeightmapStepBound();
		}

		/** @brief The engine's ImageSpace shape now. Render owner. */
		Frame::Vars::ImageSpaceShape EngineShapeNow()
		{
			Frame::Vars::ImageSpaceShape shape;
			if (!RE::ImageSpaceManager::GetSingleton())
				return shape;
			shape.luminanceLevels = Frame::PostChain::LuminanceLevels();
			shape.saoLevels = EngineSaoLevels();
			WaterShapeNow(shape);
			// The manager makes some effects after the first lookups, and each one it makes can take its pass.
			for (std::size_t i = 0; i < kEffects.size(); ++i)
				shape.effects += ShaderOf(i) ? 1u : 0u;
			// A pass needs its blend state when its build makes it.
			for (const auto blend : kShapeBlends) {
				D3D11_BLEND_DESC desc{};
				shape.states += BlendDescOf(blend, desc) ? 1u : 0u;
			}
			return shape;
		}

		/**
		* @brief Render owner. Writes the engine's ImageSpace shape as the EngineImageSpace variable, at an event: Setup, each Begin until
		* the engine has made its manager, the first sight of an effect object (EffectOf), and a hook that sees its part of the shape
		* change (the HDR effect, RenderSSAO, the water update). A change registers the step clients again at the next revision.
		*/
		void RaiseShape(std::string_view a_writer)
		{
			const auto shape = EngineShapeNow();
			if (!Frame::Vars::Set(Frame::Vars::EngineImageSpace(), shape, a_writer))
				return;
			logger::info("[ORG] ImageSpace: the engine's shape changed ({}): luminance {}, SAO {}, displacement {}, flow copies {}, effects {}, states {}, "
						 "heightmap steps {}",
				a_writer, shape.luminanceLevels, shape.saoLevels, shape.displacementSize, shape.flowCopySize, shape.effects, shape.states, shape.heightmapSteps);
			// The composites' programs build from the first shape with their effects, not from their first revision.
			WantMenuPrograms(shape);
		}

		/** @brief The shape the hooks last raised. */
		Frame::Vars::ImageSpaceShape RaisedShape()
		{
			return Frame::Vars::Written(Frame::Vars::EngineImageSpace());
		}

		/** @brief Setup. The kept textures and every step client, declared on their variables. A missing engine target is fatal. */
		void DeclareSteps()
		{
			DeclareWorldSteps();
			DeclareChainSteps();
			DeclareStartSteps();
			DeclareMenuSteps();
		}

		/** @brief An ImageSpace draw with no CS pass in native mode. Fatal in development builds, logged once in release. Draws nothing. */
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

		/** @brief True when a CS step takes the draw and the caller should skip the engine's. */
		bool TakeDraw()
		{
			auto& state = IS();
			auto& scope = *state.current;
			scope.drew = true;
			const auto& effect = kEffects[scope.effect];
			++state.drawIndex;
			static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
			if (effect.rastersNothing) {
				// Apply the draw's state to D3D11 after the pending batch. It may include a clear at the bind.
				Flush();
				setDirtyStates(false);
				++state.taken;
				return true;
			}
			if (LensFlareStepNow(scope.effect))
				return TakeLensFlareStep(scope.effect);
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

		/** @brief True when a CS step takes the dispatch and the caller should skip the engine's. */
		bool TakeDispatch(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
		{
			auto& state = IS();
			auto& scope = *state.current;
			scope.drew = true;
			++state.drawIndex;
			if (a_x == 0 || a_y == 0 || a_z == 0)
				return true;  // the engine's call would do nothing either
			if (SaoCsStepNow())
				return TakeSaoCsStep(scope.effect, { a_x, a_y, a_z });
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

		std::string_view Ineligible(const void* a_geometry, bool a_geometryEffect)
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active";
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager)
				return "no ImageSpaceManager";
			// When +0x58 is set, RenderEffect draws the dynamic-resolution quad at +0x50 instead of +0x48 (SE 0x141297b40, AE 0x141481ff0).
			// VR's layout differs.
			if (a_geometryEffect)
				return {};
			// The quad itself is refused, not the manager's flag. The upsample draws the full quad while the flag is set and is taken.
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
		 * @brief This Render sets neither target nor viewport and applies no manager rectangle. Null geometry means the +0x48 quad. An
		 * effect that draws its own geometry (the underwater mask over each water shape) takes any.
		 */
		std::string_view IneligibleExplicit(const void* a_geometry, bool a_geometryEffect)
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active";
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager)
				return "no ImageSpaceManager";
			if (a_geometryEffect)
				return {};
			const auto* quad = *reinterpret_cast<void* const*>(reinterpret_cast<const std::byte*>(manager) + 0x48);
			if (a_geometry && a_geometry != quad)
				return "a geometry other than the full-screen quad";
			return {};
		}

		std::string_view IneligibleDispatch()
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active";
			return {};
		}

		/** @brief Passes are ordered by their place in the scope, and the batch runs at its end at the latest. Nested scopes join the outer one. */
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
			// A lens flare scope leaves its batch for the next flush, so the frame's sprites run in one pass. The composites' scope,
			// next in RenderWorldPostProcess, runs it. So does any engine draw or dispatch CS does not take.
			// The compute SAO's scope leaves its batch the same way. It runs before RenderWorldPostProcess, where the sun stage may
			// still be open, and its steps belong to the post stage.
			const auto open = state.scope == static_cast<std::uint32_t>(ScopeKind::LensFlare) || state.scope == static_cast<std::uint32_t>(ScopeKind::SaoCs);
			if (--state.scopeDepth == 0 && FrameAnchors::IsRenderOwner() && !open)
				Flush();
		}

		/** @brief The draw is taken when the effect is covered and a_ineligible gives no reason. */
		template <class Call>
		void WithEffect(const RE::BSImagespaceShader* a_shader, bool a_compute, std::string_view a_ineligible, Call&& a_call)
		{
			auto& state = IS();
			const auto index = EffectOf(a_shader);
			EffectScope scope{ .shader = a_shader, .effect = index >= 0 ? static_cast<std::size_t>(index) : 0, .compute = a_compute };
			if (index >= 0 && kEffects[scope.effect].compute == a_compute) {
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
		* @brief Runs a_call in the enclosing scope, or in one of its own outside every scope.
		* False when no scope could be entered, and a_call did not run.
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
			const bool lensFlare = index >= 0 && (id == Effects::ISLensFlare || id == Effects::ISLensFlareVisibility);
			EnterScope(flowCopy ? ScopeKind::FlowMap : lensFlare ? ScopeKind::LensFlare : ScopeKind::Effect);
			if (state.scopeDepth == 0)
				return false;
			const FlowCopyGuard flow(flowCopy);
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

		template <class Call>
		void EngineCall(const RE::BSImagespaceShader* a_shader, Call&& a_call)
		{
			auto& state = IS();
			state.callShaders.push_back(a_shader);
			a_call();
			state.callShaders.pop_back();
		}

		/**
		* @brief Whether the current effect's draw can be staged without the engine's (NativeSteps): a covered, eligible quad draw of a world,
		* start or post chain step whose step is found from engine globals alone. Steps that read the engine's bound state to find their
		* pass or inputs (lens flares, image-based flares, geometry and loaded-texture steps, menus) stay the capture's.
		*/
		const char* NativeStepNow()
		{
			const auto& state = IS();
			if (NativeSteps::s_mode == NativeSteps::Mode::Off)
				return "off";
			if (!state.current || !state.current->covered)
				return "an effect CS does not cover";
			if (!state.current->eligible)
				return "a draw the capture refuses";
			const auto& info = kEffects[state.current->effect];
			// The lens flare sprites and the wading ripples are geometry steps the native path makes whole (BuildSprite, BuildShape).
			const bool sprite = info.id == Effects::ISLensFlare;
			const bool ripple = info.id == Effects::ISWaterDisplacementWadingRipple;
			// The tint masks (ISAlphaBlend) are loaded textures the native path reads from the effect's texture list.
			const bool masks = info.id == Effects::ISAlphaBlend;
			if (!sprite && !ripple && !masks && (info.geometry || info.rastersNothing || info.latched || info.vertexStage))
				return "a geometry, loaded-texture or no-raster step";
			using E = Effects;
			switch (info.id) {
			case E::ISDownsampleCS:
				return "an image-based flare dispatch";
			default:
				break;
			}
			// TakeDraw's order: a lens flare first, then the start, world and post chain steps. A menu step is what is left.
			// TakeDispatch's order: the compute SAO first. Its views and the flow copies' are set in the shadow state by their callers,
			// before the call, so a native dispatch still finds them there.
			if (state.current->compute && SaoCsStepNow())
				return nullptr;
			if (!state.current->compute && LensFlareStepNow(state.current->effect))
				return nullptr;
			if (sprite)
				return "a lens flare outside the post chain";
			if (StartStepScope() != WorldScope::None || WorldStepScope() != WorldScope::None || ChainStepNow() || MenuStepNow(state.current->effect))
				return nullptr;
			return "outside every step";
		}

		/**
		* @brief The shadow state base Render (AE 0x1414f88b0) leaves behind, without its draw: the effect's output (its first texture,
		* ImageSpaceEffect +0x48 -> ImageSpaceTexture +0x8) as render target 0 (77456), the other targets and the depth target unbound.
		* Later engine and CS code reads the current target (the Scaleform displays, the end-of-frame copies), so a native step keeps it.
		*/
		void KeepRenderTargets(const RE::BSImagespaceShader* a_shader)
		{
			const auto* effect = static_cast<const RE::ImageSpaceEffect*>(a_shader);
			const auto* const* textures = *reinterpret_cast<RE::ImageSpaceTexture* const* const*>(reinterpret_cast<const std::byte*>(effect) + 0x48);
			if (!textures || !textures[0])
				return;
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto output = textures[0]->renderTarget;
			bool changed = shadow.renderTargets[0] != output || shadow.setRenderTargetMode[0] != RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
			shadow.renderTargets[0] = output;
			shadow.setRenderTargetMode[0] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
			for (std::uint32_t i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
				changed |= shadow.renderTargets[i] != RE::RENDER_TARGET::kNONE || shadow.setRenderTargetMode[i] != RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
				shadow.renderTargets[i] = RE::RENDER_TARGET::kNONE;
				shadow.setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
			}
			if (shadow.depthStencil != static_cast<std::uint32_t>(-1)) {
				shadow.depthStencil = static_cast<std::uint32_t>(-1);
				changed = true;
			}
			if (changed) {
				shadow.cubeMapRenderTarget = static_cast<std::uint32_t>(-1);
				shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
			}
		}

		/**
		* @brief The engine's Render for the current effect, or its native step. Draw mode stages the step from the param's rows and skips
		* the engine's call; observe mode computes the rows, lets the capture take the draw and compares them.
		*/
		void RenderOrNative(const RE::BSImagespaceShader* a_shader, const void* a_param, const void* a_geometry, const std::function<void()>& a_render,
			bool a_explicit = false)
		{
			auto& native = NativeSteps::Get();
			native.sprite = {};
			if (const char* why = NativeStepNow()) {
				if (NativeSteps::s_mode != NativeSteps::Mode::Off && IS().current) {
					static Util::ReasonLog s_capture("[ORG] ImageSpace native: a draw is captured");
					s_capture.Report(fmt::format("{}: {}", kEffects[IS().current->effect].effectName, why));
				}
				a_render();
				return;
			}
			const auto id = kEffects[IS().current->effect].id;
			const char* why = a_explicit ? NativeSteps::BoundRows(native.rows) : NativeSteps::BuildRows(*a_shader, a_param, native.rows);
			if (!why && id == Effects::ISLensFlare)
				why = NativeSteps::BuildSprite(*a_shader, a_param, a_geometry, native.sprite);
			else if (!why && id == Effects::ISWaterDisplacementWadingRipple)
				why = NativeSteps::BuildShape(a_geometry, native.sprite);
			if (why) {
				++native.declined;
				static Util::ReasonLog s_declined("[ORG] ImageSpace native: a step stays the capture's");
				s_declined.Report(why, kEffects[IS().current->effect].effectName);
				a_render();
				return;
			}
			native.effect = IS().current->effect;
			native.shader = a_shader;
			if (NativeSteps::s_mode == NativeSteps::Mode::Observe) {
				native.observing = true;
				native.observingTextures = true;
				a_render();
				native.observing = false;
				native.observingTextures = false;
				return;
			}
			if (NativeSteps::s_mode == NativeSteps::Mode::Hybrid) {
				native.armed = true;
				a_render();
				native.armed = false;
				return;
			}
			// Base Render (AE 0x1414f88b0) runs the shader's PreRender before its draw and PostRender after; an effect's own work can
			// live there (nested steps take their own draws in the engine's order).
			auto* shader = const_cast<RE::BSImagespaceShader*>(a_shader);
			// The explicit Render (AE 0x1414f8b00) sets no target: its caller did.
			if (!a_explicit)
				KeepRenderTargets(a_shader);
			shader->PreRender();
			native.effect = IS().current->effect;
			native.armed = true;
			TakeDraw();
			native.armed = false;
			shader->PostRender();
			static std::array<bool, kEffects.size()> s_logged{};
			if (!std::exchange(s_logged[native.effect], true)) {
				const auto* param = static_cast<const std::byte*>(a_param);
				const bool clear = param && *reinterpret_cast<const bool*>(param + 0xC);
				const float* colour = param ? reinterpret_cast<const float*>(param + 0x10) : nullptr;
				logger::info("[ORG] ImageSpace native: first {} staged natively (clear {}{}, keeps viewport {}, param {})", kEffects[native.effect].effectName, clear,
					clear ? fmt::format(" to {} {} {} {}", colour[0], colour[1], colour[2], colour[3]) : std::string(),
					*(reinterpret_cast<const bool*>(a_shader) + 0x90 + 0x88), a_param != nullptr);
			}
		}

		/**
		* @brief The engine's DispatchComputeShader (AE 0x1414f8be0: PreRender, the compute shader's Dispatch, PostRender), or its native
		* step: the effect wrote its CS b0 rows before the call, and Renderer::DispatchCSShader (AE 0x140e49cb0) only applies state, binds
		* that buffer and dispatches, so the step is staged from the buffer with neither.
		*/
		void DispatchOrNative(RE::BSImagespaceShader* a_shader, std::array<std::uint32_t, 3> a_groups, const std::function<void()>& a_dispatch)
		{
			auto& native = NativeSteps::Get();
			if (NativeSteps::s_mode != NativeSteps::Mode::Draw || NativeStepNow()) {
				a_dispatch();
				return;
			}
			native.effect = IS().current->effect;
			a_shader->PreRender();
			native.armed = true;
			TakeDispatch(a_groups[0], a_groups[1], a_groups[2]);
			native.armed = false;
			a_shader->PostRender();
		}

#include "RenderGraph/Engine/ImageSpace/NativePost.inl"

		/** @brief BSImagespaceShader::Render (100950 / 107732). */
		struct ShaderRender
		{
			static void thunk(void* a_effect, void* a_quad, void* a_param)
			{
				if (!a_effect || !Usable()) {
					func(a_effect, a_quad, a_param);
					return;
				}
				// The ImageSpaceEffect base sits at +0x90 of a BSImagespaceShader in SE and AE.
				const auto* effect = static_cast<const RE::ImageSpaceEffect*>(a_effect);
				const auto* shader = static_cast<const RE::BSImagespaceShader*>(effect);
				const bool covered = EffectOf(shader) >= 0;
				EngineCall(shader, [&] {
					const bool scoped = InScope(shader, [&] {
						WithEffect(shader, false, covered ? Ineligible(a_quad, kEffects[static_cast<std::size_t>(EffectOf(shader))].geometry) : std::string_view{},
							[&] { RenderOrNative(shader, a_param, a_quad, [&] { func(a_effect, a_quad, a_param); }); });
					});
					if (!scoped)
						func(a_effect, a_quad, a_param);
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The explicit-target Render (100951 / 107733). Its this is the BSImagespaceShader itself. */
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
						WithEffect(a_shader, false, covered ? IneligibleExplicit(a_geometry, kEffects[static_cast<std::size_t>(EffectOf(a_shader))].geometry) : std::string_view{},
							[&] { RenderOrNative(a_shader, nullptr, a_geometry, [&] { func(a_shader, a_geometry); }, true); });
					});
					if (!scoped)
						func(a_shader, a_geometry);
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSImagespaceShader::DispatchComputeShader (100952 / 107734). Taken at DispatchCSShader. */
		struct DispatchComputeShader
		{
			static void thunk(RE::BSImagespaceShader* a_shader, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
			{
				if (!a_shader || !Usable()) {
					func(a_shader, a_x, a_y, a_z);
					return;
				}
				EngineCall(a_shader, [&] {
					const bool scoped = InScope(a_shader, [&] {
						WithEffect(a_shader, true, IneligibleDispatch(), [&] { DispatchOrNative(a_shader, { a_x, a_y, a_z }, [&] { func(a_shader, a_x, a_y, a_z); }); });
					});
					if (!scoped)
						func(a_shader, a_x, a_y, a_z);
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceEffectHDR::Render (100465 / 107183). */
		struct HDRRender
		{
			static void thunk(void* a_effect, void* a_quad, void* a_param)
			{
				// The HDR effect runs the luminance chain the shape counts. Its change is an event.
				HdrFamily(a_effect, [&] {
					if (Usable() && Frame::PostChain::LuminanceLevels() != RaisedShape().luminanceLevels)
						RaiseShape("ImageSpace: the HDR effect's luminance chain changed");
					EnterScope(ScopeKind::HDR);
					{
						const ChainHdrGuard chain;
						const MenuHdrGuard menu;
						func(a_effect, a_quad, a_param);
						LeaveScope();
					}
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Render of the full-screen blur and get-hit effects, vtable slot 1. Their draws are the slot's inner steps. */
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

		/** @brief ImageSpaceManager::RenderPreTonemapComposites (99024 / 105675). */
		struct PreTonemapComposites
		{
			static void thunk(void* a_manager, std::uintptr_t a_source, std::uintptr_t a_destination)
			{
				CompositesFamily([&] {
					EnterScope(ScopeKind::Composites);
					{
						const ChainCompositesGuard chain;
						func(a_manager, a_source, a_destination);
						LeaveScope();
					}
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief RenderSSAO (100520 / 107245). Its this is the SAO helper at ImageSpaceManager+0x1c8. */
		struct RenderSSAO
		{
			static std::uint64_t thunk(void* a_helper, std::int32_t a_source, std::uint64_t a_destination)
			{
				// Main::RenderWorld sets this to 0 for the first call and 1 for the composite. The snow pass runs between them.
				static REL::Relocation<const bool*> composite{ REL::RelocationID(528126, 415071) };
				const bool second = *composite.get();
				if (Usable() && EngineSaoLevels() != RaisedShape().saoLevels)
					RaiseShape("ImageSpace: RenderSSAO's level count changed");
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
		* @brief The compute SAO (100526 / 107252). Main::Draw calls it after the world render, before RenderWorldPostProcess, when
		* its helper's flag is set (bSAO_CS_Enable:Display or ToggleSAOCS). Its this is the helper at ImageSpaceManager+0x1d0.
		*/
		struct RenderSaoCs
		{
			static void thunk(void* a_helper, void* a_camera)
			{
				EnterScope(ScopeKind::SaoCs);
				{
					const SaoCsGuard guard(a_helper);
					func(a_helper, a_camera);
					LeaveScope();
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The screen-space reflections pass (100428 / 107146). */
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

		/** @brief The water displacement update (31385 / 32176). Its this is the water system. */
		struct WaterDisplacement
		{
			static void thunk(void* a_system)
			{
				// The water update reads the displacement pair, the flow map and the timer the shape holds. Their change is an event.
				if (Usable()) {
					const auto raised = RaisedShape();
					auto water = raised;
					WaterShapeNow(water);
					if (!(water == raised))
						RaiseShape("ImageSpace: the water update's textures or timer changed");
				}
				EnterScope(ScopeKind::Water);
				{
					const WaterUpdateGuard water;
					func(a_system);
					LeaveScope();
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceManager::RenderEffect (99025 / 105676). */
		struct RenderEffect
		{
			static void thunk(void* a_manager, std::uint32_t a_effect, std::uint64_t a_source, std::uint64_t a_destination, std::uint64_t a_param)
			{
				const MenuEffectCall call(a_effect, static_cast<std::int32_t>(a_source), static_cast<std::int32_t>(a_destination));
				func(a_manager, a_effect, a_source, a_destination, a_param);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief DrawTriShape (75477 / 77263). Any draw other than the current effect's runs the batch first. */
		struct DrawTriShape
		{
			static void thunk(void* a_renderer, void* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
			{
				auto& state = IS();
				if (state.on && !state.flushing && FrameAnchors::IsRenderOwner()) {
					// Only the manager's quad is an effect's draw. Other shapes in the scope go to the engine and must not reach TakeDraw.
					// DrawTriShape gets the renderer data, not the geometry, so compare against the quads' renderer data.
					const auto* manager = RE::ImageSpaceManager::GetSingleton();
					const auto* base = reinterpret_cast<const std::byte*>(manager);
					const auto rendererDataOf = [&](std::size_t a_offset) -> const void* {
						auto* geometry = manager ? *reinterpret_cast<RE::BSGeometry* const*>(base + a_offset) : nullptr;
						return geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
					};
					const bool quad = a_shape && (a_shape == rendererDataOf(0x48) || a_shape == rendererDataOf(0x50));
					const bool geometry = a_shape && state.current && kEffects[state.current->effect].geometry;
					state.drawShape = a_shape;
					state.drawStart = a_start;
					state.drawTriangles = a_triangles;
					// The underwater mask draws several shapes in one call. One whose pass is already batched starts a new run.
					if ((quad || geometry) && state.scopeDepth > 0 && state.current && state.current->eligible && !state.current->compute &&
						(!state.current->drew || geometry)) {
						// TakeDraw applies state before DrawTriShape would, so set this shape's vertex description first.
						// Otherwise the input layout uses the previous draw's. VertexDesc is at +0x10 of BSGraphics::TriShape.
						const auto shapeDesc = *reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(a_shape) + 0x10);
						auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
						if (shadow.vertexDesc != shapeDesc) {
							shadow.vertexDesc = shapeDesc;
							shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
						}
						if (TakeDraw())
							return;
					} else if ((quad || geometry) && state.current && !state.current->compute) {
						NoPassDraw(state.current->shader, false, state.current->covered ? state.lastRefusal.empty() ? "not taken" : state.lastRefusal.c_str() : "an effect CS does not cover");
						return;
					} else
						Flush();
				}
				// What CS did not take is the engine's own draw.
				func(a_renderer, a_shape, a_start, a_triangles);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		* @brief Decision 4 of native-post-chain.md: a family whose container or effect vtable slot holds another mod's hook stays on the
		* engine path, with one log line naming it. Checked once, at the first frame with the ImageSpace manager. Render owner.
		*/
		void CheckForeignHooks()
		{
			using namespace NativePost;
			auto& config = Settings();
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			const auto effect = [&](std::uint32_t a_index) -> const void* {
				return manager && a_index < manager->effects.free_idx() ? manager->effects[static_cast<std::uint16_t>(a_index)] : nullptr;
			};
			const auto check = [&](std::uint8_t a_family, std::string a_why) {
				if (a_why.empty())
					return;
				config.foreign |= a_family;
				logger::warn("[ORG] native post chain: {}; that family stays on the engine path", a_why);
			};
			check(Family::kHdr, ForeignAt("ImageSpaceEffectHDR::Render (AE 107183)", REL::RelocationID(100465, 107183).address(), HDRRender::func.address()));
			check(Family::kHdr, ForeignSlot("the HDR effect", effect(0x1e), 7, REL::RelocationID(100467, 107185).address()));
			for (std::uint32_t r = 0; r < 7; ++r)
				check(Family::kHdr, ForeignSlot(fmt::format("the bloom blur effect {}", r), effect(0x17 + r), 7, REL::RelocationID(101560, 108558).address()));
			check(Family::kComposites, ForeignAt("RenderPreTonemapComposites (AE 105675)", REL::RelocationID(99024, 105675).address(), PreTonemapComposites::func.address()));
			check(Family::kEof, ForeignAt("RenderEndOfFrameEffects (AE 105674)", REL::RelocationID(99023, 105674).address(), 0));
			check(Family::kPost, ForeignAt("RenderWorldPostProcess (AE 107148)", REL::RelocationID(100430, 107148).address(), 0));
			// The tonemap call site inside RenderEndOfFrameEffects is CS's: the native end-of-frame effects run its work themselves.
			// SE has two: the HDR form (+0x1EA) and the copy (+0x230).
			for (const auto offset : REL::Module::IsAE() ? std::vector<std::uintptr_t>{ 0x178 } : std::vector<std::uintptr_t>{ 0x1EA, 0x230 }) {
				const auto site = REL::RelocationID(99023, 105674).address() + offset;
				const auto target = FollowJumps(site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1));
				if (!InModule(target, OwnModule()))
					check(Family::kEof, fmt::format("the HDR effect's call site in RenderEndOfFrameEffects leads to {:#x}, outside Community Shaders", target));
			}
			// The slot effects CS stages natively: Render (slot 1) and UpdateParams (slot 7) must be the game's or CS's (Legacy Graphics
			// Compatibility wraps the depth of field's Render).
			const auto foreignSlot = [&](std::uint32_t a_slot, std::string a_why) {
				config.foreignSlots |= static_cast<std::uint8_t>(1u << a_slot);
				logger::warn("[ORG] native post chain: {}; the engine's RenderEffect draws end-of-frame slot {}", a_why, a_slot);
			};
			// The full-screen blur's and get hit's Render hold CS's slot guard, which calls the code it replaced.
			for (const auto& [slot, replaced] : { std::pair{ 6u, SlotEffectRender<6>::func.address() }, std::pair{ 7u, SlotEffectRender<7>::func.address() } })
				if (const auto target = FollowJumps(replaced); replaced && !InModule(target, GetModuleHandleW(nullptr)) && !InModule(target, OwnModule()))
					foreignSlot(slot, fmt::format("its Render under CS's guard leads to {:#x}, another module", target));
			// Their blurs (effects 9 to 15) must keep the engine's UpdateParams.
			for (std::uint32_t r = 0; r < 7; ++r)
				if (const auto why = ForeignSlot(fmt::format("the slot blur effect {}", r), effect(9 + r), 7, REL::RelocationID(101560, 108558).address()); !why.empty()) {
					foreignSlot(6, why);
					foreignSlot(7, why);
				}
			for (const std::uint32_t slot : { 1u, 3u, 5u, 6u, 7u }) {
				const auto* object = effect(slot);
				if (!object)
					continue;
				const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(object);
				for (const std::uint32_t entry : { 1u, 7u }) {
					const auto target = FollowJumps(vtable[entry]);
					if (InModule(target, GetModuleHandleW(nullptr)) || InModule(target, OwnModule()))
						continue;
					config.foreignSlots |= static_cast<std::uint8_t>(1u << slot);
					logger::warn("[ORG] native post chain: end-of-frame slot {}'s vtable entry {} leads to {:#x}, another module; the engine's RenderEffect draws it", slot,
						entry, target);
				}
			}
			// A family inside a foreign one cannot be native while the outer one stages it.
			if (config.foreign & Family::kHdr)
				config.foreign |= Family::kEof | Family::kPost;
			if (config.foreign & (Family::kEof | Family::kComposites))
				config.foreign |= Family::kPost;
			logger::info("[ORG] native post chain: foreign hook check done (families on the engine path for a foreign hook: {:#x})", config.foreign);
		}

		void NativePost::CheckForeignHooksOnce()
		{
			static const bool s_checked = [] {
				CheckForeignHooks();
				return true;
			}();
			(void)s_checked;
		}

		/** @brief DispatchCSShader (75532 / 77329). Any other engine dispatch runs the batch first. */
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
				}
				// What CS did not take is the engine's own dispatch.
				func(a_renderer, a_shader, a_x, a_y, a_z);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (!Engine::GraphMode())
			return;
		{
			const auto value = Switches::Value("CS_ORG_IMAGESPACE_NATIVE");
			NativeSteps::s_mode = value == "0" ? NativeSteps::Mode::Off :
			                      value == "1" ? NativeSteps::Mode::Observe :
			                      value == "3" ? NativeSteps::Mode::Hybrid :
			                                     NativeSteps::Mode::Draw;
			logger::info("[ORG] ImageSpace native: CS_ORG_IMAGESPACE_NATIVE={} ({})", value.empty() ? std::string("2, the default") : value,
				NativeSteps::s_mode == NativeSteps::Mode::Off     ? "every step captured" :
				NativeSteps::s_mode == NativeSteps::Mode::Observe ? "param steps computed and compared with the capture" :
				                                                    "param steps staged from the engine's params, without its draw");
		}
			{
			// The native post chain (native-post-chain.md): 0 the engine's containers, 1 observe, 2 native; optionally ":" and a family
			// list (hdr, eof, composites, post), all four when none. On SE only when the switch is set (its ids were never run).
			const auto value = Switches::Value("CS_ORG_POST_NATIVE");
			const auto text = value.empty() ? std::string("2") : std::string(value);
			auto& config = NativePost::Settings();
			const auto mode = text.substr(0, text.find(':'));
			config.mode = mode == "1" ? NativePost::Mode::Observe : mode == "2" ? NativePost::Mode::Native : NativePost::Mode::Engine;
			config.families = NativePost::Family::kAll;
			if (const auto colon = text.find(':'); colon != std::string::npos) {
				config.families = 0;
				const auto list = text.substr(colon + 1);
				config.families |= list.find("hdr") != std::string::npos ? NativePost::Family::kHdr : 0;
				config.families |= list.find("eof") != std::string::npos ? NativePost::Family::kEof : 0;
				config.families |= list.find("composites") != std::string::npos ? NativePost::Family::kComposites : 0;
				config.families |= list.find("post") != std::string::npos ? NativePost::Family::kPost : 0;
			}
			// The end-of-frame and body families stage the families inside them natively.
			if (config.mode == NativePost::Mode::Native && (config.families & NativePost::Family::kPost))
				config.families |= NativePost::Family::kComposites | NativePost::Family::kEof;
			if (config.mode == NativePost::Mode::Native && (config.families & NativePost::Family::kEof))
				config.families |= NativePost::Family::kHdr;
			if (!REL::Module::IsAE() && value.empty()) {
				logger::info("[ORG] native post chain: SE runs every family on the engine unless CS_ORG_POST_NATIVE is set");
				config.mode = NativePost::Mode::Engine;
			}
			NativePost::ResolveGlobals();
			logger::info("[ORG] native post chain: CS_ORG_POST_NATIVE={} (mode {}, families {:#x})", text, static_cast<int>(config.mode), config.families);
		}
		stl::detour_thunk<ShaderRender>(REL::RelocationID(100950, 107732));
		stl::detour_thunk<HDRRender>(REL::RelocationID(100465, 107183));
		stl::detour_thunk<PreTonemapComposites>(REL::RelocationID(99024, 105675));
		stl::detour_thunk<ExplicitRender>(REL::RelocationID(100951, 107733));
		stl::detour_thunk<DispatchComputeShader>(REL::RelocationID(100952, 107734));
		stl::detour_thunk<RenderSSAO>(REL::RelocationID(100520, 107245));
		stl::detour_thunk<RenderReflections>(REL::RelocationID(100428, 107146));
		stl::detour_thunk<RenderSaoCs>(REL::RelocationID(100526, 107252));
		stl::detour_thunk<WaterDisplacement>(REL::RelocationID(31385, 32176));
		stl::detour_thunk<DrawTriShape>(REL::RelocationID(75477, 77263));
		stl::detour_thunk<DispatchCSShader>(REL::RelocationID(75532, 77329));
		stl::detour_thunk<RenderEffect>(REL::RelocationID(99025, 105676));
		auto& state = IS();
		state.installed = ShaderRender::func.address() && HDRRender::func.address() && PreTonemapComposites::func.address() && ExplicitRender::func.address() &&
		                  DispatchComputeShader::func.address() && RenderSSAO::func.address() && RenderReflections::func.address() && RenderSaoCs::func.address() && WaterDisplacement::func.address() &&
		                  DrawTriShape::func.address() && DispatchCSShader::func.address() && RenderEffect::func.address();
		if (!state.installed)
			RenderGraph::Fatal("ImageSpace", "a hook could not be installed");
		logger::info(
			"[ORG] ImageSpace: hooks installed (BSImagespaceShader::Render and its explicit-target form, DispatchComputeShader, the HDR effect, the pre-tonemap "
			"composites, RenderSSAO, the reflections pass, the compute SAO, the water's displacement update, DrawTriShape, DispatchCSShader)");
	}

	void Setup()
	{
		auto& state = IS();
		if (!Engine::GraphMode() || state.on)
			return;
		if (!state.installed)
			RenderGraph::Fatal("ImageSpace", "its hooks are not installed");
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !RenderGraphRuntime::Get().IsActive())
			RenderGraph::Fatal("ImageSpace", "the render graph is not active");
		// Without this header, older staged shaders compile their D3D11 cbuffers under DXC and every pipeline fails.
		if (std::error_code error; !std::filesystem::exists(std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / "Common/OrgEngineConstants.hlsli", error))
			RenderGraph::Fatal("ImageSpace", "Data/Shaders has no Common/OrgEngineConstants.hlsli: the staged shaders predate the ORG builds of ISHDR, ISBlur and the VL composite");
		state.heldSources.Clear();
		for (std::size_t i = 0; i < kEffects.size(); ++i) {
			state.fallbacks[i] = std::make_unique<Util::ReasonLog>(fmt::format("[ORG] ImageSpace: {} is not taken", kEffects[i].effectName));
		}
		// Hooked here, not at Install. Legacy Graphics Compatibility checks the original vfunc at load, and this wraps its patch.
		static bool slotHooks = [] {
			REL::Relocation<std::uintptr_t> fullBlur{ RE::VTABLE_ImageSpaceEffectFullScreenBlur[0] };
			REL::Relocation<std::uintptr_t> getHit{ RE::VTABLE_ImageSpaceEffectGetHit[0] };
			SlotEffectRender<6>::func = fullBlur.write_vfunc(1, SlotEffectRender<6>::thunk);
			SlotEffectRender<7>::func = getHit.write_vfunc(1, SlotEffectRender<7>::thunk);
			return true;
		}();
		(void)slotHooks;
		// Every ImageSpace draw is a CS stage step, so every stage with ImageSpace steps must run.
		const auto epochs = FrameAnchors::SegmentEpochs();
		const auto runs = [&](Frame::SegmentId a_stage) {
			return std::ranges::find(epochs, RenderGraphRuntime::EpochOf(RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(a_stage)))) != epochs.end();
		};
		if (!Frame::PostChain::On() || !runs(Frame::SegmentId::Post) || !runs(Frame::SegmentId::Deferred) || !runs(Frame::SegmentId::Start) || !runs(Frame::SegmentId::Ui))
			RenderGraph::Fatal("ImageSpace", "not every CS stage with ImageSpace steps runs in this mode");
		for (std::size_t p = 0; p < state.post.size(); ++p) {
			auto& client = state.post[p];
			client.id = kPostIds[p];
			client.gate = RunGate::Create(client.id, kGateEntries);
			client.latch = org::LatchBlock::Create(fmt::format("{}::ConstantsAt", client.id), kGateEntries * kLatchStride);
		}
		if (!std::ranges::all_of(state.post, [](const PostClient& a_client) { return a_client.gate && a_client.latch; }))
			RenderGraph::Fatal("ImageSpace", "a step client's run gate or latch could not be made");
		state.on = true;
		// Before the declarations, which read it.
		RaiseShape("ImageSpace::Setup");
		logger::info("[ORG] ImageSpace: on; every covered effect; every ImageSpace draw is a CS step's pass");
		// Make the flow map now if the engine has not, so the flow copies' pass can name it at registration.
		// Unified Water puts its own flow map in the slot at DataLoaded, which comes later. Ours would then be a second
		// texture under the published id, which is fatal. So the build publishes Unified Water's when it first sees it.
		if (EffectNamed("ISCopySubRegionCS") >= 0 && !globals::features::unifiedWater.loaded) {
			MakeFlowMap();
			PublishFlowMap();
			RaiseShape("ImageSpace::Setup, the flow map");
		}
			// The first revision registers the clients the engine is ready for, so the build is ready for the main menu.
		DeclareSteps();
	}

	void OnEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final)
	{
		auto& chain = Chain();
		if (!IS().on || !chain.group.Registered()) {
			chain.inEndOfFrame = false;
			return;
		}
		if (a_begin) {
			// Earlier draws belong to the stage's earlier steps, so their batch runs first.
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
		// After the revision's adoption, so a change is a later revision's.
		if (!Usable())
			return;
			NoteWorldBegin();
		NativePost::ReadA();
		// The engine makes its ImageSpace manager, effects and blend states after CS's setup, with no hook of ours. Until it has,
		// each Begin raises the shape. Then the hooks raise what changes.
		auto& state = IS();
		if (!state.shapeMade) {
			RaiseShape("ImageSpace at Begin, until the engine has made its ImageSpace");
			const auto shape = RaisedShape();
			state.shapeMade = shape.effects > 0 && shape.states == kShapeBlends.size();
		}
	}

	void OnMenuEndOfFrame(bool a_begin, std::int32_t a_source, std::int32_t a_final)
	{
		auto& menu = Menu();
		if (!IS().on) {
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
		if (!IS().on || !chain.group.Registered() || !chain.inEndOfFrame || !chain.inHdr)
			return false;
		a_out = {};
		a_out.input = ImportOf(ImportOfName(RE::RENDER_TARGET::kMAIN));
		a_out.output = ImportOf(ImportOfName(chain.final));
		a_out.inputFormat = RtvFormatOf(RE::RENDER_TARGET::kMAIN, a_out.input);
		a_out.outputFormat = RtvFormatOf(chain.final, a_out.output);
		a_out.chain = true;
		return a_out.input && a_out.output;
	}

		void WorldPostProcess(std::uint32_t a_target, const std::function<void()>& a_engine)
	{
		NativePost::ReadB();
		const auto& f = NativePost::R().frame;
		if (IS().on && f.readB && Chain().group.Registered() && NativePost::In(NativePost::Mode::Native, NativePost::Family::kPost)) {
			NativePost::RunBody(a_target);
			return;
		}
		a_engine();
	}

	void WorldEndOfFrame(std::int32_t a_source, std::int32_t a_final, const std::function<void()>& a_engine)
	{
		if (!IS().on) {
			a_engine();
			return;
		}
		EofFamily(a_source, a_final, a_engine);
	}

	bool SceneInMain()
	{
		const auto& chain = Chain();
		return IS().on && chain.group.Registered() && chain.inEndOfFrame;
	}
}

