// Water's program family and deferred ranges (WaterDraw.h).

#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"
#include "Features/DrawcallLimitFix/Features/Water/Scene/WaterScene.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/FrameAnchors.h"
#include "State.h"

#include <atomic>
#include <functional>

namespace DCLF::Features::Water::Indirect
{
	namespace
	{
		Api::ClassifyInputId drawnInput;
		std::atomic<bool> waterDrawer{ false };
		// The adopted Interior variable, which the drawn input may read off the render thread.
		std::atomic<bool> waterInterior{ false };

		// The engine targets the ranges draw into (RE::RENDER_TARGET values, Water.h's kMaskTargets and kSurfaceTargets).
		constexpr std::uint32_t kMainTarget = 1, kNormalMaskTarget = 4, kNormalMaskSwapTarget = 5, kMotionVectorTarget = 7, kRawWaterTarget = 88;

		/** @brief The engine's BSWaterShader, as Community Shaders' compile workers handed it over. */
		const void* WaterShaderSource() { return ShaderPrograms::FamilyShader(Api::ProgramFamily::Water); }

		/** @brief Whether a vertex layout has a vertex colour, which makes the stencil pass 6E (Port::DeriveRenderPasses' hasColors). */
		constexpr bool HasColours(std::uint64_t a_vertexLayout) { return ((static_cast<std::uint32_t>(a_vertexLayout) >> 0x16) & 0x3C) != 0; }

		/** @brief The mask's program: the Stencil technique, with Vc when the geometry has colours (Port::RawDescriptor of 6D or 6E). */
		Api::DeferredKey MaskProgramKey(const Api::DeferredKey& a_key)
		{
			auto key = a_key;
			key.vertexDescriptor = key.pixelDescriptor = Port::RawDescriptor(HasColours(a_key.vertexLayout) ? 0x5C00006Eu : 0x5C00006Du);
			return key;
		}

		bool DrawsMask(const Api::DeferredKey& a_key, std::uint8_t& a_stencilRef)
		{
			// Every water key draws its mask. Without a water-type bit the stencil is off (MaskState) and the engine's reference is 0xFF.
			const std::uint8_t bit = WaterScene::MaskStencilRefOfKey(a_key.rasterFlags);
			a_stencilRef = bit ? bit : std::uint8_t{ 0xFF };
			return true;
		}

		/** @brief Every water key draws in the surface range, so a key the surface cannot draw fails its build (SurfaceState) and stays native. */
		bool DrawsSurface(const Api::DeferredKey&, std::uint8_t& a_stencilRef)
		{
			a_stencilRef = WaterScene::kSurfaceStencilRef;
			return true;
		}

		bool MaskState(const Api::DeferredKey& a_key, Api::DeferredRangeState& a_state)
		{
			// SetupGeometry's Stencil technique: bit 1 << index takes mode 2 + index with the bit as reference. Water with no water-type
			// bit (every Unified Water tile) takes mode 0, the stencil off, and writes only the normals mask and the motion vectors.
			const std::uint32_t index = WaterScene::StencilIndexOfKey(a_key.rasterFlags);
			a_state = { .writeMode = kMaskWriteMode,
				.depthMode = kMaskDepthMode,
				.stencilMode = index ? static_cast<std::uint8_t>(kMaskStencilModeBase + index) : std::uint8_t{ 0 },
				.stencilWriteMask = index ? static_cast<std::uint8_t>(1u << index) : std::uint8_t{ 0 },
				.twoSided = true };
			return true;
		}

		/**
		 * @brief The surface's state for the plain techniques: 0, LOD and simple, without Wading. It tests bit 0 (mode 30, ref 1)
		 * whatever the water-type bit.
		 */
		bool SurfaceState(const Api::DeferredKey& a_key, Api::DeferredRangeState& a_state)
		{
			const std::uint32_t technique = Port::TechniqueOf(a_key.vertexDescriptor);
			if (technique != 0 && technique != Port::kTechniqueLod && technique != Port::kTechniqueSimple)
				return false;
			if (a_key.vertexDescriptor & Port::Flag::kWading)
				return false;
			a_state = { .writeMode = kSurfaceWriteMode, .depthMode = kSurfaceDepthMode, .stencilMode = kSurfaceStencilMode, .twoSided = true };
			return true;
		}

		/**
		 * @brief The mask writes the normals mask and the motion vectors. The normals target alternates with its swap by frame, so set 0
		 * has kNORMAL_TAAMASK_SSRMASK and set 1 its swap.
		 */
		FrameAnchors::Texture MaskTarget(std::uint32_t a_variant, std::uint32_t a_index)
		{
			if (a_index > 1)
				return nullptr;
			const std::uint32_t normals = a_variant ? kNormalMaskSwapTarget : kNormalMaskTarget;
			return FrameAnchors::EngineTexture(FrameAnchors::Colour(a_index == 0 ? normals : kMotionVectorTarget));
		}

