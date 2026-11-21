#pragma once

#include <d3d11.h>

#include <cstdint>

#include "RenderGraph/FrameAnchors.h"

/**
* @brief The engine's side of the engine target set (EngineTargetSet.h): the four create detours and the set's begin and end.
*
* The engine's first set is made with D3D11 before the graph exists. Engine::Setup then declares every target (DeclareSet), and
* the fields get tripwire handles over the declared textures. After that the create detours only record the engine's properties
* and put the same handles back into the fields. They never make or release anything, and they never call the engine's function,
* except for the targets that stay the engine's own (mip-generating targets and mip views of them). EndSet raises the shapes,
* and the revision that carries them points the handles at the new versions at its adoption.
*/
namespace RenderGraph::Engine::Targets
{
	void Install();

	/** @brief Call before BSShaderRenderTargets::Create. Opens a set. */
	void BeginSet();

	/**
	* @brief Call after BSShaderRenderTargets::Create. Before DeclareSet it only records. After it, it saves the size the engine
	* wrote into BSGraphics::State, writes back the size of the targets the frame has until adoption, and raises
	* Vars::EngineTargetShapes() and Vars::DisplaySize().
	*/
	void EndSet();

	/** @brief Whether DeclareSet ran, so a set now is a later set that only records. Hooks.cpp skips State::Setup then. Any thread. */
	bool OwnsSets();

	/**
	* @brief Puts tripwire handles in the fields once the guest device and context exist (guest mode: the Renderer::Init state
	* setup hook; native mode: DeclareSet itself). Fills Identify's map. Once; later calls do nothing.
	*/
	void InstallHandles();

	/** @brief Fatal when a colour target's field no longer holds the set's handle (a feature wrote it). Render thread. */
	void CheckFields(std::string_view a_where);

	/** @brief How a CS feature changes an engine target's shape. Only read by the target's declaration. */
	struct Plan
	{
		const char* owner = "?";                   // for the log
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // UNKNOWN keeps the engine's
		UINT bindFlags = 0;                        // 0 keeps the engine's
		bool mainSize = false;                     // kMAIN's size instead of the engine's own
		bool dropCopy = false;                     // otherwise the copy keeps the engine's format, as HDR leaves it
	};

	/** @brief Call before Engine::Setup (DeclareSet). A later call for the same target replaces the plan; one after DeclareSet is fatal. */
	void PlanTarget(std::uint32_t a_target, const Plan& a_plan);
}
