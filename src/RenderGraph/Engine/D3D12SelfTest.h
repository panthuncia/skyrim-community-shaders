#pragma once

namespace RenderGraph::Engine::D3D12SelfTest
{
	/**
	 * @brief CS_ORG_D3D12_SELFTEST=1: proves the D3D12 backend on this machine, beside the game, without the native mode.
	 *
	 * On a thread of its own it creates a D3D12 device with CreateD3D12Device and runs three checks:
	 * 1. DXC builds RenderGraph/D3D12SelfTest.hlsl to DXIL with the dual-target resource list (CS_ORG_BINDLESS).
	 * 2. A compute dispatch on that device writes a buffer through ResourceDescriptorHeap and root constants, and the readback
	 *    matches.
	 * 3. A small window's swap chain takes ConfigureSwapchain (HDR10 with CS_ORG_D3D12_SELFTEST_HDR=1, frame latency 2) and
	 *    presents 120 cleared frames, pacing on the waitable object.
	 * Each step logs "[ORG] D3D12 self-test: ..." and the last line says passed or names the step that failed. The game keeps
	 * its own device: nothing here touches DXVK or the render graph. CS_ORG_D3D12_DEBUG=1 turns the D3D12 debug layer on.
	 */
	void Start();
}
