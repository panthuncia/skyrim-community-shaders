#include "RenderGraph/Engine/Engine.h"

#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/Pipeline/EngineHooks.h"
#include "RenderGraph/Engine/Scaleform/Scaleform.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Targets/EngineTargets.h"
#include "RenderGraph/Engine/Textures/EngineTextures.h"

namespace RenderGraph::Engine
{
	void InstallGameHooks()
	{
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
	}
}
