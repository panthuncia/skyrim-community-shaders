#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace RE
{
	class GTexture;
}

/**
 * @brief CS's own GTexture and GRenderTarget objects for native mode, and the renderer state behind them (video mode, uploads).
 *
 * The D3D1x renderer's texture and target slots are CS's in native mode. GFx gets CS objects with GTextureD3D1x's and
 * GRenderTargetD3D1x's field layout (so code that reads those fields keeps working) and a CS vtable built from the D3D1x one:
 * the D3D1x functions are CS ports, the GFx base functions (user data, change handlers) stay GFx's. Objects are refcounted the
 * GFx way (+0x08, slot 0 deletes) and live as long as GFx holds them.
 *
 * Contents: a texture made with an image gets all of it at creation (any thread). Later writes (glyph cache Update, Unmap) are
 * uploads: copied and queued with the UI stream they belong to, and applied on the render owner before that stream's draws are
 * replayed, in call order. A stream that is never replayed still has its uploads applied before the next one. Streams hold their
 * own references to the views they read, so a texture GFx releases stays alive while a snapshot may still draw it. The render side
 * lets streams and applied uploads go through the retire queue (Snapshot/Retire.h), once the GPU finished the frame that read them.
 */
namespace RenderGraph::Engine::Scaleform::UiResources
{
	/** @brief Builds the vtables from the D3D1x ones. Native mode, before the renderer makes its first texture. */
	void Install();

	/** @brief GTextureD3D1x's layout, which CS's texture keeps. */
	struct Texture
	{
		void** vtable;                   // 0x00
		volatile long refCount;          // 0x08
		std::uint32_t pad0C;             // 0x0C
		void* node[2];                   // 0x10: the D3D1x renderer's texture list, unused
		void* userData;                  // 0x20
		std::uint8_t multipleHandlers;   // 0x28
		std::uint8_t pad29[7];           // 0x29
		void* handlers;                  // 0x30: one ChangeHandler, or a GArray of them
		D3D11_TEXTURE2D_DESC desc;       // 0x38
		std::uint32_t pad64;             // 0x64
		void* renderer;                  // 0x68
		ID3D11Texture2D* texture;        // 0x70, one reference
		ID3D11ShaderResourceView* view;  // 0x78, one reference
		// CS's own, past the D3D1x layout.
		std::uint8_t* mapped;  // Map's CPU copy of every level, null when not mapped
		std::uint32_t mappedSize;
	};
	static_assert(offsetof(Texture, desc) == 0x38 && offsetof(Texture, renderer) == 0x68 && offsetof(Texture, view) == 0x78);

	/** @brief GRenderTargetD3D1x's layout, which CS's target keeps. */
	struct RenderTarget
	{
		void** vtable;                  // 0x00
		volatile long refCount;         // 0x08
		std::uint32_t pad0C;            // 0x0C
		void* node[2];                  // 0x10
		void* userData;                 // 0x20
		std::uint8_t multipleHandlers;  // 0x28
		std::uint8_t pad29[7];          // 0x29
		void* handlers;                 // 0x30
		RE::GTexture* target;           // 0x38, one GFx reference
		RE::GTexture* depth;            // 0x40, one GFx reference
		void* renderer;                 // 0x48
		ID3D11RenderTargetView* rtv;    // 0x50, borrowed from the engine (InitRenderTarget with views)
		ID3D11DepthStencilView* dsv;    // 0x58, borrowed
		std::uint32_t width;            // 0x60: the size the pusher asked for
		std::uint32_t height;           // 0x64
		std::uint8_t pooled;            // 0x68: a temp pool target, whose depth Pop lets go
	};
	static_assert(offsetof(RenderTarget, target) == 0x38 && offsetof(RenderTarget, rtv) == 0x50 && offsetof(RenderTarget, pooled) == 0x68);

	/** @brief CreateTexture (renderer slot 2): a CS texture with one reference. Any thread. */
	RE::GTexture* CreateTexture(void* a_renderer);

	/** @brief CreateRenderTarget (renderer slot 6): a CS target with one reference. Any thread. */
	RenderTarget* CreateRenderTarget(void* a_renderer);

	/** @brief Whether a_texture is one of CS's. */
	bool IsTexture(const RE::GTexture* a_texture);

	/** @brief Whether a_target is one of CS's. */
	bool IsRenderTarget(const void* a_target);

	/** @brief GFx's AddRef and Release on a refcounted object (+0x08, slot 0 with 1 deletes). Null is ignored. */
	void AddRef(void* a_object);
	void Release(void* a_object);

	/** @brief The object's refcount now. */
	std::int32_t RefCount(const void* a_object);

	/** @name The video mode. SetDependentVideoMode keeps the engine's device and context; textures are made and written through them. */
	/** @{ */
	void SetVideoMode(ID3D11Device* a_device, ID3D11DeviceContext* a_context);
	void ResetVideoMode();
	bool VideoModeSet();
	ID3D11Device* Device();
	ID3D11DeviceContext* Context();
	/** @} */

	/**
	 * @brief PushTempRenderTarget's pool (SE 82888 / AE 85005), CS's port: a pooled target at least a_width x a_height, with a
	 * depth-stencil when a_stencil, its asked size set. The caller pushes it. Render owner.
	 */
	RenderTarget* AcquireTempTarget(void* a_renderer, std::uint32_t a_width, std::uint32_t a_height, bool a_stencil);

	/** @brief ReleaseTempRenderTargets (SE 82895 / AE 85012), CS's port: keeps the targets in use and the rest up to a_keepPixels. */
	void ReleaseTempTargets(std::uint32_t a_keepPixels);

	/**
	 * @name Uploads in stream order. The recorder names the stream it records; uploads made while it is open belong to it, and
	 * uploads made between streams belong to the next one.
	 */
	/** @{ */
	/** @brief The recorder opened stream a_sequence. Render owner. */
	void OpenStream(std::uint64_t a_sequence);
	/** @brief The recorder finished stream a_sequence. Render owner. */
	void CloseStream(std::uint64_t a_sequence);
	/** @brief Applies every queued upload of streams up to a_sequence, in call order. Render owner. */
	void ApplyUploads(std::uint64_t a_sequence);
	/** @} */

	/** @brief Counts since the last call, for the census line. */
	std::string TakeStatsLine();
}
