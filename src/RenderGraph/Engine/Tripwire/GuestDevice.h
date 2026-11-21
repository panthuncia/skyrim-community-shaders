#pragma once

#include <d3d11.h>

/**
 * @brief The guest device and context, in guest mode. Wrapping tripwire handles go in the engine's device and
 * context pointers, so the engine can pass tripwire handles to D3D11 while DXVK is the host.
 *
 * DXVK casts every object it gets to its own class, so a handle reaching DXVK directly crashes. The guest handles unwrap
 * handle arguments before forwarding. The replaced pointers are the device global (SE 524729 / AE 411348), the context global
 * (SE 524771 / AE 411391) and RendererData's device and context at +0x38 / +0x40.
 * globals::d3d::device and context stay DXVK's own objects, since CS's hooks and DXVK's exports need them. Use Tripwire::Unwrap.
 */
namespace RenderGraph::Engine::Tripwire
{
	bool GuestRequested();

	/** @brief Puts the guest handles in the engine's pointers, right after InitD3D and before any other thread renders. Once. */
	void InstallGuestDevice();

	/** @brief Marks the native handles the engine already stored as installed, so ports install handles as with the guest. Once. */
	void InstallNativeDevice(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	/** @brief Whether the engine's device and context pointers hold guest or native handles. Ports gate their installs on it. */
	bool GuestContextInstalled();

	/**
	 * @brief The engine's immediate context: its guest or native handle once installed, else D3D11's own (globals::d3d::context).
	 * The handle keeps what the engine binds, so CS code that reads the engine's state asks it. In guest mode CS's own binds are on
	 * D3D11's context instead. Any thread.
	 */
	ID3D11DeviceContext* EngineContext();
}
