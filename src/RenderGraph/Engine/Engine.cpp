#include "RenderGraph/Engine/Engine.h"

#include "Features/VolumetricLighting/ORGVolumetricLighting.h"

#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/Pipeline/EngineHooks.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Engine/Scaleform/Scaleform.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Targets/EngineTargets.h"
#include "RenderGraph/Engine/Textures/EngineTextures.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"

namespace RenderGraph::Engine
{
	void InstallGameHooks()
	{
		// First, so its DispatchCSShader detour sits inside ImageSpace's and Hooks.cpp's, and its SetDirtyStates detour outside Hooks.cpp's.
		Flush::Install();
		Textures::InstallHooks();
		Buffers::InstallGameHooks();
		Constants::InstallGameHooks();
		Bink::InstallHooks();
		LoadingScreen::Install();
		Pipeline::InstallGameHooks();
		Targets::Install();
		Skeleton::Install();
		ImageSpace::Install();
		Scaleform::Install();
		OcclusionQueries::Install();
		// Native mode installs it with the feature off too.
		ORGVolumetricLighting::Install();
	}

	void Reserve()
	{
		Skeleton::Reserve();
		ImageSpace::Reserve();
		ORGVolumetricLighting::Reserve();
	}

	void Setup()
	{
		// Before any feature reads the targets.
		Targets::TakeOver();
		// Once its targets are where they stay.
		Skeleton::Setup();
		ImageSpace::Setup();
	}

	void PostSetup()
	{
		// After HDR made its UI texture.
		Scaleform::Setup();
		// After SetupResources so HDR's UI texture is in the client from the start, and the startup logo needs no refresh.
		Bink::Setup();
		// After SetupResources, which make Terrain and Cloud Shadows' inputs.
		(void)ORGVolumetricLighting::Get().Setup();
		if (RenderGraphRuntime::Get().IsActive()) {
			(void)FrameAnchors::BuildPending("CS's setup");
			// Record the UI stage's first schedule now so the startup logo's first frame draws it.
			FrameAnchors::PrimeStages("CS's setup");
		}
	}
}
