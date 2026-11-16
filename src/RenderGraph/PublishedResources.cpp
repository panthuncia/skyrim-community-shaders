#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/PublishedResources.h"

#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/UndeclaredAccess.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/GloballyIndexedResource.h>
#	include <Resources/VersionedTexture.h>

#	include <algorithm>
#	include <atomic>
#	include <mutex>
#	include <optional>
#	include <string>
#	include <vector>

namespace PublishedResources
{
	namespace
	{
		constexpr const char* kExtensionId = "cs.org.published-resources";

		struct Entry
		{
			std::string id;
			std::shared_ptr<org::Resource> resource;
			// A versioned twin's slot (GraphResources::Options::versioned): every graph registers it as the id's resolver, and a
			// resize of the twin is a new version of it, not a new resource.
			std::shared_ptr<org::VersionedTexture> slot;
			std::optional<org::SRVViewType> view;  // none: the resource's primary view
			bool published = false;                // false: withdrawn, still registered
			const ID3D11ShaderResourceView* srv = nullptr;  // the twin's D3D11 export while published; compared, never used
			const GraphResources::Resource* twin = nullptr;  // read again by Refresh while alive has not expired
			std::weak_ptr<const void> alive;
		};

		/** @brief What every graph build registers. Immutable once stored. */
		struct Snapshot
		{
			struct Registered
			{
				std::string id;
				std::shared_ptr<org::Resource> resource;
				std::shared_ptr<org::VersionedTexture> slot;  // registered as the id's resolver in place of resource
			};
			std::vector<Registered> resources;
		};

		struct State
		{
			std::vector<Entry> entries;
			// Ids a build skipped before they were published (IsDeclarable): their publish builds every graph again. Under skippedMutex:
			// builds run on the build manager's thread.
			std::vector<std::string> skipped;
			std::mutex skippedMutex;
			// Stays atomic although every call is on the render thread: it is cheap, and an extension made off that thread (a build
			// on another owner thread) would still read a whole snapshot.
			std::atomic<std::shared_ptr<const Snapshot>> snapshot;
		};

		// The snapshot the graph being built on this thread registers (PublishedExtension::PrepareForBuild, before any pass declares): what
		// IsDeclarable answers from during a build. Weak, so a thread's last build pins nothing.
		thread_local std::weak_ptr<const Snapshot> t_building;

		State& Published()
		{
			static State s_state;
			return s_state;
		}

		/**
		 * @brief Logs once when called off the render thread: the entries are not locked. That is the thread that first used them, or
		 * with CS_ORG_LOADING_THREAD=1 the render owner (FrameAnchors::IsRenderOwner), which a loading screen's thread may be.
		 */
		void CheckThread()
		{
			if (RenderGraph::Engine::LoadingScreen::Requested()) {
				static std::atomic<bool> s_offOwner{ false };
				if (!FrameAnchors::IsRenderOwner() && !s_offOwner.exchange(true, std::memory_order_relaxed))
					logger::critical("[ORG] PublishedResources used on thread {}, not the render owner: its entries are not locked", ::GetCurrentThreadId());
				return;
			}
			static std::atomic<DWORD> s_owner{ 0 };
			static std::atomic<bool> s_logged{ false };
			const DWORD self = ::GetCurrentThreadId();
			DWORD owner = 0;
			if (s_owner.compare_exchange_strong(owner, self, std::memory_order_relaxed) || owner == self)
				return;
			if (!s_logged.exchange(true, std::memory_order_relaxed))
				logger::critical("[ORG] PublishedResources used on thread {}, not the render thread {}: its entries are not locked", self, owner);
		}

		Entry* Find(State& a_state, std::string_view a_id)
		{
			CheckThread();
			const auto found = std::ranges::find(a_state.entries, a_id, &Entry::id);
			return found != a_state.entries.end() ? &*found : nullptr;
		}

