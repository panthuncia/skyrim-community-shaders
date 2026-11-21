#include "Globals.h"
#include "GpuEventTimers.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"

#include "CSEditor/SceneManager/SceneManager.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "Deferred.h"
#include "Features/CSEditor.h"
#include "Features/CloudShadows.h"
#include "Features/DeveloperTools.h"
#include "Features/GraphicsAPI.h"
#include "Features/DynamicCubemaps.h"
#include "Features/Effects11.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/GrassCollision.h"
#include "Features/GrassLighting.h"
#include "Features/GrassOptimizations.h"
#include "Features/HDRDisplay.h"
#include "Features/HairSpecular.h"
#include "Features/HorizonFix.h"
#include "Features/IBL.h"
#include "Features/InteriorSun.h"
#include "Features/LODBlending.h"
#include "Features/DrawcallLimitFix.h"
#include "Utils/SwitchFile.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PerformanceOverlay.h"
#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/ProceduralAurora.h"
#include "Features/ProceduralSun.h"
#include "Features/RainRefraction.h"
#include "Features/RemoteControl.h"
#include "Features/RenderDoc.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Skin.h"
#include "Features/SkySync.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainHelper.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/TreeWind.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/ViewAtmosphere.h"
#include "Features/VolumetricLighting.h"
#include "Features/VolumetricShadows.h"
#include "Features/TextureStreaming.h"
#include "Features/VRAMBudget.h"
#include "Features/WaterEffects.h"
#include "Features/WetnessEffects.h"
#include "HiZPyramid.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/Game.h"
#include "TextureStreaming/Census.h"
// Last: DXVK's interop header includes vulkan.h only when no Vulkan header is in, so volk users come first.
#include "DxvkExports.h"

#include <array>
#include <atomic>
#include <intrin.h>
#include <mutex>

namespace globals
{
	namespace d3d
	{
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		IDXGISwapChain* swapChain = nullptr;
	}

	namespace features
	{
		CloudShadows cloudShadows{};
		DynamicCubemaps dynamicCubemaps{};
		VolumetricShadows volumetricShadows{};
		ExtendedMaterials extendedMaterials{};
		GrassCollision grassCollision{};
		GrassLighting grassLighting{};
		DrawcallLimitFix drawcallLimitFix{};
		GrassOptimizations grassOptimizations{};
		IBL ibl{};
		LightLimitFix lightLimitFix{};
		LinearLighting linearLighting{};
		LODBlending lodBlending{};
		HairSpecular hairSpecular{};
		HorizonFix horizonFix{};
		InteriorSun interiorSun{};
		PhysicalSky physicalSky{};
		ProceduralSun proceduralSun{};
		RainRefraction rainRefraction{};
		ScreenSpaceGI screenSpaceGI{};
		ScreenSpaceShadows screenSpaceShadows{};
		Skylighting skylighting{};
		TerrainVariation terrainVariation{};
		SkySync skySync{};
		SubsurfaceScattering subsurfaceScattering{};
		TerrainBlending terrainBlending{};
		TerrainHelper terrainHelper{};
		TerrainShadows terrainShadows{};
		TreeWind treeWind{};
		UnifiedWater unifiedWater{};
		VolumetricLighting volumetricLighting{};
		ViewAtmosphere viewAtmosphere{};
		VRAMBudget vramBudget{};
		TextureStreamingFeature textureStreaming{};
		WaterEffects waterEffects{};
		PerformanceOverlay performanceOverlay{};
		WetnessEffects wetnessEffects{};
		ExtendedTranslucency extendedTranslucency{};
		Upscaling upscaling{};
		HDRDisplay hdrDisplay{};
		Effects11 effects11{};
		DeveloperTools developerTools{};
		GraphicsAPI graphicsAPI{};
		RenderDoc renderDoc{};
		RemoteControl remoteControl{};
		ScreenshotFeature screenshotFeature{};
		CSEditor csEditor{};
		ExponentialHeightFog exponentialHeightFog{};
		TruePBR truePBR{};
		Skin skin{};
		SceneManager sceneManager{};
		PostProcessing postProcessing{};
		ProceduralAurora proceduralAurora{};

		namespace llf
		{
		}
	}

