#include "RenderGraph/Engine/Engine.h"

#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/Pipeline/EngineHooks.h"
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
		// CS_ORG_RESIDUE_CENSUS / CS_ORG_ENGINE_FLUSH_LATCH: first, so its DispatchCSShader detour is inside ImageSpace's and Hooks.cpp's;
		// its SetDirtyStates detour is outside Hooks.cpp's, which is installed before this.
		Flush::Install();
		Textures::InstallHooks();
		Buffers::InstallGameHooks();
		Constants::InstallGameHooks();
		// CS_ORG_BINK: the movie player's tiles on the render graph (reserves the Present anchor).
		Bink::InstallHooks();
		// CS_ORG_LOADING_THREAD: the render graph on whichever thread begins the engine's frame (the loading screen, the logo).
		LoadingScreen::Install();
		Pipeline::InstallGameHooks();
		Targets::Install();
		// CS_ORG_ENGINE_SKELETON: the engine's clear and depth copy call sites.
		Skeleton::Install();
		// CS_ORG_IMAGESPACE: the ImageSpace draws the graph takes (BSImagespaceShader::Render, the HDR effect, the pre-tonemap composites).
		ImageSpace::Install();
		Scaleform::Install();
	}

	void Reserve()
	{
		Skeleton::Reserve();
		ImageSpace::Reserve();
	}

	void Setup()
	{
		// CS_ORG_ENGINE_TARGETS: the engine's targets, made on D3D11 before the graph existed, move to ORG before any feature reads them.
		Targets::TakeOver();
		// CS_ORG_ENGINE_SKELETON: the engine's clears and its main depth copy on the graph, once its targets are where they stay.
		Skeleton::Setup();
		// CS_ORG_IMAGESPACE: the ImageSpace draws on the graph; the client registers once the effects' shapes are known.
		ImageSpace::Setup();
		// CS_ORG_BINK: the movie player's client at the Present anchor, registered once for the session.
		Bink::Setup();
	}

	void PostSetup()
	{
		// CS_ORG_SCALEFORM: the framebuffer's and HDR's UI texture's surfaces (HDR made its texture in its SetupResources).
		Scaleform::Setup();
		// Every client registered at setup joins its anchors' graphs now, not at the first frame's build point.
		if (RenderGraphRuntime::Get().IsActive())
			(void)FrameAnchors::BuildPending("CS's setup");
	}
}
