#include "RenderGraph/Engine/Engine.h"

#include "Features/VolumetricLighting/ORGVolumetricLighting.h"

#include "RenderGraph/Engine/Bink/Bink.h"
#include "RenderGraph/Engine/Bink/BinkGraph.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Pipeline/EngineHooks.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Engine/Scaleform/Passes.h"
#include "RenderGraph/Engine/Scaleform/Scaleform.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Targets/EngineTargetSet.h"
#include "RenderGraph/Engine/Targets/EngineTargets.h"
#include "RenderGraph/Engine/Textures/EngineTextures.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/SharedConstants.h"

namespace RenderGraph::Engine
{
	void InstallGameHooks()
	{
		// First, so its DispatchCSShader detour sits inside ImageSpace's and Hooks.cpp's.
		Flush::Install();
		Textures::InstallHooks();
		Buffers::InstallGameHooks();
		Constants::InstallGameHooks();
		Bink::InstallHooks();
		LoadingScreen::Install();
		// The frame driver detours Renderer::Begin. After the Bink hooks, which mark their own ticks.
		Frame::Driver::Install();
		Frame::PostChain::Install();
		Pipeline::InstallGameHooks();
		Targets::Install();
		Skeleton::Install();
		ImageSpace::Install();
		Scaleform::Install();
		OcclusionQueries::Install();
		// Native mode installs it with the feature off too.
		ORGVolumetricLighting::Install();
	}

	void Setup()
	{
		// The shared constants, before any client that reads them registers. CS sized FeatureData in State::SetupResources.
		// Without them those clients fail to register.
		if (RenderGraphRuntime::Get().IsActive())
			(void)::SharedConstants::Ensure();
		// Before any feature reads the targets: every engine target becomes a declared texture.
		Targets::DeclareSet();
		// Before any client adds a depth copy. Built with the setup batch.
		AnchorPasses::QueuePrograms();
		// Once its targets are where they stay.
		Skeleton::Setup();
		ImageSpace::Setup();
	}

	void PostSetup()
	{
		// After HDR made its UI texture.
		Scaleform::Passes::Setup();
		// After SetupResources so HDR's UI texture is in the client from the start, and the startup logo needs no refresh.
		Bink::Graph::Setup();
		// After SetupResources, which make Terrain and Cloud Shadows' inputs.
		(void)ORGVolumetricLighting::Get().Setup();
		if (RenderGraphRuntime::Get().IsActive()) {
			// The setup batch: every program queued at setup is built before the setup graphs that read it.
			Programs::JoinSetup("CS's setup");
			// Every declaration is evaluated here, so its client is in the setup build and runs from the first frame.
			Frame::Revision::EvaluateAtSetup("CS's setup");
			(void)FrameAnchors::BuildPending("CS's setup");
			// Record the UI stage's first schedule now so the startup logo's first frame draws it.
			FrameAnchors::PrimeStages("CS's setup");
		}
	}
}
