#pragma once

namespace RenderGraph::Engine::D3D12SelfTest
{
	/**
	* @brief CS_ORG_D3D12_SELFTEST=1 checks the D3D12 backend on this machine on its own thread, beside the game.
	* It compiles DXIL, runs compute dispatches, and presents frames on a small window. Each step logs, and the last line says
	* passed or names the failed step. Nothing here touches DXVK or the render graph.
	*/
	void Start();
}
