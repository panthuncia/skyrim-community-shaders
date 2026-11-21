#pragma once

#include <d3d11.h>

#include <cstddef>
#include <memory>
#include <optional>

class Texture2D;
class Texture3D;

namespace GraphResources
{
	class Texture2D;
	class Texture3D;
}

namespace RenderGraph::Frame
{
	template <class Twin>
	class DeclaredTexture;
}

/**
* @brief A feature's Buffer.h texture over a twin's D3D11 export, re-pointed when the twin's version changes (cleanup plan B15).
*
* A feature that still has D3D11 readers holds a ::Texture2D or ::Texture3D. MakeWrapper makes one over the twin's export. At
* every revision adoption, after the twins adopt their new versions, RetargetWrappers points each live wrapper's texture, its
* description and its views at the twin's bound version. So the wrapper object stays the same, and its D3D11 objects change only
* at the adoption point. Readers that keep a raw view pointer across frames must read it again after an adoption.
*
* The objects are plain D3D11 objects (DXVK's in guest mode, native handles in native mode), not tripwire handles, so CS's own
* D3D11 code binds them on the raw context as before.
*
* The feature owns the ::Texture2D or ::Texture3D. The D3D11Wrapper is its registration. The object is touched only when the twin's
* export changes, so it must live while the twin can get a new version. Dropping the registration stops the re-pointing. A
* registration whose twin is gone is dropped at the next adoption.
*
* Render thread only.
*/
namespace GraphResources
{
	namespace Detail
	{
		struct WrapperEntry;
	}

	/**
	* @brief Views a wrapper makes itself over each version, with its own descriptions. Unset: the twin's exported view. A 2D
	* wrapper always takes the twin's exported DSV.
	*/
	struct WrapperViews
	{
		std::optional<D3D11_SHADER_RESOURCE_VIEW_DESC> srv;
		std::optional<D3D11_UNORDERED_ACCESS_VIEW_DESC> uav;
		std::optional<D3D11_RENDER_TARGET_VIEW_DESC> rtv;
	};

	/** @brief The registration of one wrapper. Copies share it: the last one dropped stops the re-pointing. */
	template <class T>
	class D3D11Wrapper
	{
	public:
		D3D11Wrapper() = default;
		explicit D3D11Wrapper(std::shared_ptr<Detail::WrapperEntry> a_entry, T* a_wrapper) :
			entry(std::move(a_entry)), wrapper(a_wrapper) {}

		/** @brief The feature's object. The feature owns it. */
		T* Get() const { return wrapper; }
		explicit operator bool() const { return wrapper != nullptr; }

	private:
		std::shared_ptr<Detail::WrapperEntry> entry;  // the registry holds it weakly
		T* wrapper = nullptr;
	};

	/**
	* @brief Render thread. A new ::Texture2D over a_twin's export, named a_name, with the twin's exported views or the ones
	* a_views describes. The caller takes the object. Fatal when the twin has no export or a view cannot be made.
	*/
	D3D11Wrapper<::Texture2D> MakeWrapper(const Texture2D& a_twin, const char* a_name, const WrapperViews& a_views = {});

	/** @brief As above, for a 3D twin. A 3D wrapper has no RTV. */
	D3D11Wrapper<::Texture3D> MakeWrapper(const Texture3D& a_twin, const char* a_name, const WrapperViews& a_views = {});

	/** @brief MakeWrapper on a declared texture's twin. */
	D3D11Wrapper<::Texture2D> MakeWrapper(const RenderGraph::Frame::DeclaredTexture<Texture2D>& a_declared, const char* a_name, const WrapperViews& a_views = {});
	D3D11Wrapper<::Texture3D> MakeWrapper(const RenderGraph::Frame::DeclaredTexture<Texture3D>& a_declared, const char* a_name, const WrapperViews& a_views = {});

	/**
	* @brief Render owner, inside the adoption point, after the twins adopted their prepared versions (Frame/Revision.cpp). Points
	* every live wrapper at its twin's bound version. Returns how many changed.
	*/
	std::size_t RetargetWrappers();
}
