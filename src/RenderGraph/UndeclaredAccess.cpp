#include "RenderGraph/UndeclaredAccess.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/CensusWindow.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "Utils/D3D.h"

#	include <Resources/Resource.h>

#	include <d3d11.h>

#	include <algorithm>
#	include <array>
#	include <filesystem>
#	include <functional>
#	include <map>
#	include <mutex>
#	include <set>
#	include <string>
#	include <unordered_map>
#	include <vector>

namespace RenderGraph::UndeclaredAccess
{
	namespace
	{
		thread_local const char* t_epoch = nullptr;

		/** @brief Why an access is reported. */
		enum class Class : std::uint8_t
		{
			Graph,      // an ORG resource the graph manages
			Mutable,    // an engine or CS D3D11 resource that is not exempt
			Published,  // a published twin by id
			RhiBuffer,  // a BasicRHI buffer by handle
			Count
		};
		constexpr const char* kClassNames[] = { "graph-managed", "mutable D3D11 resource", "published twin", "BasicRHI buffer by handle" };

		/** @brief The resource an access names. */
		struct Subject
		{
			std::uint64_t key = 0;  // identity for the tallies (a pointer, a serial, a handle or an id's hash)
			Class type = Class::Mutable;
			std::string name;
			std::string detail;  // its description, for the first line
		};

		struct SiteResource
		{
			std::uint64_t count = 0;
			Class type = Class::Mutable;
			std::string name;
		};

		struct Site
		{
			std::string what;
			std::uint64_t count = 0;
			std::set<std::string> contexts;  // the first few
			std::unordered_map<std::uint64_t, SiteResource> resources;
		};

		struct Window
		{
			std::map<std::string, Site> sites;  // by source location
			std::uint64_t exempt = 0;           // exempt accesses skipped
			std::uint64_t unknown = 0;          // accesses to a resource nothing could describe (skipped)
		};