		/**
		 * @brief This frame's normals target: GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP (100460), which the mask's SetupTechnique
		 * binds. It is the swap on every other frame while SSR is on. Render thread.
		 */
		std::uint32_t MaskVariant()
		{
			const auto world = ::RenderGraph::Frame::PostChain::ReadWorld(::RenderGraph::Frame::PostChain::Scope::None, {});
			return world.normals == kNormalMaskSwapTarget ? 1u : 0u;
		}

		/** @brief The surface writes kMAIN (set 0), or kRAW_WATER (set 1) when ISWaterBlend composites it. The two share a format. */
		FrameAnchors::Texture SurfaceTarget(std::uint32_t a_variant, std::uint32_t a_index)
		{
			return a_index == 0 ? FrameAnchors::EngineTexture(FrameAnchors::Colour(a_variant ? kRawWaterTarget : kMainTarget)) : nullptr;
		}

		/** @brief The global SetupTechnique reads (528167 / 415112): set, the surface draws into kRAW_WATER. Render thread. */
		std::uint32_t SurfaceVariant()
		{
			const auto offset = Port::Address::kRawWater.Get();
			return offset && *reinterpret_cast<const bool*>(REL::Module::get().base() + offset) ? 1u : 0u;
		}

		bool SampleDrawn()
		{
			if (!waterDrawer.load(std::memory_order_acquire))
				return false;
			// CPU-driven frames draw claimed water in the water family's anchor pass (Draws::TakeCpuRange), once its client is up.
			if (WaterScene::ClaimsInCpuFrames() && WaterDraws::Available())
				return true;
			return !Draws::CpuPipelineForced() && !Draws::GraphBuiltForCpu() && !CpuPipelineFor(Toggles::Get().Active(), waterInterior.load(std::memory_order_relaxed));
		}

