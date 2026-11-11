#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/PublishedResources.h"

#	include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/GloballyIndexedResource.h>

#	include <algorithm>
#	include <atomic>
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
			std::optional<org::SRVViewType> view;  // none: the resource's primary view
			bool published = false;                // false: withdrawn, still registered
			const ID3D11ShaderResourceView* srv = nullptr;  // the twin's D3D11 export while published; compared, never used
			const GraphResources::Resource* twin = nullptr;  // read again by Refresh while alive has not expired
			std::weak_ptr<const void> alive;
		};

		/** @brief What every graph build registers. Immutable once stored. */
		struct Snapshot
		{
			std::vector<std::pair<std::string, std::shared_ptr<org::Resource>>> resources;
		};

		struct State
		{
			std::vector<Entry> entries;
			// Stays atomic although every call is on the render thread: it is cheap, and an extension made off that thread (a build
			// on another owner thread) would still read a whole snapshot.
			std::atomic<std::shared_ptr<const Snapshot>> snapshot;
		};

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
				if (!snapshot)
					return;
				for (const auto& [id, resource] : snapshot->resources)
					a_graph.RegisterResource(org::ResourceIdentifier(id), resource);
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>&) override {}

		private:
			std::shared_ptr<const Snapshot> snapshot;
		};

		/** @brief Stores the entries for the next builds and rebuilds every graph with them (adding the extension rebuilds them all). */
		void Store(State& a_state, org::GraphHost& a_host)
		{
			auto snapshot = std::make_shared<Snapshot>();
			for (const auto& entry : a_state.entries)
				snapshot->resources.emplace_back(entry.id, entry.resource);
			a_state.snapshot.store(std::move(snapshot), std::memory_order_release);
			a_host.AddExtension(kExtensionId, [&a_state]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
				return std::make_unique<PublishedExtension>(a_state.snapshot.load(std::memory_order_acquire));
			});
		}

		/** @brief The view a whole-resource read takes of a 2D texture: a cube's TextureCube, a 2D array's full array, else Texture2D. */
		std::optional<org::SRVViewType> WholeView(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			if (a_desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
				return a_desc.ArraySize > 6 ? std::nullopt : std::optional(org::SRVViewType::TextureCube);
			return a_desc.ArraySize > 1 ? std::optional(org::SRVViewType::Texture2DArrayFull) : std::optional(org::SRVViewType::Texture2D);
		}

		bool PublishAs(std::string_view a_id, const GraphResources::Resource& a_resource, std::optional<org::SRVViewType> a_view)
		{
			auto& state = Published();
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !RenderGraphRuntime::Get().IsActive()) {
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
			auto* entry = Find(state, a_id);
			if (entry && entry->resource == a_resource.Graph()) {
				entry->srv = a_resource.SRV();
				entry->twin = &a_resource;
				entry->alive = a_resource.Alive();
				if (!std::exchange(entry->published, true))
					logger::info("[ORG] '{}' published again", a_id);
				return true;
			}
			if (!entry)
				entry = &state.entries.emplace_back(Entry{ .id = std::string(a_id) });
			entry->resource = a_resource.Graph();
			entry->view = a_view;
			entry->published = true;
			entry->srv = a_resource.SRV();
			entry->twin = &a_resource;
			entry->alive = a_resource.Alive();
			Store(state, *host);
			logger::info("[ORG] '{}' published to every epoch's graph ({})", a_id, a_resource.Name());
			return true;
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

	void Refresh()
	{
		auto& state = Published();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !RenderGraphRuntime::Get().IsActive())
			return;
		bool changed = false;
		for (auto& entry : state.entries) {
			if (!entry.twin || entry.alive.expired() || !*entry.twin || entry.twin->Graph() == entry.resource)
				continue;
			entry.resource = entry.twin->Graph();
			entry.srv = entry.published ? entry.twin->SRV() : nullptr;  // a withdrawn id has no export to match
			changed = true;
			logger::info("[ORG] '{}' published again after a resize ({})", entry.id, entry.twin->Name());
		}
		if (changed)
			Store(state, *host);
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

	bool IsPublished(std::string_view a_id)
	{
		const auto* entry = Find(Published(), a_id);
		return entry && entry->published && RenderGraphRuntime::Get().IsActive();
	}

	bool IsRegistered(std::string_view a_id)
	{
		return Find(Published(), a_id) && RenderGraphRuntime::Get().IsActive();
	}

	bool IsExportOf(const ID3D11ShaderResourceView* a_view, std::string_view a_id)
	{
		if (!a_view)
			return false;
		const auto* entry = Find(Published(), a_id);
		return entry && entry->published && entry->srv == a_view;
	}

	bool ShaderResourceIndex(std::string_view a_id, std::uint32_t& a_index)
	{
		const auto* entry = Find(Published(), a_id);
		if (!entry || !entry->published || !RenderGraphRuntime::Get().IsActive())
			return false;
		const auto* indexed = dynamic_cast<const org::GloballyIndexedResource*>(entry->resource.get());
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

	std::string ExportedId(const ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return {};
		CheckThread();
		for (const auto& entry : Published().entries)
			if (entry.published && entry.srv == a_view)
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
	void Refresh() {}
	void Withdraw(std::string_view) {}
	bool IsPublished(std::string_view) { return false; }
	bool IsRegistered(std::string_view) { return false; }
	bool IsExportOf(const ID3D11ShaderResourceView*, std::string_view) { return false; }
	bool ShaderResourceIndex(std::string_view, std::uint32_t&) { return false; }
	std::string ExportedId(const ID3D11ShaderResourceView*) { return {}; }
}

#endif