	namespace game
	{
		RE::BSGraphics::RendererShadowState* shadowState = nullptr;
		RE::BSGraphics::State* graphicsState = nullptr;
		RE::BSGraphics::Renderer* renderer = nullptr;
		RE::BSShaderManager::State* smState = nullptr;
		RE::TES* tes = nullptr;
		RE::MemoryManager* memoryManager = nullptr;
		RE::INISettingCollection* iniSettingCollection = nullptr;
		RE::INIPrefSettingCollection* iniPrefSettingCollection = nullptr;
		RE::GameSettingCollection* gameSettingCollection = nullptr;
		float* cameraNear = nullptr;
		float* cameraFar = nullptr;
		float* deltaTime = nullptr;
		RE::BSUtilityShader* utilityShader = nullptr;
		RE::PlayerCharacter* player = nullptr;
		RE::Sky* sky = nullptr;
		RE::UI* ui = nullptr;
		RE::Calendar* calendar = nullptr;
		RE::ImageSpaceManager* imageSpaceManager = nullptr;
		bool* bEnableVolumetricLighting = nullptr;
		std::atomic<bool> quitGame{ false };

		RE::BSGraphics::PixelShader** currentPixelShader = nullptr;
		RE::BSGraphics::VertexShader** currentVertexShader = nullptr;
		REX::EnumSet<RE::BSGraphics::ShaderFlags, uint32_t>* stateUpdateFlags = nullptr;

		RE::Setting* bEnableLandFade = nullptr;
		RE::Setting* bShadowsOnGrass = nullptr;
		RE::Setting* shadowMaskQuarter = nullptr;

		REL::Relocation<ID3D11Buffer**> perFrame;
		REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> currentAccumulator;

		D3D11_MAPPED_SUBRESOURCE* mappedFrameBuffer = nullptr;
		FrameBufferCache frameBufferCached{};
	}

	static void RefreshTES()
	{
		if (auto tes = RE::TES::GetSingleton())
			game::tes = tes;
	}

	namespace rtti
	{
		REL::Relocation<const RE::NiRTTI*> NiIntegerExtraDataRTTI;
		REL::Relocation<const RE::NiRTTI*> BSLightingShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSEffectShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSWaterShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> NiParticleSystemRTTI;
		REL::Relocation<const RE::NiRTTI*> NiBillboardNodeRTTI;
		REL::Relocation<const RE::NiRTTI*> NiAlphaPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> NiSourceTextureRTTI;
		REL::Relocation<const RE::NiRTTI*> BSGrassShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSMultiStreamInstanceTriShapeRTTI;
	}

	State* state = nullptr;
	Deferred* deferred = nullptr;
	Menu* menu = nullptr;
	SIE::ShaderCache* shaderCache = nullptr;
	SceneSettingsManager* sceneSettingsManager = nullptr;

	static Profiler profilerInstance;
	Profiler* profiler = &profilerInstance;
	static HiZPyramid hiZPyramidInstance;
	HiZPyramid* hiZPyramid = &hiZPyramidInstance;

	void OnInit()
	{
		shaderCache = &SIE::ShaderCache::Instance();
		state = State::GetSingleton();
		menu = Menu::GetSingleton();
		deferred = Deferred::GetSingleton();
		sceneSettingsManager = SceneSettingsManager::GetSingleton();
	}

