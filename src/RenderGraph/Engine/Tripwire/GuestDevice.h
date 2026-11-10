#pragma once

#include <d3d11.h>

/**
 * @brief The guest device and context (org-full-port.md P4.1, CS_ORG_TRIPWIRE_GUEST=1): wrapping tripwire handles in the
 * engine's device and immediate-context pointers, so the engine can pass tripwire handles to D3D11 while DXVK is the host.
 *
 * DXVK casts every object it is given to its own class, so a handle that reaches DXVK's context directly crashes there. With the
 * guest device and context installed, every engine call goes through a handle that unwraps handle arguments before it forwards
 * the call to DXVK, and logs each call site that passes one (a reader not ported yet). The pointers replaced are the device global
 * (SE 524729 / AE 411348), the context global (SE 524771 / AE 411391) and RendererData's device and context (+0x38 / +0x40, the
 * same fields Scaleform reads through GetScaleformRendererParameters, 75576 / 77378). Scaleform's renderer is made after the
 * install, so it gets the handles too.
 *
 * globals::d3d::device and globals::d3d::context stay DXVK's own objects (ReInit unwraps them): CS's D3D11 hooks sit on DXVK's
 * vtables, DXVK's private exports take DXVK's objects, and CS's own calls skip the forwarding. CS code that passes an engine
 * field to them goes through Tripwire::Unwrap.
 *
 * With the switch off nothing changes. Ports install handles in engine fields only when GuestContextInstalled() is true.
 */
namespace RenderGraph::Engine::Tripwire
{
	/** @brief Whether CS_ORG_TRIPWIRE_GUEST=1 asks for the guest device and context. */
	bool GuestRequested();

	/**
	 * @brief Startup, right after the engine created its device and context (the InitD3D hook in Hooks.cpp), on the main thread
	 * before any other thread renders: puts the guest handles in the engine's pointers when GuestRequested(). Does nothing
	 * otherwise, or a second time. Logs what it did.
	 */
	void InstallGuestDevice();

	/** @brief Whether the engine's device and context pointers hold the guest handles. Ports gate their handle installs on it. */
	bool GuestContextInstalled();

	/** @brief The guest device handle (no reference added), null before InstallGuestDevice. */
	ID3D11Device* GuestDeviceHandle();

	/** @brief The guest immediate-context handle (no reference added), null before InstallGuestDevice. */
	ID3D11DeviceContext* GuestContextHandle();
}