		void* ColourView(RE::RENDER_TARGET a_target)
		{
			return globals::game::renderer ? static_cast<void*>(globals::game::renderer->GetRuntimeData().renderTargets[a_target].SRV) : nullptr;
		}
		/**
		 * @brief The refraction at t1: kMAIN's copy texture, which the engine fills from kMAIN just before the water draws. Not the
		 * kMAIN_COPY target, which the post chain writes at the frame's end.
		 */
		void* RefractionView()
		{
			return globals::game::renderer ? static_cast<void*>(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN].SRVCopy) : nullptr;
		}
		void* DisplacementView() { return ColourView(RE::RENDER_TARGET::kWATER_DISPLACEMENT); }
		/**
		 * @brief The flow map SetupGeometry binds at t8 for a Flowmap draw: the flow map source texture's view (AE 0x1414e2957).
		 * Unified Water makes that source its world flow map; otherwise it is the ImageSpace port's map, which the commit reads by id.
		 */
		void* FlowMapView()
		{
			const auto offset = Port::Address::kFlowmapSource.Get();
			if (!offset)
				return nullptr;
			const auto* source = reinterpret_cast<const RE::NiPointer<RE::NiSourceTexture>*>(REL::Module::get().base() + offset)->get();
			return source && source->rendererTexture ? static_cast<void*>(source->rendererTexture->resourceView) : nullptr;
		}
		void* SsrView() { return ColourView(RE::RENDER_TARGET::kSSR); }
		void* SsrRawView() { return ColourView(RE::RENDER_TARGET::kSSR_RAW); }
		/**
		 * @brief Whether CS's cubemap fix leaves t3 empty where the engine binds kREFLECTIONS: in an interior, outside the map menu
		 * (Port's nullReflectionCube).
		 */
		bool GlobalCubeUnbound()
		{
			auto* tes = RE::TES::GetSingleton();
			if (!tes || (globals::state && globals::state->isMapMenuOpen))
				return false;
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			return tes->interiorCell || (cell && cell->IsInteriorCell());
		}
		void* GlobalCubeView()
		{
			if (!globals::game::renderer || GlobalCubeUnbound())
				return nullptr;
			return static_cast<void*>(globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS].SRV);
		}
		// The published ids of the engine's targets the surface reads. The renderer's SRVs are not their exports, so each frame texture
		// names its id through graphId rather than matching the view. Only wading water reads the displacement swap, and it is not claimed.
		constexpr std::string_view kRefractionId = "Engine::kMAIN Copy", kDisplacementId = "Engine::kWATER_DISPLACEMENT",
								   kDepthId = "Engine::Depth::kPOST_ZPREPASS_COPY", kSsrId = "Engine::kSSR", kSsrRawId = "Engine::kSSR_RAW",
								   kGlobalCubeId = "Engine::Cubemap::kREFLECTIONS";
		std::string_view RefractionId() { return kRefractionId; }
		std::string_view DisplacementId() { return kDisplacementId; }
		std::string_view DepthId() { return kDepthId; }
		std::string_view SsrId() { return kSsrId; }
		std::string_view SsrRawId() { return kSsrRawId; }
		std::string_view GlobalCubeId() { return GlobalCubeUnbound() ? std::string_view{} : kGlobalCubeId; }

		void* DepthView()
		{
			return globals::game::renderer ?
			           static_cast<void*>(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
			           nullptr;
		}
	}

	void Register(Api::InstallContext& a_context)
	{
		if (!WaterScene::On())
			return;
		a_context.RegisterProgramFamily({
			.family = Api::ProgramFamily::Water,
			.shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Water),
			.name = "Water",
			.keyName = "water",
			.sourcePath = "Data/Shaders/Water.hlsl",
			.nativeNote = "water stays the latch path's",
			.stages = Api::ProgramStages::VertexPixel,
			.build = Api::ProgramBuild::DeferredRanges,
			// The material values are the water material pool's, so the rows' blocks are not read.
			.rowBlocks = false,
			.programSource = &WaterShaderSource,
		});
		a_context.RegisterDeferredRange({ .slot = Api::DeferredRangeSlot::Range0,
			.name = "water-mask",
			.family = Api::ProgramFamily::Water,
			.draws = &DrawsMask,
			.target = &MaskTarget,
			.variants = 2,
			.variant = &MaskVariant,
			.programKey = &MaskProgramKey,
			.state = &MaskState });
		a_context.RegisterDeferredRange({ .slot = Api::DeferredRangeSlot::Range1,
			.name = "water-surface",
			.family = Api::ProgramFamily::Water,
			.draws = &DrawsSurface,
			.target = &SurfaceTarget,
			.variants = 2,
			.variant = &SurfaceVariant,
			.state = &SurfaceState });
		// The engine's targets are graph textures written by CS passes, published under their declared names. Their D3D11 views hold
		// nothing, so each frame texture reads its graph resource, as the latch path reads them by published id.
		a_context.RegisterFrameTexture({ .name = "water refraction",
			.textureRegister = kRefractionRegister,
			.view = &RefractionView,
			.graphResources = { kRefractionId },
			.graphId = &RefractionId });
		a_context.RegisterFrameTexture({ .name = "water displacement",
			.textureRegister = kDisplacementRegister,
			.view = &DisplacementView,
			.graphResources = { kDisplacementId },
			.graphId = &DisplacementId });
		a_context.RegisterFrameTexture(
			{ .name = "water depth", .textureRegister = kDepthRegister, .view = &DepthView, .graphResources = { kDepthId }, .graphId = &DepthId });
		a_context.RegisterFrameTexture({ .name = "water flow map",
			.textureRegister = kFlowMapRegister,
			.view = &FlowMapView,
			.graphResources = { ::RenderGraph::Engine::ImageSpace::kFlowMapId } });
		a_context.RegisterFrameTexture({ .name = "water SSR", .textureRegister = kSsrRegister, .view = &SsrView, .graphResources = { kSsrId }, .graphId = &SsrId });
		a_context.RegisterFrameTexture(
			{ .name = "water SSR raw", .textureRegister = kSsrRawRegister, .view = &SsrRawView, .graphResources = { kSsrRawId }, .graphId = &SsrRawId });
		a_context.RegisterFrameTexture({ .name = "water global cube",
			.textureRegister = kGlobalCubeRegister,
			.view = &GlobalCubeView,
			.graphResources = { kGlobalCubeId },
			.graphId = &GlobalCubeId });
		drawnInput = a_context.RegisterClassifyInput({ .name = "water drawn by dclf", .sample = &SampleDrawn });
		::RenderGraph::Frame::Vars::OnAdopted(::RenderGraph::Frame::Vars::Interior(),
			std::function<void(const bool&, const bool&)>([](const bool&, const bool& a_now) { waterInterior.store(a_now, std::memory_order_relaxed); }));
		logger::info("[DCLF] water: the Water program family and the water-mask and water-surface deferred ranges are registered");
	}

	Api::ClassifyInputId DrawnInput()
	{
		return drawnInput;
	}

	void SetDrawer(bool a_drawer)
	{
		waterDrawer.store(a_drawer, std::memory_order_release);
	}
}