		/** @brief Registers every entry, withdrawn ones too, with each graph built. */
		class PublishedExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit PublishedExtension(std::shared_ptr<const Snapshot> a_snapshot) :
				snapshot(std::move(a_snapshot)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				t_building = snapshot;
				if (!snapshot)
					return;
				for (const auto& entry : snapshot->resources) {
					const org::ResourceIdentifier id(entry.id);
					if (!entry.slot)
						a_graph.RegisterResource(id, entry.resource);
					else if (!a_graph._resolverMap.contains(id))  // the producer's own registration may have come first
						a_graph.RegisterResolver(id, entry.slot);
				}
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>&) override {}

		private:
			std::shared_ptr<const Snapshot> snapshot;
		};

		/** @brief How an entry changed: what Store rebuilds for it. */
		enum class Change
		{
			Added,     // a new id: only the graphs that looked it up need it
			Replaced,  // another resource under the id: every graph holds the old one
		};

		/**
		 * @brief Stores the entries for the next builds (a_what: the ids changed, quoted). The extension, added once, registers the latest
		 * snapshot at every build. A new id (the last entry)
		 * rebuilds only the graphs that looked it up (a reader registering with it builds its own epoch again); a replaced resource
		 * rebuilds every graph, since each holds the old one until it is built again.
		 */
		void Store(State& a_state, org::GraphHost& a_host, Change a_change, std::string_view a_what)
		{
			auto snapshot = std::make_shared<Snapshot>();
			for (const auto& entry : a_state.entries)
				snapshot->resources.push_back({ entry.id, entry.resource, entry.slot });
			a_state.snapshot.store(std::move(snapshot), std::memory_order_release);
			if (!a_host.HasExtension(kExtensionId)) {
				a_host.AddExtension(kExtensionId, [&a_state]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
					return std::make_unique<PublishedExtension>(a_state.snapshot.load(std::memory_order_acquire));
				});
				return;
			}
			const auto& added = a_state.entries.back().id;
			bool wasSkipped = false;
			if (a_change == Change::Added) {
				std::scoped_lock lock(a_state.skippedMutex);
				wasSkipped = std::erase(a_state.skipped, added) != 0;
			}
			if (wasSkipped)
				RenderGraph::Builds::RequestAll(fmt::format("published resource {} added, which a build skipped", a_what));
			else if (a_change == Change::Added)
				RenderGraph::Builds::RequestFor(added, fmt::format("published resource {} added", a_what));
			else
				GraphResources::RequestStructuralRebuild(fmt::format("published resource {} replaced by a new resource", a_what));
		}

		/** @brief The D3D11 export of an entry's twin: a versioned twin's current one (a resize makes it again), else the one stored. */
		const ID3D11ShaderResourceView* ExportOf(const Entry& a_entry)
		{
			if (!a_entry.published)
				return nullptr;
			if (a_entry.slot && a_entry.twin && !a_entry.alive.expired() && a_entry.twin->Versioned() == a_entry.slot)
				return a_entry.twin->SRV();
			return a_entry.srv;
		}

		/** @brief The resource an entry stands for now: its slot's current version, or the resource stored. */
		std::shared_ptr<org::Resource> CurrentOf(const Entry& a_entry)
		{
			return a_entry.slot ? a_entry.slot->CurrentResource() : a_entry.resource;
		}

		/** @brief The view a whole-resource read takes of a 2D texture: a cube's TextureCube, a 2D array's full array, else Texture2D. */
		std::optional<org::SRVViewType> WholeView(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			if (a_desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
				return a_desc.ArraySize > 6 ? std::nullopt : std::optional(org::SRVViewType::TextureCube);
			return a_desc.ArraySize > 1 ? std::optional(org::SRVViewType::Texture2DArrayFull) : std::optional(org::SRVViewType::Texture2D);
		}

		/** @brief What an entry is published from: a twin (Refresh follows its resizes) or a resource alone (an import). */
		struct Source
		{
			std::string_view name;
			std::shared_ptr<org::Resource> resource;
			const ID3D11ShaderResourceView* srv = nullptr;
			const GraphResources::Resource* twin = nullptr;
			std::weak_ptr<const void> alive;
			std::shared_ptr<org::VersionedTexture> slot;  // a versioned twin's slot: a new version of it is not a new resource
		};

		bool PublishEntry(std::string_view a_id, const Source& a_source, std::optional<org::SRVViewType> a_view)
		{
			auto& state = Published();
			auto* host = RenderGraphRuntime::Get().Host();
			auto* entry = Find(state, a_id);
			// The same resource, or a new version of the same slot (a versioned twin's resize): nothing to build.
			if (entry && (entry->resource == a_source.resource || (entry->slot && entry->slot == a_source.slot))) {
				entry->resource = a_source.resource;
				entry->srv = a_source.srv;
				entry->twin = a_source.twin;
				entry->alive = a_source.alive;
				if (!std::exchange(entry->published, true))
					logger::info("[ORG] '{}' published again", a_id);
				return true;
			}
			const auto change = entry ? Change::Replaced : Change::Added;
			if (!entry)
				entry = &state.entries.emplace_back(Entry{ .id = std::string(a_id) });
			entry->resource = a_source.resource;
			entry->slot = a_source.slot;
			entry->view = a_view;
			entry->published = true;
			entry->srv = a_source.srv;
			entry->twin = a_source.twin;
			entry->alive = a_source.alive;
			Store(state, *host, change, fmt::format("'{}'", a_id));
			logger::info("[ORG] '{}' published to every epoch's graph ({})", a_id, a_source.name);
			return true;
		}

		bool PublishAs(std::string_view a_id, const GraphResources::Resource& a_resource, std::optional<org::SRVViewType> a_view)
		{
			if (!RenderGraphRuntime::Get().Host() || !RenderGraphRuntime::Get().IsActive()) {
				logger::warn("[ORG] {} not published as '{}': the render graph is not active", a_resource.Name(), a_id);
				return false;
			}
			if (!a_resource || a_id.empty()) {
				logger::error("[ORG] {} not published as '{}': it has no graph resource or no id", a_resource.Name(), a_id);
				return false;
			}
			if (a_resource.Aliasable()) {
				logger::error("[ORG] {} not published as '{}': the graph may alias it (a transient, not exported), so another epoch would read nothing",
					a_resource.Name(), a_id);
				return false;
			}
			return PublishEntry(a_id, { .name = a_resource.Name(), .resource = a_resource.Graph(), .srv = a_resource.SRV(), .twin = &a_resource, .alive = a_resource.Alive(), .slot = a_resource.Versioned() },
				a_view);
		}

		/** @brief The whole-resource view of a 2D texture's SRV (ShaderResourceIndex): its own dimension's. None for another dimension. */
		std::optional<org::SRVViewType> WholeViewOf(const ID3D11ShaderResourceView* a_srv)
		{
			if (!a_srv)
				return std::nullopt;
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			const_cast<ID3D11ShaderResourceView*>(a_srv)->GetDesc(&desc);
			switch (desc.ViewDimension) {
			case D3D11_SRV_DIMENSION_TEXTURE2D:
				return org::SRVViewType::Texture2D;
			case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
				return org::SRVViewType::Texture2DArrayFull;
			case D3D11_SRV_DIMENSION_TEXTURECUBE:
				return org::SRVViewType::TextureCube;
			default:
				return std::nullopt;
			}
		}
	}

	bool Publish(std::string_view a_id, const GraphResources::Resource& a_resource)
	{
		return PublishAs(a_id, a_resource, std::nullopt);
	}

	bool Publish(std::string_view a_id, const GraphResources::Texture2D& a_texture)
	{
		return PublishAs(a_id, a_texture, WholeView(a_texture.Desc()));
	}

	bool Publish(const GraphResources::Resource& a_resource)
	{
		return Publish(a_resource.Name(), a_resource);
	}

	bool Publish(const GraphResources::Texture2D& a_texture)
	{
		return Publish(a_texture.Name(), a_texture);
	}

	bool PublishImport(std::string_view a_id, const FrameAnchors::Texture& a_import, const ID3D11ShaderResourceView* a_srv)
	{
		if (!RenderGraphRuntime::Get().Host() || !RenderGraphRuntime::Get().IsActive() || !a_import || a_id.empty()) {
			logger::warn("[ORG] import '{}' not published: {}", a_id, a_import ? "the render graph is not active" : "it has no graph resource");
			return false;
		}
		const auto view = WholeViewOf(a_srv);
		if (!view) {
			logger::error("[ORG] import '{}' not published: its view is not a 2D texture, a 2D array or a cube", a_id);
			return false;
		}
		// A slot is the id's resolver in every graph, and ShaderResourceIndex follows its current version.
		std::shared_ptr<org::Resource> resource = a_import.Slot() ? a_import.Slot()->CurrentResource() : a_import.Plain();
		return PublishEntry(a_id, { .name = a_id, .resource = std::move(resource), .srv = a_srv, .slot = a_import.Slot() }, view);
	}

	void Refresh()
	{
		auto& state = Published();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !RenderGraphRuntime::Get().IsActive())
			return;
		std::string changed;
		for (auto& entry : state.entries) {
			if (!entry.twin || entry.alive.expired() || !*entry.twin || entry.twin->Graph() == entry.resource)
				continue;
			if (entry.slot && entry.twin->Versioned() == entry.slot) {
				// A new version of the same slot: every graph already resolves it.
				entry.resource = entry.twin->Graph();
				entry.srv = entry.published ? entry.twin->SRV() : nullptr;
				continue;
			}
			entry.slot = entry.twin->Versioned();
			entry.resource = entry.twin->Graph();
			entry.srv = entry.published ? entry.twin->SRV() : nullptr;  // a withdrawn id has no export to match
			changed += fmt::format("{}'{}'", changed.empty() ? "" : ", ", entry.id);
			logger::info("[ORG] '{}' published again after a resize ({})", entry.id, entry.twin->Name());
		}
		if (!changed.empty())
			Store(state, *host, Change::Replaced, changed);
	}

	void Withdraw(std::string_view a_id)
	{
		auto* entry = Find(Published(), a_id);
		if (!entry)
			return;
		// No export to match until the next Publish.
		entry->srv = nullptr;
		if (std::exchange(entry->published, false))
			logger::info("[ORG] '{}' withdrawn: its readers return to D3D11", a_id);
	}

	namespace
	{
		/** @brief During a graph build on this thread: whether the snapshot that graph registers holds a_id. Empty outside a build. */
		std::optional<bool> InBuildingSnapshot(std::string_view a_id)
		{
			const auto snapshot = t_building.lock();
			if (!snapshot)
				return std::nullopt;
			return std::ranges::any_of(snapshot->resources, [&](const Snapshot::Registered& a_entry) { return a_entry.id == a_id; });
		}
	}

	bool IsPublished(std::string_view a_id)
	{
		// A pass made while a graph is built, possibly on the build manager's thread, reads the snapshot that graph registers, never the
		// entries the render thread changes.
		if (const auto building = InBuildingSnapshot(a_id))
			return *building && RenderGraphRuntime::Get().IsActive();
		const auto* entry = Find(Published(), a_id);
		return entry && entry->published && RenderGraphRuntime::Get().IsActive();
	}

	bool IsRegistered(std::string_view a_id)
	{
		if (const auto building = InBuildingSnapshot(a_id))
			return *building && RenderGraphRuntime::Get().IsActive();
		return Find(Published(), a_id) && RenderGraphRuntime::Get().IsActive();
	}

	bool IsDeclarable(std::string_view a_id)
	{
		// Called by passes as a graph is built, which may be on the build manager's thread: it reads the snapshot that graph registers
		// (the latest published one outside a build), never the entries the render thread changes.
		auto& state = Published();
		auto snapshot = t_building.lock();
		if (!snapshot)
			snapshot = state.snapshot.load(std::memory_order_acquire);
		if (snapshot && std::ranges::any_of(snapshot->resources, [&](const Snapshot::Registered& a_entry) { return a_entry.id == a_id; }))
			return RenderGraphRuntime::Get().IsActive();
		std::scoped_lock lock(state.skippedMutex);
		if (std::ranges::find(state.skipped, a_id) == state.skipped.end())
			state.skipped.emplace_back(a_id);
		return false;
	}

	bool IsExportOf(const ID3D11ShaderResourceView* a_view, std::string_view a_id)
	{
		if (!a_view)
			return false;
		const auto* entry = Find(Published(), a_id);
		return entry && ExportOf(*entry) == a_view;
	}

	bool ShaderResourceIndex(std::string_view a_id, std::uint32_t& a_index, std::source_location a_where)
	{
		const auto* entry = Find(Published(), a_id);
		if (!entry || !entry->published || !RenderGraphRuntime::Get().IsActive())
			return false;
		if (RenderGraph::UndeclaredAccess::On())
			RenderGraph::UndeclaredAccess::NotePublished(a_id, a_where);
		const auto current = CurrentOf(*entry);
		const auto* indexed = dynamic_cast<const org::GloballyIndexedResource*>(current.get());
		if (!indexed)
			return false;
		try {
			if (entry->view) {
				if (indexed->GetNumSRVSlices(*entry->view) == 0 || indexed->GetNumSRVMipLevels(*entry->view) == 0)
					return false;
				a_index = indexed->GetSRVInfo(*entry->view, 0, 0).slot.index;
			} else {
				// Throws for a resource without a primary view.
				if (indexed->GetNumSRVSlices() == 0 || indexed->GetNumSRVMipLevels() == 0)
					return false;
				a_index = indexed->GetSRVInfo(0).slot.index;
			}
		} catch (const std::exception&) {
			return false;
		}
		return true;
	}

	std::uint32_t ShaderResourceVariant(std::string_view a_id)
	{
		const auto* entry = Find(Published(), a_id);
		return entry && entry->view ? static_cast<std::uint32_t>(*entry->view) : UINT32_MAX;
	}

	std::string ExportedId(const ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return {};
		CheckThread();
		for (const auto& entry : Published().entries)
			if (ExportOf(entry) == a_view)
				return entry.id;
		return {};
	}
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: nothing is ever published, and every reader stays on D3D11.
#	include "RenderGraph/PublishedResources.h"

namespace PublishedResources
{
	bool Publish(std::string_view, const GraphResources::Resource&) { return false; }
	bool Publish(const GraphResources::Resource&) { return false; }
	bool Publish(std::string_view, const GraphResources::Texture2D&) { return false; }
	bool Publish(const GraphResources::Texture2D&) { return false; }
	bool PublishImport(std::string_view, const FrameAnchors::Texture&, const ID3D11ShaderResourceView*) { return false; }
	void Refresh() {}
	void Withdraw(std::string_view) {}
	bool IsPublished(std::string_view) { return false; }
	bool IsRegistered(std::string_view) { return false; }
	bool IsDeclarable(std::string_view) { return false; }
	bool IsExportOf(const ID3D11ShaderResourceView*, std::string_view) { return false; }
	bool ShaderResourceIndex(std::string_view, std::uint32_t&, std::source_location) { return false; }
	std::uint32_t ShaderResourceVariant(std::string_view) { return UINT32_MAX; }
	std::string ExportedId(const ID3D11ShaderResourceView*) { return {}; }
}

#endif
