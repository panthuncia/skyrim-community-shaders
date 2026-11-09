#pragma once

#include <cstdint>
#include <functional>

struct IDXGISwapChain;
struct ID3D11Device;
struct DXGI_SWAP_CHAIN_DESC;

namespace RenderGraph::Engine
{
	/**
	 * @brief The owned frame's tail (CS_ORG_OWNED_FRAME=1): the game's frame ends in a BasicRHI swap chain that ORG presents.
	 *
	 * DXVK runs as a guest on BasicRHI's device (GuestDevice) with a headless DXGI swap chain. At the game's Present the guest
	 * frame ends: DXVK's stream signals the guest-frame timeline, and the headless Present flushes. A queue callback behind that
	 * flush, on DXVK's submission thread with the graphics queue held, then waits for the point on the GPU, copies the guest back
	 * buffer into the acquired BasicRHI swap chain image and presents it (rhi::Swapchain::Present). The caller's thread does no
	 * Vulkan work, so the main frame, the loading screen's job thread and Bink present the same way under the engine's lock.
	 */
	class OwnedFrame
	{
	public:
		static OwnedFrame& Get();

		/** @brief Whether the owned frame presents (Initialize succeeded). */
		static bool Active();

		/**
		 * @brief Creates the BasicRHI swap chain on the game's window, before the game's device exists (GuestDevice::Prepare made the
		 * device). A failure leaves the owned frame off.
		 * @param a_desc The game's swap chain description, as CS adjusted it.
		 */
		bool CreateSwapchain(const DXGI_SWAP_CHAIN_DESC& a_desc);

		/** @brief After the game's device and headless swap chain exist: resolves DXVK's interop exports. */
		bool Initialize(IDXGISwapChain* a_swapChain, ID3D11Device* a_device);

		/**
		 * @brief The game's Present in the owned frame (the innermost call of CS's Present chain). Any thread that drives the immediate
		 * context under the engine's lock.
		 * @param a_guestPresent DXVK's Present: on a headless swap chain it ends the guest frame and flushes.
		 */
		long Present(IDXGISwapChain* a_swapChain, unsigned a_syncInterval, unsigned a_flags,
			const std::function<long(IDXGISwapChain*, unsigned, unsigned)>& a_guestPresent);

		/** @brief HDRDisplay's colour space for the swap chain, applied at the next Present. */
		void RequestColorSpace(bool a_hdr);

		/** @brief The swap chain's surface, as dxvkGetPresenterSurfaceState reports DXVK's (Vulkan values). Serial 0: none yet. */
		struct SurfaceState
		{
			std::uint64_t serial = 0;
			std::uint32_t format = 0;
			std::uint32_t requestedColorSpace = 0;
			std::uint32_t effectiveColorSpace = 0;
		};
		SurfaceState GetSurfaceState() const;

	private:
		OwnedFrame() = default;
		struct State;
		State* state = nullptr;  // never freed: DXVK's submission thread may still run a tail at static destruction
	};
}
