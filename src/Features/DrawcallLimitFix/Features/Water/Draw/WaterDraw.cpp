// Water's program family and deferred ranges (WaterDraw.h).

#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"
#include "Features/DrawcallLimitFix/Features/Water/Scene/WaterScene.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/FrameAnchors.h"

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
			a_stencilRef = WaterScene::MaskStencilRefOfKey(a_key.rasterFlags);
			return a_stencilRef != 0;
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
			return !Draws::CpuPipelineForced() && !Draws::GraphBuiltForCpu() && !CpuPipelineFor(Toggles::Get().Active(), waterInterior.load(std::memory_order_relaxed));
		}

		void* ColourView(RE::RENDER_TARGET a_target)
		{
			return globals::game::renderer ? static_cast<void*>(globals::game::renderer->GetRuntimeData().renderTargets[a_target].SRV) : nullptr;
		}
		void* RefractionView() { return ColourView(RE::RENDER_TARGET::kMAIN_COPY); }
		void* DisplacementView() { return ColourView(RE::RENDER_TARGET::kWATER_DISPLACEMENT); }
		void* FlowMapView() { return ColourView(RE::RENDER_TARGET::kWATER_FLOW); }
		void* SsrView() { return ColourView(RE::RENDER_TARGET::kSSR); }
		void* SsrRawView() { return ColourView(RE::RENDER_TARGET::kSSR_RAW); }
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
		a_context.RegisterFrameTexture({ .name = "water refraction", .textureRegister = kRefractionRegister, .view = &RefractionView });
		a_context.RegisterFrameTexture({ .name = "water displacement", .textureRegister = kDisplacementRegister, .view = &DisplacementView });
		a_context.RegisterFrameTexture({ .name = "water depth", .textureRegister = kDepthRegister, .view = &DepthView });
		a_context.RegisterFrameTexture({ .name = "water flow map",
			.textureRegister = kFlowMapRegister,
			.view = &FlowMapView,
			.graphResources = { ::RenderGraph::Engine::ImageSpace::kFlowMapId } });
		a_context.RegisterFrameTexture({ .name = "water SSR", .textureRegister = kSsrRegister, .view = &SsrView });
		a_context.RegisterFrameTexture({ .name = "water SSR raw", .textureRegister = kSsrRawRegister, .view = &SsrRawView });
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
