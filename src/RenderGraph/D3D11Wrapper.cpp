#include "RenderGraph/D3D11Wrapper.h"

#include "Buffer.h"
#include "Globals.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/GraphResources.h"

#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace GraphResources
{
	namespace Detail
	{
		/** @brief One wrapper: how to point it at its twin's bound version. */
		struct WrapperEntry
		{
			std::string name;
			std::weak_ptr<const void> twinAlive;
			// Points the wrapper at the twin's version. Returns whether it changed. Only while the twin lives.
			std::function<bool()> follow;
		};
	}

	namespace
	{
		/** @brief Render thread. Weak, so a dropped registration leaves only an expired entry. */
		std::vector<std::weak_ptr<Detail::WrapperEntry>>& Entries()
		{
			static std::vector<std::weak_ptr<Detail::WrapperEntry>> s_entries;
			return s_entries;
		}

		/** @brief The wrapper's views over its current resource: the twin's exported ones, or its own of a_views. */
		template <class Wrapper, class Twin>
		void TakeViews(Wrapper& a_wrapper, const Twin& a_twin, const WrapperViews& a_views, const std::string& a_name)
		{
			try {
				if (a_views.srv)
					a_wrapper.CreateSRV(*a_views.srv);
				else
					a_wrapper.srv.copy_from(a_twin.SRV());
				if (a_views.uav)
					a_wrapper.CreateUAV(*a_views.uav);
				else
					a_wrapper.uav.copy_from(a_twin.UAV());
				if constexpr (std::is_same_v<Wrapper, ::Texture2D>) {
					if (a_views.rtv)
						a_wrapper.CreateRTV(*a_views.rtv);
					else
						a_wrapper.rtv.copy_from(a_twin.RTV());
					a_wrapper.dsv.copy_from(a_twin.DSV());
				}
			} catch (const std::exception& e) {
				RenderGraph::Fatal(a_name, fmt::format("a view could not be made on the export ({})", e.what()));
			}
		}

		template <class Wrapper, class Twin>
		D3D11Wrapper<Wrapper> Make(const Twin& a_twin, const char* a_name, const WrapperViews& a_views)
		{
			const std::string name = a_name ? std::string(a_name) : a_twin.Name();
			auto* d3d11 = a_twin.D3D11();
			if (!a_twin || !d3d11)
				RenderGraph::Fatal(name, "the twin has no D3D11 export");
			if constexpr (std::is_same_v<Wrapper, ::Texture3D>) {
				if (a_views.rtv)
					RenderGraph::Fatal(name, "a 3D wrapper has no RTV");
			}
			// The wrapper's constructor takes this reference.
			d3d11->AddRef();
			Wrapper* wrapper = nullptr;
			if constexpr (std::is_same_v<Wrapper, ::Texture3D>)
				wrapper = new ::Texture3D(d3d11);
			else
				wrapper = new ::Texture2D(d3d11, name.c_str());
			TakeViews(*wrapper, a_twin, a_views, name);

			auto entry = std::make_shared<Detail::WrapperEntry>();
			entry->name = name;
			entry->twinAlive = a_twin.Alive();
			// The twin outlives the call while twinAlive holds, so the pointer is safe there. The wrapper is touched only when the
			// twin's export changed, so a feature that dropped the wrapper of a twin that never changes is no hazard.
			entry->follow = [wrapper, twin = &a_twin, views = a_views, name, last = static_cast<const void*>(d3d11)]() mutable {
				auto* current = twin->D3D11();
				if (!current)
					RenderGraph::Fatal(name, "the twin's bound version has no D3D11 export");
				if (static_cast<const void*>(current) == last)
					return false;
				last = current;
				wrapper->resource.copy_from(current);
				current->GetDesc(&wrapper->desc);
				wrapper->srv = nullptr;
				wrapper->uav = nullptr;
				if constexpr (std::is_same_v<Wrapper, ::Texture2D>) {
					wrapper->rtv = nullptr;
					wrapper->dsv = nullptr;
				}
				TakeViews(*wrapper, *twin, views, name);
				return true;
			};
			Entries().push_back(entry);
			return D3D11Wrapper<Wrapper>(std::move(entry), wrapper);
		}
	}

	D3D11Wrapper<::Texture2D> MakeWrapper(const Texture2D& a_twin, const char* a_name, const WrapperViews& a_views)
	{
		return Make<::Texture2D>(a_twin, a_name, a_views);
	}

	D3D11Wrapper<::Texture3D> MakeWrapper(const Texture3D& a_twin, const char* a_name, const WrapperViews& a_views)
	{
		return Make<::Texture3D>(a_twin, a_name, a_views);
	}

	D3D11Wrapper<::Texture2D> MakeWrapper(const RenderGraph::Frame::DeclaredTexture<Texture2D>& a_declared, const char* a_name, const WrapperViews& a_views)
	{
		if (!a_declared)
			RenderGraph::Fatal(a_name ? a_name : "a declared texture", "the declaration has no twin");
		return Make<::Texture2D>(*a_declared, a_name, a_views);
	}

	D3D11Wrapper<::Texture3D> MakeWrapper(const RenderGraph::Frame::DeclaredTexture<Texture3D>& a_declared, const char* a_name, const WrapperViews& a_views)
	{
		if (!a_declared)
			RenderGraph::Fatal(a_name ? a_name : "a declared texture", "the declaration has no twin");
		return Make<::Texture3D>(*a_declared, a_name, a_views);
	}

	std::size_t RetargetWrappers()
	{
		std::size_t changed = 0;
		auto& entries = Entries();
		std::erase_if(entries, [&](const std::weak_ptr<Detail::WrapperEntry>& a_weak) {
			const auto entry = a_weak.lock();
			if (!entry || entry->twinAlive.expired())
				return true;
			if (entry->follow())
				++changed;
			return false;
		});
		return changed;
	}
}