	void ReInit()
	{
		{
			using namespace game;

			shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			graphicsState = RE::BSGraphics::State::GetSingleton();
			renderer = RE::BSGraphics::Renderer::GetSingleton();
			smState = &RE::BSShaderManager::State::GetSingleton();
			iniSettingCollection = RE::INISettingCollection::GetSingleton();
			iniPrefSettingCollection = RE::INIPrefSettingCollection::GetSingleton();
			gameSettingCollection = RE::GameSettingCollection::GetSingleton();
			RefreshTES();
			cameraNear = (float*)(REL::RelocationID(517032, 403540).address() + 0x40);
			cameraFar = (float*)(REL::RelocationID(517032, 403540).address() + 0x44);
			deltaTime = (float*)REL::RelocationID(523660, 410199).address();

			currentPixelShader = &(shadowState->GetRuntimeData().currentPixelShader);
			currentVertexShader = &(shadowState->GetRuntimeData().currentVertexShader);
			stateUpdateFlags = &(shadowState->GetRuntimeData().stateUpdateFlags);

			ui = RE::UI::GetSingleton();
			calendar = RE::Calendar::GetSingleton();
			perFrame = { REL::RelocationID(524768, 411384) };

			currentAccumulator = { REL::RelocationID(527650, 414600) };
		}

		{
			using namespace rtti;
			NiIntegerExtraDataRTTI = { RE::NiIntegerExtraData::Ni_RTTI };
			BSLightingShaderPropertyRTTI = { RE::BSLightingShaderProperty::Ni_RTTI };
			BSEffectShaderPropertyRTTI = { RE::BSEffectShaderProperty::Ni_RTTI };
			BSWaterShaderPropertyRTTI = { RE::BSWaterShaderProperty::Ni_RTTI };
			NiParticleSystemRTTI = { RE::NiParticleSystem::Ni_RTTI };
			NiBillboardNodeRTTI = { RE::NiBillboardNode::Ni_RTTI };
			NiAlphaPropertyRTTI = { RE::NiAlphaProperty::Ni_RTTI };
			NiSourceTextureRTTI = { RE::NiSourceTexture::Ni_RTTI };
			BSGrassShaderPropertyRTTI = { RE::BSGrassShaderProperty::Ni_RTTI };
			BSMultiStreamInstanceTriShapeRTTI = { RE::BSMultiStreamInstanceTriShape::Ni_RTTI };
		}

		// DXVK's own objects, also when the engine's pointers hold the guest handles (CS_ORG_TRIPWIRE_GUEST, P4.1). In native mode
		// (CS_ORG_NATIVE=1) there is nothing under the handles: CS uses the native handles, so each of its D3D11 calls is a native
		// service or a tripwire that names its call site.
		if (RenderGraph::Engine::Native::Requested()) {
			d3d::device = reinterpret_cast<ID3D11Device*>(game::renderer->GetRuntimeData().forwarder);
			d3d::context = reinterpret_cast<ID3D11DeviceContext*>(game::renderer->GetRuntimeData().context);
		} else {
			d3d::device = RenderGraph::Engine::Tripwire::Unwrap(reinterpret_cast<ID3D11Device*>(game::renderer->GetRuntimeData().forwarder));
			d3d::context = RenderGraph::Engine::Tripwire::Unwrap(reinterpret_cast<ID3D11DeviceContext*>(game::renderer->GetRuntimeData().context));
		}
		d3d::swapChain = reinterpret_cast<IDXGISwapChain*>(game::renderer->GetRuntimeData().renderWindows->swapChain);
	}

	void OnDataLoaded()
	{
		using namespace game;
		RefreshTES();
		player = RE::PlayerCharacter::GetSingleton();
		tes = RE::TES::GetSingleton();
		sky = RE::Sky::GetSingleton();
		utilityShader = RE::BSUtilityShader::GetSingleton();
		imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		bEnableVolumetricLighting = reinterpret_cast<bool*>(REL::RelocationID(527940, 414913).address());

		bEnableLandFade = iniSettingCollection->GetSetting("bEnableLandFade:Display");

		bShadowsOnGrass = RE::GetINISetting("bShadowsOnGrass:Display");
		shadowMaskQuarter = RE::GetINISetting("iShadowMaskQuarter:Display");
	}

	void OnGameWindowClose()
	{
		game::quitGame = true;
		RenderGraph::Engine::Tripwire::Stop();
		RenderGraph::Engine::Native::Shutdown();
		if (shaderCache)
			shaderCache->StopCompilation();
	}

	/**
 * @brief Caches the current frame buffer data and clears the mapped pointer.
 *
 * Copies the contents of the mapped frame buffer into an internal cache and resets the mapped frame buffer pointer.
 */
	void CacheFramebuffer()
	{
		using namespace game;
		static_assert(sizeof(FrameBuffer) % sizeof(__m128i) == 0);

		auto* source = static_cast<__m128i*>(mappedFrameBuffer->pData);
		auto* destination = reinterpret_cast<__m128i*>(&frameBufferCached.data);

		if (reinterpret_cast<uintptr_t>(source) % alignof(__m128i) == 0) {
			for (size_t i = 0; i < sizeof(FrameBuffer) / sizeof(__m128i); ++i)
				_mm_storeu_si128(destination + i, _mm_stream_load_si128(source + i));
		} else {
			frameBufferCached.data = *reinterpret_cast<FrameBuffer*>(source);
		}

		mappedFrameBuffer = nullptr;
	}