		struct State
		{
			std::mutex lock;
			CensusWindow::PerWindow<Window> windows;
			std::set<std::pair<std::string, std::uint64_t>> logged;  // (site, resource) first lines written
			std::uint64_t firstLines = 0;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		std::uint64_t LogLimit()
		{
			static const std::uint64_t limit = Switches::Number("CS_ORG_VALIDATE_UNDECLARED_LOG", 256);
			return limit;
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

		/** @brief A D3D11 object's debug name: a handle's, else its private data's. */
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

		/** @brief a_typed's description's bind flags and usage. */
		template <class Desc, class Typed>
		void TakeDesc(Typed* a_typed, std::uint32_t& a_bind, std::uint32_t& a_usage)
		{
			Desc desc{};
			a_typed->GetDesc(&desc);
			a_bind = desc.BindFlags;
			a_usage = static_cast<std::uint32_t>(desc.Usage);
		}

		/** @brief The D3D11 description's bind flags, usage and dimension (any mode). False when it has none. */
		bool DescriptionOf(ID3D11Resource* a_resource, std::uint32_t& a_bind, std::uint32_t& a_usage, std::uint32_t& a_dimension)
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			a_resource->GetType(&dimension);
			a_dimension = static_cast<std::uint32_t>(dimension);
			// A resource of a dimension is that interface: the casts are COM's single inheritance.
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

		/** @brief Whether the description is "immutable-looking": a texture bound SRV-only, or a buffer bound VB/IB-only. */
		bool ShapeExempt(std::uint32_t a_bind, std::uint32_t a_dimension)
		{
			if (a_dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
				constexpr std::uint32_t geometry = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
				return a_bind && (a_bind & ~geometry) == 0;
			}
			return a_bind == D3D11_BIND_SHADER_RESOURCE;
		}

		/**
		 * @brief The subject of an access to a D3D11 resource (a_object: the handle the site passed, a view or the resource), or false
		 * when it is exempt. a_unknown is set when nothing could describe it.
		 */
		bool SubjectOf(ID3D11Resource* a_resource, const void* a_object, Subject& a_out, bool& a_unknown)
		{
			a_unknown = false;
			// An ORG resource behind the handle: the graph's own rule.
			for (const void* object : { a_object, static_cast<const void*>(a_resource) }) {
				if (const auto graph = object ? Engine::Tripwire::GraphResourceOf(object) : nullptr) {
					if (!graph->IsRenderGraphManaged())
						return false;
					a_out.key = reinterpret_cast<std::uintptr_t>(graph.get());
					a_out.type = Class::Graph;
					a_out.name = graph->GetName();
					a_out.detail = "an ORG resource the graph manages";
					return true;
				}
			}
			std::uint32_t bind = 0, usage = 0, dimension = 0;
			if (Engine::Native::Active()) {
				Engine::Native::ResourceFacts facts;
				if (!Engine::Native::DescribeForCensus(a_object, facts)) {
					a_unknown = true;
					return false;
				}
				if (facts.usage == D3D11_USAGE_IMMUTABLE && ShapeExempt(facts.bindFlags, facts.dimension))
					return false;
				if (facts.initialData && !facts.written && ShapeExempt(facts.bindFlags, facts.dimension))
					return false;
				a_out.key = facts.serial;
				bind = facts.bindFlags;
				usage = facts.usage;
				dimension = facts.dimension;
				a_out.detail = fmt::format("bind {:#x}, usage {}, dimension {}, {}{}", bind, usage, dimension, facts.initialData ? "made with data" : "made without data",
					facts.written ? ", written since" : "");
			} else {
				if (!a_resource || !DescriptionOf(a_resource, bind, usage, dimension)) {
					a_unknown = true;
					return false;
				}
				// Guest mode does not know the initial data: an SRV-only or VB/IB-only DEFAULT or IMMUTABLE resource is taken as immutable.
				if ((usage == D3D11_USAGE_IMMUTABLE || usage == D3D11_USAGE_DEFAULT) && ShapeExempt(bind, dimension))
					return false;
				a_out.key = reinterpret_cast<std::uintptr_t>(a_resource);
				a_out.detail = fmt::format("bind {:#x}, usage {}, dimension {}", bind, usage, dimension);
			}
			a_out.type = Class::Mutable;
			a_out.name = NameOf(a_resource);
			if (a_out.name.empty())
				a_out.name = "(unnamed)";
			return true;
		}

		void Count(const std::source_location& a_where, std::string_view a_what, const Subject* a_subject, bool a_unknown)
		{
			auto& state = Get();
			std::lock_guard guard(state.lock);
			auto& window = state.windows.Active();
			if (!a_subject) {
				++(a_unknown ? window.unknown : window.exempt);
				return;
			}
			const std::string site = SiteOf(a_where);
			const std::string context = ContextOf();
			auto& entry = window.sites[site];
			if (!entry.count)
				entry.what = a_what;
			++entry.count;
			if (entry.contexts.size() < 4)
				entry.contexts.insert(context);
			auto& resource = entry.resources[a_subject->key];
			if (!resource.count) {
				resource.type = a_subject->type;
				resource.name = a_subject->name;
			}
			++resource.count;
			if (state.firstLines < LogLimit() && state.logged.insert({ site, a_subject->key }).second) {
				++state.firstLines;
				logger::warn(
					"[ORG] undeclared access: {} at {} reads \"{}\" ({}: {}) {}; no pass declared it, so the graph does not order it (first for this site and "
					"resource{})",
					a_what, site, a_subject->name, kClassNames[static_cast<std::size_t>(a_subject->type)], a_subject->detail, context,
					state.firstLines == LogLimit() ? "; the last such line, CS_ORG_VALIDATE_UNDECLARED_LOG; the window reports go on" : "");
			}
		}

		void Report(CensusWindow::Kind a_window, const Window& a_data, std::uint64_t a_frames)
		{
			const char* window = CensusWindow::Name(a_window);
			const double frames = static_cast<double>((std::max<std::uint64_t>)(a_frames, 1));
			std::uint64_t total = 0;
			std::array<std::size_t, static_cast<std::size_t>(Class::Count)> byClass{};
			std::set<std::uint64_t> resources;
			for (const auto& [site, entry] : a_data.sites) {
				total += entry.count;
				for (const auto& [key, resource] : entry.resources)
					if (resources.insert(key).second)
						++byClass[static_cast<std::size_t>(resource.type)];
			}
			logger::info(
				"[ORG] undeclared access ({}, {} frames): {:.2f} raw accesses a frame from {} sites to {} resources ({} graph-managed, {} mutable D3D11, {} "
				"published twins, {} BasicRHI buffers by handle); skipped: {:.2f} exempt and {:.2f} undescribed a frame",
				window, a_frames, static_cast<double>(total) / frames, a_data.sites.size(), resources.size(), byClass[0], byClass[1], byClass[2], byClass[3],
				static_cast<double>(a_data.exempt) / frames, static_cast<double>(a_data.unknown) / frames);
			std::vector<const std::pair<const std::string, Site>*> sorted;
			for (const auto& entry : a_data.sites)
				sorted.push_back(&entry);
			std::ranges::sort(sorted, [](const auto* a, const auto* b) { return a->second.count > b->second.count; });
			const std::size_t top = CensusWindow::ReportTop();
			for (std::size_t i = 0; i < sorted.size() && i < top; ++i) {
				const auto& [site, entry] = *sorted[i];
				std::vector<const SiteResource*> named;
				for (const auto& [key, resource] : entry.resources)
					named.push_back(&resource);
				std::ranges::sort(named, [](const auto* a, const auto* b) { return a->count > b->count; });
				std::string most;
				for (std::size_t r = 0; r < named.size() && r < 4; ++r)
					most += fmt::format("{}\"{}\" ({}) {:.2f}", r ? ", " : "", named[r]->name, kClassNames[static_cast<std::size_t>(named[r]->type)],
						static_cast<double>(named[r]->count) / frames);
				std::string contexts;
				for (const auto& context : entry.contexts)
					contexts += fmt::format("{}{}", contexts.empty() ? "" : "; ", context);
				logger::info("[ORG] undeclared access ({}): site #{} {} ({}): {:.2f} a frame to {} resources, {}; most: {}", window, i + 1, site, entry.what,
					static_cast<double>(entry.count) / frames, entry.resources.size(), contexts, most);
			}
			if (sorted.size() > top)
				logger::info("[ORG] undeclared access ({}): {} more sites", window, sorted.size() - top);
		}
	}

