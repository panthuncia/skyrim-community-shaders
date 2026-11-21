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
* @brief The engine's textures as ORG textures, in graph mode (Engine::GraphMode). The creators were checked on SE 1.5.97 and AE
* 1.6.1170, not on VR.
*
* Hooks the DDS loader's CreateD3DResources (SE 75724, AE 77538), which runs on loader threads with no lock, and
* Renderer::CreateTexture2D (SE 75511, AE 77303) for DEFAULT one-mip textures. BasicRHI creates the image and DXVK wraps it
* as the engine's D3D11 texture, uploading the contents in its own stream. So the contents are ready before first use, from
* any thread, with no wait. When this cannot make a texture the engine's own call runs.
*
* Releases come from any thread. The D3D11 texture and view hold the record until DXVK destroys them, then it goes to
* ReleaseAfterD3D11, so the image outlives every D3D11 command issued before the release.
*/
namespace RenderGraph::Engine::Textures
{
	/** @brief Graph mode (Engine::GraphMode), and the render graph can wrap images with their contents. Any thread. */
	bool Active();

	/** @brief Main thread, at plugin load. */
	void InstallHooks();

	/** @brief Immutable once made. */
	struct Texture
	{
		virtual ~Texture() = default;

		/** @brief "Engine::<file>" for a file texture, "Engine::Texture2D <w>x<h>" for the others. */
		std::string name;
		/** @brief In the layout DXVK rests the image in. Its GetAPIResource is the image. */
		std::shared_ptr<org::Resource> graph;
		/** @brief A 3D texture keeps its depth in ArraySize. */
		D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
		D3D11_TEXTURE2D_DESC desc{};
		/** @brief The swizzle DXVK's views apply, for views made beside them. */
		std::int32_t components[4]{};
	};

	struct CreateInfo
	{
		D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;  // 2D, cubes included, or 3D
		UINT width = 0;
		UINT height = 0;
		UINT depthOrArraySize = 1;  // six per cube
		UINT mipLevels = 1;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT bindFlags = D3D11_BIND_SHADER_RESOURCE;
		UINT miscFlags = 0;  // only D3D11_RESOURCE_MISC_TEXTURECUBE
		/** @brief One entry per subresource, mip-major within each slice. Null for zeroed contents. */
		const D3D11_SUBRESOURCE_DATA* initialData = nullptr;
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		std::string name;
	};

	/** @brief Each field carries one reference for the engine. */
	struct Created
	{
		winrt::com_ptr<ID3D11Resource> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
	};

	/**
	* @brief The one path a texture takes onto ORG. Any thread, never waits.
	* @return False, logged once per reason, when it cannot. The caller then makes the texture as the engine would.
	*/
	bool Create(const CreateInfo& a_info, Created& a_out);

	/** @brief Also finds views made later on the texture. Any thread, and a_view must be alive. Null if none. */
	std::shared_ptr<const Texture> Find(const ID3D11ShaderResourceView* a_view);

	struct Stats
	{
		std::uint64_t created = 0;
		std::uint64_t live = 0;        // released ones wait for the GPU
		std::uint64_t handles = 0;
		std::uint64_t left = 0;        // a format, dimension or usage this does not take
		std::uint64_t failed = 0;      // failed here and fell back to the engine
	};
	Stats GetStats();
}
