// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/PublishedResources.h"

#include "RenderGraph/BuildManager.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/Buffers/VersionedBuffer.h>
#include <Resources/GloballyIndexedResource.h>
#include <Resources/VersionedTexture.h>

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace PublishedResources
{
	namespace
	{
		constexpr const char* kExtensionId = "cs.org.published-resources";

		struct Entry
		{
			std::string id;
			// A plain resource. Null for a slot, whose versions the entry never holds: a version held here would outlive its
			// replacement for the session.
			std::shared_ptr<org::Resource> resource;
			// A versioned twin's slot. Every graph registers it as the id's resolver, so a resize is a new version, not a new resource.
			std::shared_ptr<org::VersionedTexture> slot;
			std::shared_ptr<org::VersionedBuffer> bufferSlot;  // the same for a declared buffer (Frame::DeclareBuffer)
			std::optional<org::SRVViewType> view;  // none means the primary view
			bool published = false;
			const ID3D11ShaderResourceView* srv = nullptr;  // compared, never used
			const GraphResources::Resource* twin = nullptr;  // its export is read through it while alive has not expired
			std::weak_ptr<const void> alive;
		};

		/** @brief What every graph build registers. Immutable once stored. */
		struct Snapshot
		{
			struct Registered
			{
				std::string id;
				std::shared_ptr<org::Resource> resource;      // null for a slot, as Entry's
				std::shared_ptr<org::VersionedTexture> slot;  // registered as the id's resolver in place of resource
				std::shared_ptr<org::VersionedBuffer> bufferSlot;  // the same for a buffer
				// Read by a build's ShaderResourceVariant and ShaderResourceIndex, which may run off the render thread.
				std::optional<org::SRVViewType> view;
				bool published = false;
			};
			std::vector<Registered> resources;
			// Each id's position in resources. Its keys point into resources, which never changes once stored.
			ankerl::unordered_dense::map<std::string_view, std::uint32_t> index;

			const Registered* Find(std::string_view a_id) const
			{
				const auto found = index.find(a_id);
				return found != index.end() ? &resources[found->second] : nullptr;
			}
		};

		/** @brief A string map key that finds by string_view without a copy. */
		struct IdHash
		{
			using is_transparent = void;
			using is_avalanching = void;
			std::uint64_t operator()(std::string_view a_id) const noexcept { return ankerl::unordered_dense::hash<std::string_view>{}(a_id); }
		};

		struct State
		{
			std::vector<Entry> entries;
			// Each entry's position. Entries are only appended, so a position stays valid.
			ankerl::unordered_dense::map<std::string, std::uint32_t, IdHash, std::equal_to<>> entryIndex;
			// Ids a build skipped before they were published. Their publish rebuilds every graph. Locked, since builds run on another thread.
			std::vector<std::string> skipped;
			std::mutex skippedMutex;
			// Atomic so an extension made off the render thread still reads a whole snapshot.
			std::atomic<std::shared_ptr<const Snapshot>> snapshot;
			// Bumped after each snapshot store, so a thread can keep its last load while it holds.
			std::atomic<std::uint64_t> snapshotGeneration{ 0 };
			// Bumped by every change to an entry (PublishEntry). Render owner.
			std::uint64_t entriesSerial = 0;
			// Which entry exports a view, for ExportedId and IsExport: they are asked per draw and per texture slot. A versioned
			// twin's export changes only when a version is adopted, at a frame's Begin, so the answers hold for one frame and one
			// entriesSerial. Render owner.
			struct ExportAnswer
			{
				std::uint32_t first = UINT32_MAX;  // the first entry whose export it is
				std::uint32_t named = UINT32_MAX;  // the first such entry with an id
			};
			ankerl::unordered_dense::map<const ID3D11ShaderResourceView*, ExportAnswer> exportAnswers;
			std::uint64_t exportAnswersFrame = ~0ull;
			std::uint64_t exportAnswersSerial = ~0ull;
		};

		// The snapshot the graph being built on this thread registers. Weak, so a thread's last build pins nothing.
		thread_local std::weak_ptr<const Snapshot> t_building;
		// This thread built a graph. Outside a build it reads the latest snapshot, never the live entries, which are the render
		// thread's. The render thread reads the latest snapshot too once it has built one, and it holds every publish.
		thread_local bool t_buildThread = false;

		State& Published()
		{
			static State s_state;
			return s_state;
		}

		/** @brief The snapshot this thread's lookups read: the build's, or the latest on a thread that built. Null: the live entries. */
		std::shared_ptr<const Snapshot> ThreadSnapshot()
		{
			if (auto building = t_building.lock())
				return building;
			if (!t_buildThread)
				return nullptr;
			// The atomic load locks, and the frame textures ask hundreds of times a frame. Keep the last one while it is current.
			thread_local std::uint64_t t_generation = ~0ull;
			thread_local std::shared_ptr<const Snapshot> t_latest;
			auto& state = Published();
			const std::uint64_t generation = state.snapshotGeneration.load(std::memory_order_acquire);
			if (generation != t_generation) {
				t_latest = state.snapshot.load(std::memory_order_acquire);
				t_generation = generation;
			}
			return t_latest;
		}

		/** @brief Logs once when called off the owning thread, since the entries are not locked. A loading screen's thread may own them. */
		void CheckThread()
		{
			if (RenderGraph::Engine::GraphMode()) {
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
			const auto found = a_state.entryIndex.find(a_id);
			return found != a_state.entryIndex.end() ? &a_state.entries[found->second] : nullptr;
		}

		/** @brief Registers every entry with each graph built. */
		class PublishedExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit PublishedExtension(std::shared_ptr<const Snapshot> a_snapshot) :
				snapshot(std::move(a_snapshot)) {}

			// A build's snapshot ends with the build: a thread that kept it would never see a later publish.
			void FinishBuild(org::RenderGraph&) override
			{
				t_building.reset();
				t_buildThread = true;
			}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				t_building = snapshot;
				if (!snapshot)
					return;
				for (const auto& entry : snapshot->resources) {
					const org::ResourceIdentifier id(entry.id);
					if (!entry.slot && !entry.bufferSlot)
						a_graph.RegisterResource(id, entry.resource);
					else if (a_graph._resolverMap.contains(id))  // the producer's own registration may have come first
						continue;
					else if (entry.slot)
						a_graph.RegisterResolver(id, entry.slot);
					else
						a_graph.RegisterResolver(id, entry.bufferSlot);
				}
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>&) override {}

		private:
			std::shared_ptr<const Snapshot> snapshot;
		};

		/** @brief Stores the entries for the next builds, and requests none. */
		void StoreSnapshot(State& a_state)
		{
			auto snapshot = std::make_shared<Snapshot>();
			for (const auto& entry : a_state.entries)
				snapshot->resources.push_back({ entry.id, entry.resource, entry.slot, entry.bufferSlot, entry.view, entry.published });
			snapshot->index.reserve(snapshot->resources.size());
			for (std::uint32_t i = 0; i < snapshot->resources.size(); ++i)
				snapshot->index.try_emplace(snapshot->resources[i].id, i);
			a_state.snapshot.store(std::move(snapshot), std::memory_order_release);
			a_state.snapshotGeneration.fetch_add(1, std::memory_order_acq_rel);
		}

		/** @brief Stores the entries for the next builds. A new id rebuilds only the graphs that looked it up, never a running one's shape. */
		void Store(State& a_state, org::GraphHost& a_host, std::string_view a_what)
		{
			StoreSnapshot(a_state);
			if (!a_host.HasExtension(kExtensionId)) {
				a_host.AddExtension(kExtensionId, [&a_state]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
					return std::make_unique<PublishedExtension>(a_state.snapshot.load(std::memory_order_acquire));
				});
				return;
			}
			const auto& added = a_state.entries.back().id;
			bool wasSkipped = false;
			{
				std::scoped_lock lock(a_state.skippedMutex);
				wasSkipped = std::erase(a_state.skipped, added) != 0;
			}
			// Every request waits for the next adoption point. An added id changes nothing a running recording binds.
			if (wasSkipped)
				RenderGraph::Builds::Ask({ .all = true }, fmt::format("published resource {} added, which a build skipped", a_what));
			else
				RenderGraph::Builds::Ask({ .ids = { added } }, fmt::format("published resource {} added", a_what));
		}

		/** @brief The D3D11 export of an entry's twin. A versioned twin's export changes with each adopted version. */
		const ID3D11ShaderResourceView* ExportOf(const Entry& a_entry)
		{
			if (!a_entry.published)
				return nullptr;
			if (a_entry.slot && a_entry.twin && !a_entry.alive.expired() && a_entry.twin->Versioned() == a_entry.slot)
				return a_entry.twin->SRV();
			if (a_entry.bufferSlot && a_entry.twin && !a_entry.alive.expired() && a_entry.twin->BufferVersions() == a_entry.bufferSlot)
				return a_entry.twin->SRV();
			return a_entry.srv;
		}

		std::shared_ptr<org::Resource> CurrentOf(const Entry& a_entry)
		{
			if (a_entry.slot)
				return a_entry.slot->CurrentResource();
			if (a_entry.bufferSlot)
				return a_entry.bufferSlot->Get();
			return a_entry.resource;
		}

		/** @brief A cube gets TextureCube, a 2D array its full array, else Texture2D. */
		std::optional<org::SRVViewType> WholeView(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			if (a_desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
				return a_desc.ArraySize > 6 ? std::nullopt : std::optional(org::SRVViewType::TextureCube);
			return a_desc.ArraySize > 1 ? std::optional(org::SRVViewType::Texture2DArrayFull) : std::optional(org::SRVViewType::Texture2D);
		}

		/** @brief What an entry is published from, either a twin, whose new versions it follows, or an import. */
		struct Source
		{
			std::string_view name;
			std::shared_ptr<org::Resource> resource;
			const ID3D11ShaderResourceView* srv = nullptr;
			const GraphResources::Resource* twin = nullptr;
			std::weak_ptr<const void> alive;
			std::shared_ptr<org::VersionedTexture> slot;  // a new version of it is not a new resource
			std::shared_ptr<org::VersionedBuffer> bufferSlot;
		};

		bool PublishEntry(std::string_view a_id, const Source& a_source, std::optional<org::SRVViewType> a_view)
		{
			auto& state = Published();
			auto* host = RenderGraphRuntime::Get().Host();
			auto* entry = Find(state, a_id);
			// A slot's entry holds no version. Its readers follow the slot (CurrentOf).
			auto resource = a_source.slot || a_source.bufferSlot ? nullptr : a_source.resource;
			// The same resource or a new version of the same slot needs no build.
			if (entry && ((entry->resource && entry->resource == resource) || (entry->slot && entry->slot == a_source.slot) ||
							 (entry->bufferSlot && entry->bufferSlot == a_source.bufferSlot))) {
				// A publish of what the entry already holds changes no answer, so lookups keep theirs.
				const bool sameAlive = !entry->alive.owner_before(a_source.alive) && !a_source.alive.owner_before(entry->alive);
				if (entry->resource != resource || entry->srv != a_source.srv || entry->twin != a_source.twin || !sameAlive || !entry->published)
					++state.entriesSerial;
				entry->resource = std::move(resource);
				entry->srv = a_source.srv;
				entry->twin = a_source.twin;
				entry->alive = a_source.alive;
				if (!std::exchange(entry->published, true)) {
					StoreSnapshot(state);
					logger::info("[ORG] '{}' published again", a_id);
				}
				return true;
			}
			// Another resource under a published id would rebuild every graph that holds it, and its readers would bind two
			// resources across one build. Declare the id once and let its versions follow the settings.
			if (entry) {
				const auto held = CurrentOf(*entry);
				RenderGraph::Fatal("published resources", fmt::format("'{}' is already published as {}; {} cannot replace it (declare the id once, at setup)", a_id,
					held ? held->GetName() : std::string("another resource"), a_source.name));
			}
			++state.entriesSerial;
			entry = &state.entries.emplace_back(Entry{ .id = std::string(a_id) });
			state.entryIndex.try_emplace(entry->id, static_cast<std::uint32_t>(state.entries.size() - 1));
			entry->resource = std::move(resource);
			entry->slot = a_source.slot;
			entry->bufferSlot = a_source.bufferSlot;
			entry->view = a_view;
			entry->published = true;
			entry->srv = a_source.srv;
			entry->twin = a_source.twin;
			entry->alive = a_source.alive;
			Store(state, *host, fmt::format("'{}'", a_id));
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
			return PublishEntry(a_id,
				{ .name = a_resource.Name(), .resource = a_resource.Graph(), .srv = a_resource.SRV(), .twin = &a_resource, .alive = a_resource.Alive(), .slot = a_resource.Versioned(),
					.bufferSlot = a_resource.BufferVersions() },
				a_view);
		}

		/** @brief The whole-resource view matching a 2D SRV's own dimension. None for another dimension. */
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

	namespace
	{
		/** @brief During a graph build on this thread, whether its snapshot holds a_id. Empty outside a build. */
		std::optional<bool> InBuildingSnapshot(std::string_view a_id)
		{
			const auto snapshot = ThreadSnapshot();
			if (!snapshot)
				return std::nullopt;
			return snapshot->Find(a_id) != nullptr;
		}

		/** @brief What ShaderResourceIndex and ShaderResourceVariant read for one id. */
		struct IndexSource
		{
			std::shared_ptr<org::Resource> current;
			std::optional<org::SRVViewType> view;
			bool found = false;
			bool published = false;
		};

		/** @brief During a build, from the snapshot that graph registers. Otherwise from the live entries, on the render thread. */
		IndexSource IndexSourceOf(std::string_view a_id)
		{
			if (const auto snapshot = ThreadSnapshot()) {
				const auto* found = snapshot->Find(a_id);
				if (!found)
					return {};
				std::shared_ptr<org::Resource> current = found->resource;
				if (found->slot)
					current = found->slot->CurrentResource();
				else if (found->bufferSlot)
					current = found->bufferSlot->Get();
				return { std::move(current), found->view, true, found->published };
			}
			const auto* entry = Find(Published(), a_id);
			if (!entry)
				return {};
			return { CurrentOf(*entry), entry->view, true, entry->published };
		}
	}

	bool IsPublished(std::string_view a_id)
	{
		// During a build, possibly on another thread, read the snapshot that graph registers, never the live entries.
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
		// Builds may run on another thread, so read the building snapshot or the latest one, never the live entries.
		auto& state = Published();
		auto snapshot = t_building.lock();
		if (!snapshot)
			snapshot = state.snapshot.load(std::memory_order_acquire);
		if (snapshot && snapshot->Find(a_id))
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
		// A pass's Declare reads it during a build, which may run on the build thread.
		const auto source = IndexSourceOf(a_id);
		if (!source.published || !RenderGraphRuntime::Get().IsActive())
			return false;
		if (RenderGraph::UndeclaredAccess::On())
			RenderGraph::UndeclaredAccess::NotePublished(a_id, a_where);
		const auto* indexed = dynamic_cast<const org::GloballyIndexedResource*>(source.current.get());
		if (!indexed)
			return false;
		try {
			if (source.view) {
				if (indexed->GetNumSRVSlices(*source.view) == 0 || indexed->GetNumSRVMipLevels(*source.view) == 0)
					return false;
				a_index = indexed->GetSRVInfo(*source.view, 0, 0).slot.index;
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

	std::string DescribeIndex(std::string_view a_id)
	{
		const auto source = IndexSourceOf(a_id);
		if (!source.found)
			return "not registered";
		if (!source.published)
			return "registered, not published";
		if (!source.current)
			return "published, with no current resource";
		const auto* indexed = dynamic_cast<const org::GloballyIndexedResource*>(source.current.get());
		if (!indexed)
			return fmt::format("'{}' is not an indexed resource", source.current->GetName());
		if (!source.view)
			return fmt::format("'{}': primary view, {} slices, has SRV {}", source.current->GetName(), indexed->GetNumSRVSlices(), indexed->HasSRV());
		const auto slices = indexed->GetNumSRVSlices(*source.view);
		return fmt::format("'{}': view {}, {} slices, {} mips, has SRV {}", source.current->GetName(), static_cast<int>(*source.view), slices,
			slices ? indexed->GetNumSRVMipLevels(*source.view) : 0u, indexed->HasSRV());
	}

	bool IsVersioned(std::string_view a_id)
	{
		if (const auto snapshot = ThreadSnapshot()) {
			const auto* found = snapshot->Find(a_id);
			return found && (found->slot || found->bufferSlot);
		}
		const auto* entry = Find(Published(), a_id);
		return entry && (entry->slot || entry->bufferSlot);
	}

	std::uint32_t ShaderResourceVariant(std::string_view a_id)
	{
		const auto source = IndexSourceOf(a_id);
		return source.found && source.view ? static_cast<std::uint32_t>(*source.view) : UINT32_MAX;
	}

	std::uint64_t ResourceIdOf(std::string_view a_id)
	{
		const auto source = IndexSourceOf(a_id);
		return source.published && source.current ? source.current->GetGlobalResourceID() : 0;
	}

	namespace
	{
		/** @brief Which entries export a view (State::exportAnswers). A cached entry is checked again before it is used. */
		State::ExportAnswer ExportAnswerOf(const ID3D11ShaderResourceView* a_view)
		{
			CheckThread();
			auto& state = Published();
			const std::uint64_t frame = RenderGraph::Frame::Driver::Current().frame;
			if (state.exportAnswersFrame != frame || state.exportAnswersSerial != state.entriesSerial) {
				state.exportAnswers.clear();
				state.exportAnswersFrame = frame;
				state.exportAnswersSerial = state.entriesSerial;
			}
			if (const auto found = state.exportAnswers.find(a_view); found != state.exportAnswers.end()) {
				const auto answer = found->second;
				const bool current = answer.first == UINT32_MAX || ExportOf(state.entries[answer.first]) == a_view;
				if (current)
					return answer;
			}
			State::ExportAnswer answer;
			for (std::uint32_t i = 0; i < state.entries.size(); ++i) {
				if (ExportOf(state.entries[i]) != a_view)
					continue;
				if (answer.first == UINT32_MAX)
					answer.first = i;
				if (!state.entries[i].id.empty()) {
					answer.named = i;
					break;
				}
			}
			state.exportAnswers[a_view] = answer;
			return answer;
		}
	}

	std::string ExportedId(const ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return {};
		const auto answer = ExportAnswerOf(a_view);
		return answer.first != UINT32_MAX ? Published().entries[answer.first].id : std::string{};
	}

	bool IsExport(const ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return false;
		return ExportAnswerOf(a_view).named != UINT32_MAX;
	}

	const org::IVersionedResource* VersionedSlotOf(std::string_view a_id)
	{
		const auto* entry = Find(Published(), a_id);
		if (!entry)
			return nullptr;
		if (entry->slot)
			return entry->slot.get();
		return entry->bufferSlot.get();
	}

	std::uint64_t Serial()
	{
		// Both only grow, so their sum changes whenever either does.
		auto& state = Published();
		return state.entriesSerial + state.snapshotGeneration.load(std::memory_order_acquire);
	}
}