	void OnContextMap(ID3D11Resource* a_resource, D3D11_MAP a_type, D3D11_MAPPED_SUBRESOURCE* a_mapped)
	{
		if (*globals::game::perFrame.get() == a_resource)
			globals::game::mappedFrameBuffer = a_mapped;
		DrawcallLimitFix::OnMap(a_resource, a_mapped->pData);
		RenderGraph::Engine::Constants::OnMap(a_resource, a_type, a_mapped->pData);
	}

	void OnContextUnmap(ID3D11Resource* a_resource)
	{
		if (*globals::game::perFrame.get() == a_resource && globals::game::mappedFrameBuffer)
			CacheFramebuffer();
		DrawcallLimitFix::OnUnmap(a_resource);
		RenderGraph::Engine::Constants::OnUnmap(a_resource);
	}

	/**
 * @brief Hooks the ID3D11DeviceContext::Map method to track mapping of the per-frame resource.
 *
 * Calls the original Map function and, if the mapped resource matches the current per-frame buffer, stores the mapped subresource pointer for later use.
 *
 * @return HRESULT Result of the original Map call.
 */
	struct ID3D11DeviceContext_Map
	{
		static HRESULT thunk(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
		{
			HRESULT hr = func(This, pResource, Subresource, MapType, MapFlags, pMappedResource);
			if (hr == S_OK)
				OnContextMap(pResource, MapType, pMappedResource);
			return hr;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
 * @brief Hooked implementation of ID3D11DeviceContext::Unmap that caches the frame buffer if applicable.
 *
 * If the resource being unmapped matches the current per-frame buffer and a mapped frame buffer is present, caches the frame buffer data before calling the original Unmap function.
 */
	struct ID3D11DeviceContext_Unmap
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource)
		{
			OnContextUnmap(pResource);
			func(This, pResource, Subresource);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Feeds Drawcall Limit Fix's constant buffer mirror (buffers updated without Map). */
	struct ID3D11DeviceContext_UpdateSubresource
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch,
			UINT SrcDepthPitch)
		{
			DrawcallLimitFix::OnUpdateSubresource(pDstResource, pDstBox, pSrcData);
			RenderGraph::Engine::Constants::OnUpdateSubresource(pDstResource, pDstBox, pSrcData);
			func(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// CS_PASS_SKIP's drops: the engine runs as usual, and these calls do nothing inside the chosen event.
	struct PassSkipDrawIndexed
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, INT c)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b, c);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDraw
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDrawIndexedInstanced
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, UINT c, INT d, UINT e)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b, c, d, e);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDrawInstanced
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, UINT c, UINT d)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b, c, d);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDrawAuto
	{
		static void thunk(ID3D11DeviceContext* a_context)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDrawIndexedInstancedIndirect
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Buffer* a, UINT b)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDrawInstancedIndirect
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Buffer* a, UINT b)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDispatch
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, UINT c)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b, c);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct PassSkipDispatchIndirect
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Buffer* a, UINT b)
		{
			if (!GpuEventTimers::PassSkipDropping())
				func(a_context, a, b);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct OpDrawIndexed
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, INT c)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "DrawIndexed", a, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b, c);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpDraw
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "Draw", a, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpDrawIndexedInstanced
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, UINT c, INT d, UINT e)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "DrawIdxInst", a, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b, c, d, e);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpDrawInstanced
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, UINT c, UINT d)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "DrawInst", a, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b, c, d);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpDrawIndexedInstancedIndirect
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Buffer* a, UINT b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "DrawIdxIndirect", 0, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpDrawInstancedIndirect
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Buffer* a, UINT b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "DrawIndirect", 0, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpDispatch
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a, UINT b, UINT c)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "Dispatch", a * b * c, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b, c);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpDispatchIndirect
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Buffer* a, UINT b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "DispatchIndirect", 0, nullptr, nullptr, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpCopyRegion
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a, UINT b, UINT c, UINT d, UINT e, ID3D11Resource* f, UINT g, const D3D11_BOX* h)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "CopyRegion", h ? 1 : 0, a, nullptr, _ReturnAddress());
			func(a_context, a, b, c, d, e, f, g, h);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpCopy
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a, ID3D11Resource* b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "Copy", 0, a, nullptr, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpUpdate
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a, UINT b, const D3D11_BOX* c, const void* d, UINT e, UINT f)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "Update", 0, a, nullptr, _ReturnAddress());
			func(a_context, a, b, c, d, e, f);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpClearRTV
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a, const FLOAT* b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "ClearRTV", 0, nullptr, a, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpClearUAVU
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11UnorderedAccessView* a, const UINT* b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "ClearUAVU", 0, nullptr, a, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpClearUAVF
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11UnorderedAccessView* a, const FLOAT* b)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "ClearUAVF", 0, nullptr, a, _ReturnAddress());
			func(a_context, a, b);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpClearDSV
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a, UINT b, FLOAT c, UINT8 d)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "ClearDSV", 0, nullptr, a, _ReturnAddress());
			func(a_context, a, b, c, d);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpGenMips
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "GenMips", 0, nullptr, a, _ReturnAddress());
			func(a_context, a);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct OpResolve
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a, UINT b, ID3D11Resource* c, UINT d, DXGI_FORMAT e)
		{
			const bool on = GpuEventTimers::OpBegin(a_context, "Resolve", 0, a, nullptr, _ReturnAddress());
			func(a_context, a, b, c, d, e);
			if (on)
				GpuEventTimers::OpEnd();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallD3DHooks(ID3D11DeviceContext* a_context)
	{
		if (GpuEventTimers::PassSkipRequested()) {
			stl::detour_vfunc<12, PassSkipDrawIndexed>(a_context);
			stl::detour_vfunc<13, PassSkipDraw>(a_context);
			stl::detour_vfunc<20, PassSkipDrawIndexedInstanced>(a_context);
			stl::detour_vfunc<21, PassSkipDrawInstanced>(a_context);
			stl::detour_vfunc<38, PassSkipDrawAuto>(a_context);
			stl::detour_vfunc<39, PassSkipDrawIndexedInstancedIndirect>(a_context);
			stl::detour_vfunc<40, PassSkipDrawInstancedIndirect>(a_context);
			stl::detour_vfunc<41, PassSkipDispatch>(a_context);
			stl::detour_vfunc<42, PassSkipDispatchIndirect>(a_context);
		}
		TextureStreaming::Census::InstallD3DHooks(a_context);
		if (GpuEventTimers::OpProbeRequested()) {
			stl::detour_vfunc<12, OpDrawIndexed>(a_context);
			stl::detour_vfunc<13, OpDraw>(a_context);
			stl::detour_vfunc<20, OpDrawIndexedInstanced>(a_context);
			stl::detour_vfunc<21, OpDrawInstanced>(a_context);
			stl::detour_vfunc<39, OpDrawIndexedInstancedIndirect>(a_context);
			stl::detour_vfunc<40, OpDrawInstancedIndirect>(a_context);
			stl::detour_vfunc<41, OpDispatch>(a_context);
			stl::detour_vfunc<42, OpDispatchIndirect>(a_context);
			stl::detour_vfunc<46, OpCopyRegion>(a_context);
			stl::detour_vfunc<47, OpCopy>(a_context);
			stl::detour_vfunc<48, OpUpdate>(a_context);
			stl::detour_vfunc<50, OpClearRTV>(a_context);
			stl::detour_vfunc<51, OpClearUAVU>(a_context);
			stl::detour_vfunc<52, OpClearUAVF>(a_context);
			stl::detour_vfunc<53, OpClearDSV>(a_context);
			stl::detour_vfunc<54, OpGenMips>(a_context);
			stl::detour_vfunc<57, OpResolve>(a_context);
		}
		stl::detour_vfunc<14, ID3D11DeviceContext_Map>(a_context);
		stl::detour_vfunc<15, ID3D11DeviceContext_Unmap>(a_context);
		stl::detour_vfunc<48, ID3D11DeviceContext_UpdateSubresource>(a_context);
		// CS_ORG_ENGINE_CONSTANTS=1: the constant-buffer binds behind the per-use capture.
		RenderGraph::Engine::Constants::InstallD3DHooks(a_context);
	}
}