	void NoteView(ID3D11ShaderResourceView* a_view, std::string_view a_what, std::source_location a_where)
	{
		if (!a_view)
			return;
		winrt::com_ptr<ID3D11Resource> resource;
		// A native view keeps its resource (no tripwire call); DXVK's answers it as D3D11 does.
		a_view->GetResource(resource.put());
		Subject subject;
		bool unknown = false;
		const bool report = SubjectOf(resource.get(), a_view, subject, unknown);
		Count(a_where, a_what, report ? &subject : nullptr, unknown);
	}

	void NoteBuffer(ID3D11Buffer* a_buffer, std::string_view a_what, std::source_location a_where)
	{
		if (!a_buffer)
			return;
		Subject subject;
		bool unknown = false;
		const bool report = SubjectOf(a_buffer, a_buffer, subject, unknown);
		Count(a_where, a_what, report ? &subject : nullptr, unknown);
	}

	void NoteResource(const org::Resource& a_resource, std::string_view a_what, std::source_location a_where)
	{
		if (!a_resource.IsRenderGraphManaged()) {
			Count(a_where, a_what, nullptr, false);
			return;
		}
		Subject subject;
		subject.key = reinterpret_cast<std::uintptr_t>(&a_resource);
		subject.type = Class::Graph;
		subject.name = a_resource.GetName();
		subject.detail = "an ORG resource the graph manages";
		Count(a_where, a_what, &subject, false);
	}

	void NotePublished(std::string_view a_id, std::source_location a_where)
	{
		Subject subject;
		subject.key = std::hash<std::string_view>{}(a_id);
		subject.type = Class::Published;
		subject.name = a_id;
		subject.detail = "its index from ShaderResourceIndex, not from a declared view";
		Count(a_where, "ShaderResourceIndex", &subject, false);
	}

	void NoteRhiBuffer(std::uint32_t a_index, std::uint32_t a_generation, std::source_location a_where)
	{
		Subject subject;
		subject.key = (std::uint64_t{ a_index } << 32) | a_generation;
		subject.type = Class::RhiBuffer;
		subject.name = fmt::format("BasicRHI buffer {}.{}", a_index, a_generation);
		subject.detail = "a ref by handle; the graph cannot tell which resource or pass it belongs to";
		Count(a_where, "BufferRefOf", &subject, false);
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

	void EndFrame()
	{
		if (!On())
			return;
		auto& state = Get();
		std::lock_guard guard(state.lock);
		state.windows.EndFrame(CensusWindow::ReportFrames(), [](CensusWindow::Kind a_window, const Window& a_data, std::uint64_t a_frames) { Report(a_window, a_data, a_frames); });
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::UndeclaredAccess
{
	void NoteView(ID3D11ShaderResourceView*, std::string_view, std::source_location) {}
	void NoteBuffer(ID3D11Buffer*, std::string_view, std::source_location) {}
	void NoteResource(const org::Resource&, std::string_view, std::source_location) {}
	void NotePublished(std::string_view, std::source_location) {}
	void NoteRhiBuffer(std::uint32_t, std::uint32_t, std::source_location) {}
	EpochScope::EpochScope(const char*) {}
	EpochScope::~EpochScope() {}
	void EndFrame() {}
}

#endif
