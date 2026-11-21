#include "RenderGraph/UndeclaredAccess.h"
#include "RenderGraph/Engine/Engine.h"

#include <mutex>
#include <set>

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "Utils/D3D.h"

#include <Resources/Resource.h>

#include <d3d11.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace RenderGraph::UndeclaredAccess
{
	namespace
	{
		thread_local const char* t_epoch = nullptr;
		thread_local bool t_declaredIndex = false;
		thread_local bool t_declaredView = false;

		/** @brief What an access reads, for the tap error. */
		struct Subject
		{
			const char* type = "mutable D3D11 resource";
			std::string name;
			std::string detail;
		};

		enum class Outcome : std::uint8_t
		{
			Report,   // an undeclared access
			Exempt,   // immutable by its description, ORG's ownership or its creator's note, or a host feedback buffer
			Unknown,  // nothing could describe it
		};

		/** @brief ORG resources noted by their creators, held weakly so a new resource at the same address is not taken for one. */
		struct NotedRegistry
		{
			std::mutex lock;
			std::unordered_map<const org::Resource*, std::weak_ptr<org::Resource>> resources;
			std::size_t pruneAt = 1024;
		};

		NotedRegistry& Immutables()
		{
			static NotedRegistry registry;
			return registry;
		}

		NotedRegistry& Feedbacks()
		{
			static NotedRegistry registry;
			return registry;
		}

		bool Noted(NotedRegistry& a_registry, const org::Resource* a_resource)
		{
			std::lock_guard guard(a_registry.lock);
			const auto found = a_registry.resources.find(a_resource);
			if (found == a_registry.resources.end())
				return false;
			const auto alive = found->second.lock();
			if (alive.get() == a_resource)
				return true;
			a_registry.resources.erase(found);
			return false;
		}

		bool NotedImmutable(const org::Resource* a_resource) { return Noted(Immutables(), a_resource); }

		void Note(NotedRegistry& a_registry, const std::shared_ptr<org::Resource>& a_resource)
		{
			if (!a_resource)
				return;
			std::lock_guard guard(a_registry.lock);
			a_registry.resources[a_resource.get()] = a_resource;
			if (a_registry.resources.size() >= a_registry.pruneAt) {
				std::erase_if(a_registry.resources, [](const auto& a_entry) { return a_entry.second.expired(); });
				a_registry.pruneAt = (std::max<std::size_t>)(1024, a_registry.resources.size() * 2);
			}
		}

		/** @brief Whether native mode's immediate context wrote a_object's resource since it was made. */
		bool WrittenSince(const void* a_object)
		{
			if (!a_object || !Engine::Native::Active())
				return false;
			Engine::Native::ResourceFacts facts;
			return Engine::Native::DescribeFacts(a_object, facts) && facts.written;
		}

		std::string SiteOf(const std::source_location& a_where)
		{
			return fmt::format("{}:{}", std::filesystem::path(a_where.file_name()).filename().string(), a_where.line());
		}

		std::string ContextOf()
		{
			if (t_epoch)
				return fmt::format("in epoch \"{}\"", t_epoch);
			if (FrameAnchors::IsRenderOwner())
				return "at capture (render thread, outside any epoch)";
			return "on another thread (scene workers, the import thread or the graph host's recording)";
		}

		std::string NameOf(ID3D11DeviceChild* a_object)
		{
			if (!a_object)
				return {};
			if (Engine::Tripwire::IsHandle(a_object))
				return Engine::Tripwire::Describe(a_object).name;
			char buffer[256]{};
			UINT size = sizeof(buffer) - 1;
			if (a_object->GetPrivateData(Util::WKPDID_D3DDebugObjectNameT, &size, buffer) == S_OK)
				return std::string(buffer, size);
			return {};
		}

		template <class Desc, class Typed>
		void TakeDesc(Typed* a_typed, std::uint32_t& a_bind, std::uint32_t& a_usage)
		{
			Desc desc{};
			a_typed->GetDesc(&desc);
			a_bind = desc.BindFlags;
			a_usage = static_cast<std::uint32_t>(desc.Usage);
		}

		/** @brief False when the resource has no D3D11 description. */
		bool DescriptionOf(ID3D11Resource* a_resource, std::uint32_t& a_bind, std::uint32_t& a_usage, std::uint32_t& a_dimension)
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			a_resource->GetType(&dimension);
			a_dimension = static_cast<std::uint32_t>(dimension);
			switch (dimension) {
			case D3D11_RESOURCE_DIMENSION_BUFFER:
				TakeDesc<D3D11_BUFFER_DESC>(static_cast<ID3D11Buffer*>(a_resource), a_bind, a_usage);
				return true;
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				TakeDesc<D3D11_TEXTURE1D_DESC>(static_cast<ID3D11Texture1D*>(a_resource), a_bind, a_usage);
				return true;
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				TakeDesc<D3D11_TEXTURE2D_DESC>(static_cast<ID3D11Texture2D*>(a_resource), a_bind, a_usage);
				return true;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				TakeDesc<D3D11_TEXTURE3D_DESC>(static_cast<ID3D11Texture3D*>(a_resource), a_bind, a_usage);
				return true;
			default:
				return false;
			}
		}

		/** @brief Whether the description looks immutable, a texture bound SRV-only or a buffer bound VB/IB-only. */
		bool ShapeExempt(std::uint32_t a_bind, std::uint32_t a_dimension)
		{
			if (a_dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
				constexpr std::uint32_t geometry = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
				return a_bind && (a_bind & ~geometry) == 0;
			}
			return a_bind == D3D11_BIND_SHADER_RESOURCE;
		}

		/** @brief The subject of an access to a D3D11 resource, or why it is skipped. */
		Outcome SubjectOf(ID3D11Resource* a_resource, const void* a_object, Subject& a_out)
		{
			// For an ORG resource, the graph's rule first, then its creator's note. A write since it was made overrides the note.
			for (const void* object : { a_object, static_cast<const void*>(a_resource) }) {
				if (const auto graph = object ? Engine::Tripwire::GraphResourceOf(object) : nullptr) {
					if (!graph->IsRenderGraphManaged())
						return Outcome::Exempt;
					const bool written = WrittenSince(a_object) || (a_resource != a_object && WrittenSince(a_resource));
					if (!written && NotedImmutable(graph.get()))
						return Outcome::Exempt;
					a_out.type = "graph-managed";
					a_out.name = graph->GetName();
					a_out.detail = written ? "an ORG resource the graph manages, written since it was made" : "an ORG resource the graph manages";
					return Outcome::Report;
				}
			}
			std::uint32_t bind = 0, usage = 0, dimension = 0;
			if (Engine::Native::Active()) {
				Engine::Native::ResourceFacts facts;
				if (!Engine::Native::DescribeFacts(a_object, facts))
					return Outcome::Unknown;
				// D3D11 refuses every write to an IMMUTABLE resource, whatever its binding.
				if (facts.usage == D3D11_USAGE_IMMUTABLE)
					return Outcome::Exempt;
				if (facts.initialData && !facts.written && ShapeExempt(facts.bindFlags, facts.dimension))
					return Outcome::Exempt;
				a_out.detail = fmt::format("bind {:#x}, usage {}, dimension {}, {}{}", facts.bindFlags, facts.usage, facts.dimension,
					facts.initialData ? "made with data" : "made without data", facts.written ? ", written since" : "");
			} else {
				if (!a_resource || !DescriptionOf(a_resource, bind, usage, dimension))
					return Outcome::Unknown;
				// Guest mode does not know the initial data, so an SRV-only or VB/IB-only DEFAULT resource is taken as immutable.
				if (usage == D3D11_USAGE_IMMUTABLE || (usage == D3D11_USAGE_DEFAULT && ShapeExempt(bind, dimension)))
					return Outcome::Exempt;
				a_out.detail = fmt::format("bind {:#x}, usage {}, dimension {}", bind, usage, dimension);
			}
			a_out.name = NameOf(a_resource);
			if (a_out.name.empty())
				a_out.name = "(unnamed)";
			return Outcome::Report;
		}

		/**
		 * @brief An undeclared access, warned once per site and resource. Menu and loading-screen textures that the immediate context
		 * writes still reach passes this way, so it is not a tap error yet.
		 */
		void Raise(const std::source_location& a_where, std::string_view a_what, const Subject& a_subject)
		{
			static std::mutex s_mutex;
			static std::set<std::string> s_logged;
			const auto site = SiteOf(a_where);
			{
				std::scoped_lock lock(s_mutex);
				if (!s_logged.insert(fmt::format("{}|{}", site, a_subject.name)).second)
					return;
			}
			logger::warn("[ORG] undeclared access: {} at {} reads \"{}\" ({}: {}) {}; no pass declared it (logged once per site and resource)", a_what, site,
				a_subject.name, a_subject.type, a_subject.detail, ContextOf());
		}
	}

	bool On()
	{
#if defined(CS_RELEASE_PROFILE)
		return false;
#else
		static const bool on = Engine::GraphMode();
		return on;
#endif
	}

	void NoteView(ID3D11ShaderResourceView* a_view, std::string_view a_what, std::source_location a_where)
	{
		// Inside a DeclaredViewScope the raw index is only the fallback until the build that declares the read is attached.
		if (!a_view || t_declaredView)
			return;
		winrt::com_ptr<ID3D11Resource> resource;
		// A native view keeps its resource with no tripwire call. DXVK's answers as D3D11 does.
		a_view->GetResource(resource.put());
		Subject subject;
		if (SubjectOf(resource.get(), a_view, subject) == Outcome::Report)
			Raise(a_where, a_what, subject);
	}

	void NoteBuffer(ID3D11Buffer* a_buffer, std::string_view a_what, std::source_location a_where)
	{
		if (!a_buffer)
			return;
		Subject subject;
		if (SubjectOf(a_buffer, a_buffer, subject) == Outcome::Report)
			Raise(a_where, a_what, subject);
	}

	void NoteResource(const org::Resource& a_resource, std::string_view a_what, std::source_location a_where)
	{
		if (!a_resource.IsRenderGraphManaged() || NotedImmutable(&a_resource))
			return;
		// No GPU pass reads a host feedback buffer, so there is nothing for a declaration to order.
		if (Noted(Feedbacks(), &a_resource))
			return;
		Raise(a_where, a_what, { "graph-managed", a_resource.GetName(), "an ORG resource the graph manages" });
	}

	void NotePublished(std::string_view a_id, std::source_location a_where)
	{
		// The graph already orders a declared read. Only the index is not the declared view's yet.
		if (t_declaredIndex)
			return;
		Raise(a_where, "ShaderResourceIndex", { "published twin", std::string(a_id), "its index from ShaderResourceIndex, not from a declared view" });
	}

	void NoteRhiBuffer(std::uint32_t a_index, std::uint32_t a_generation, std::source_location a_where)
	{
		Raise(a_where, "BufferRefOf",
			{ "BasicRHI buffer by handle", fmt::format("BasicRHI buffer {}.{}", a_index, a_generation),
				"a ref by handle; the graph cannot tell which resource or pass it belongs to" });
	}

	void NoteImmutable(const std::shared_ptr<org::Resource>& a_resource)
	{
		Note(Immutables(), a_resource);
	}

	void NoteHostFeedback(const std::shared_ptr<org::Resource>& a_resource)
	{
		Note(Feedbacks(), a_resource);
	}

	DeclaredIndexScope::DeclaredIndexScope() :
		previous(t_declaredIndex)
	{
		t_declaredIndex = true;
	}

	DeclaredIndexScope::~DeclaredIndexScope()
	{
		t_declaredIndex = previous;
	}

	DeclaredViewScope::DeclaredViewScope() :
		previous(t_declaredView)
	{
		t_declaredView = true;
	}

	DeclaredViewScope::~DeclaredViewScope()
	{
		t_declaredView = previous;
	}

	EpochScope::EpochScope(const char* a_label) :
		previous(t_epoch)
	{
		t_epoch = a_label;
	}

	EpochScope::~EpochScope()
	{
		t_epoch = previous;
	}
}

