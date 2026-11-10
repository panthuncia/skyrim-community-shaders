#pragma once

#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <string>
#include <winrt/base.h>

namespace org
{
	class Resource;
}

/**
 * @brief The engine's textures as ORG textures (org-full-port.md P4.3), behind CS_ORG_ENGINE_TEXTURES=1.
 *
 * The engine makes its textures in two places, both hooked here (docs/development/p4/textures.md):
 * - CreateD3DResources (SE 75724, AE 77538): the DDS loader's CreateTexture1D/2D/3D with the file's mips as initial data, then
 *   the SRV. It runs on BSResource's loader threads with no lock.
 * - Renderer::CreateTexture2D (SE 75511, AE 77303): one-mip textures (the renderer's default textures, Scaleform's external
 *   textures, FaceGen copies). Only DEFAULT ones come here; DYNAMIC (Bink) and STAGING ones stay the engine's.
 *
 * Both go through Create, the one path a texture takes: BasicRHI creates the image, which ORG owns. DXVK wraps it as the
 * D3D11 texture the engine keeps (the guest phase), and uploads its contents in its own stream where it would otherwise
 * zero it (dxvkCreateTextureFromVkImageWithData). So the contents are there before the engine's or the graph's first use,
 * from any thread, with no wait and no pass. Without that export, or anywhere this cannot make a texture, the engine's own
 * call runs.
 *
 * The engine's fields hold the D3D11 objects, or tripwire handles wrapping them once the tripwire's guest context is
 * installed (Tripwire::GuestContextInstalled), since only then does every engine bind go through a handle-aware context.
 *
 * Releases come from any thread, as the engine's do: the D3D11 texture's and view's last references carry a sentinel (private
 * data) whose release queues the texture's record. The render thread hands those to ReleaseAfterD3D11 (ServiceRetired), so the
 * image outlives every D3D11 command issued before the release. Graph readers (DCLF) hold the record themselves.
 */
namespace RenderGraph::Engine::Textures
{
	/** @brief Whether CS_ORG_ENGINE_TEXTURES=1 asked for engine textures on ORG (not on VR). */
	bool Requested();

	/** @brief Whether textures are made here now: requested, and the render graph can wrap images with their contents. Any thread. */
	bool Active();

	/** @brief Main thread, at plugin load: hooks the texture creators when requested. */
	void InstallHooks();

	/** @brief An engine texture that ORG owns. Immutable once made. */
	struct Texture
	{
		virtual ~Texture() = default;

		/** @brief "Engine::<file>" for a file texture, "Engine::Texture2D <w>x<h>" for the others. */
		std::string name;
		/** @brief The graph texture, in the layout DXVK rests the image in (its home state). Its GetAPIResource is the image. */
		std::shared_ptr<org::Resource> graph;
		/** @brief The D3D11 description of the texture (2D; a 3D texture keeps its depth in ArraySize). */
		D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
		D3D11_TEXTURE2D_DESC desc{};
		/** @brief The swizzle DXVK's views of the texture apply (VkComponentSwizzle r, g, b, a), for views made beside them. */
		std::int32_t components[4]{};
	};

	/** @brief What Create makes a texture from. */
	struct CreateInfo
	{
		D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;  // 2D (cubes too) or 3D
		UINT width = 0;
		UINT height = 0;
		UINT depthOrArraySize = 1;  // the depth of a 3D texture, the array size of a 2D one (six per cube)
		UINT mipLevels = 1;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // typed
		UINT bindFlags = D3D11_BIND_SHADER_RESOURCE;
		UINT miscFlags = 0;  // only D3D11_RESOURCE_MISC_TEXTURECUBE
		/** @brief One entry per subresource (mip-major within each slice), as CreateTexture2D takes it; null for zeroed contents. */
		const D3D11_SUBRESOURCE_DATA* initialData = nullptr;
		/** @brief The view the engine stores beside the texture. */
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		std::string name;
	};

	/** @brief What Create hands the engine: its fields' references, each with one reference for the engine. */
	struct Created
	{
		winrt::com_ptr<ID3D11Resource> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
	};

	/**
	 * @brief The one path a texture takes onto ORG: the image, its contents, its D3D11 texture and view (or tripwire handles of
	 * them), and the record Find answers with. Any thread, never waits.
	 * @return false (logged once per reason) when it cannot: the caller makes the texture as the engine would.
	 */
	bool Create(const CreateInfo& a_info, Created& a_out);

	/**
	 * @brief The engine texture a view belongs to (its D3D11 view or its tripwire handle), or null. Any thread; a_view must be
	 * alive. The record stays valid as long as the caller holds it.
	 */
	std::shared_ptr<const Texture> Find(const ID3D11ShaderResourceView* a_view);

	/** @brief Render thread, each frame (RenderGraphRuntime::SignalFramePace): released textures go to ReleaseAfterD3D11. */
	void ServiceRetired();

	/** @brief Totals for the reports. */
	struct Stats
	{
		std::uint64_t created = 0;     // textures made here
		std::uint64_t live = 0;        // records not freed yet (released ones wait for the GPU)
		std::uint64_t handles = 0;     // made with tripwire handles in the engine's fields
		std::uint64_t left = 0;        // creations left to the engine (a format, a dimension or a usage this does not take)
		std::uint64_t failed = 0;      // creations that failed here and fell back to the engine
	};
	Stats GetStats();
}
