// The texture registry, the import thread and the samplers.


// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Backend.h"
#include "RenderGraph/DxvkImageImport.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "Globals.h"
#include "Utils/D3D.h"

#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/Engine/Textures/EngineTextures.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/UndeclaredAccess.h"
#include "TextureStreaming/Census.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Gpu/TextureFeedback.h"
#include "TextureStreaming/Streamer.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/BindingTable.h>
#include <Render/Runtime/DescriptorServiceAccess.h>
#include <Render/Runtime/IDescriptorService.h>
#include <Resources/Resource.h>
#include <rhi_helpers.h>
#include <Tracy/Tracy.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <format>
#include <set>
#include <thread>
#include <vector>

namespace DCLF
{
	namespace
	{
		// Indexed by the shadow state's PSTextureAddressMode and PSTextureFilterMode. The engine's table is [6][5] on AE 1.6.1170 and
		// SE 1.5.97 alike. DCLF uses the first four address modes.
		constexpr std::uint32_t kAddressModes = 4;
		constexpr std::uint32_t kFilterModes = 5;
		constexpr Engine::Offset kSamplerTable{ { 411366 }, { 524751 } };

		// 0 is the identity mapping.
		rhi::ComponentMapping MappingOf(const VkComponentMapping& a_components)
		{
			auto source = [](VkComponentSwizzle a_swizzle, std::uint32_t a_identity) -> std::uint32_t {
				switch (a_swizzle) {
				case VK_COMPONENT_SWIZZLE_IDENTITY:
					return a_identity;
				case VK_COMPONENT_SWIZZLE_R:
					return 0;
				case VK_COMPONENT_SWIZZLE_G:
					return 1;
				case VK_COMPONENT_SWIZZLE_B:
					return 2;
				case VK_COMPONENT_SWIZZLE_A:
					return 3;
				case VK_COMPONENT_SWIZZLE_ZERO:
					return 4;
				default:
					return 5;
				}
			};
			const std::uint32_t r = source(a_components.r, 0), g = source(a_components.g, 1), b = source(a_components.b, 2), a = source(a_components.a, 3);
			if (r == 0 && g == 1 && b == 2 && a == 3)
				return 0;
			return r | (g << 3) | (b << 6) | (a << 9) | (1u << 12);
		}

		/** @brief Log a refused format once. Every draw that reads such a view is withheld from the engine and drawn by nobody. */
		void NoteRefusedMapping(rhi::Format a_format, const char* a_why, const rhi::FormatSupport& a_support, VkFormat a_dxvkFormat, rhi::ComponentMapping a_dxvk)
		{
			static std::mutex mutex;
			static std::set<rhi::Format> noted;
			std::lock_guard lock(mutex);
			if (noted.insert(a_format).second)
				logger::warn("[DCLF] Game texture views of format {} refused ({}): BasicRHI stores it as {} (backend {}, swizzle {:X}), DXVK's view format {}, swizzle {:X}; "
							 "their draws are not drawn",
					static_cast<int>(a_format), a_why, static_cast<int>(a_support.storedAs), a_support.backendFormat, a_support.srvSwizzle, static_cast<int>(a_dxvkFormat), a_dxvk);
		}

		/**
		 * @brief The swizzle the view must add on top of BasicRHI's own. DXVK stores some formats as others, such as A8 as R8.
		 * BasicRHI does the same, so usually nothing is added. False when the two disagree.
		 */
		bool ViewMappingOf(const rhi::Device& a_device, rhi::Format a_format, const VkComponentMapping& a_dxvk, VkFormat a_dxvkFormat,
			rhi::ComponentMapping& a_out)
		{
			rhi::FormatSupport support{};
			if (a_device.QueryFormatSupport(a_format, support) != rhi::Result::Ok) {
				NoteRefusedMapping(a_format, "no format support", support, a_dxvkFormat, 0);
				return false;
			}
			if (support.storedAs != rhi::Format::Unknown && a_dxvkFormat != VK_FORMAT_UNDEFINED && support.backendFormat != static_cast<std::uint32_t>(a_dxvkFormat)) {
				NoteRefusedMapping(a_format, "stored differently", support, a_dxvkFormat, 0);
				return false;
			}
			const rhi::ComponentMapping dxvk = MappingOf(a_dxvk);
			if (dxvk == 0 || dxvk == support.srvSwizzle) {
				a_out = 0;
				return true;
			}
			if (support.srvSwizzle == 0) {
				a_out = dxvk;
				return true;
			}
			NoteRefusedMapping(a_format, "swizzles differ", support, a_dxvkFormat, dxvk);
			return false;
		}

		bool DescribeView(const D3D11_SHADER_RESOURCE_VIEW_DESC& a_view, rhi::SrvDesc& a_out)
		{
			a_out.formatOverride = rhi::helpers::ToRHI(a_view.Format);
			switch (a_view.ViewDimension) {
			case D3D11_SRV_DIMENSION_TEXTURE2D:
				a_out.dimension = rhi::SrvDim::Texture2D;
				a_out.tex2D.mostDetailedMip = a_view.Texture2D.MostDetailedMip;
				a_out.tex2D.mipLevels = a_view.Texture2D.MipLevels;
				break;
			case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
				a_out.dimension = rhi::SrvDim::Texture2DArray;
				a_out.tex2DArray.mostDetailedMip = a_view.Texture2DArray.MostDetailedMip;
				a_out.tex2DArray.mipLevels = a_view.Texture2DArray.MipLevels;
				a_out.tex2DArray.firstArraySlice = a_view.Texture2DArray.FirstArraySlice;
				a_out.tex2DArray.arraySize = a_view.Texture2DArray.ArraySize;
				break;
			case D3D11_SRV_DIMENSION_TEXTURECUBE:
				a_out.dimension = rhi::SrvDim::TextureCube;
				a_out.cube.mostDetailedMip = a_view.TextureCube.MostDetailedMip;
				a_out.cube.mipLevels = a_view.TextureCube.MipLevels;
				break;
			case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
				a_out.dimension = rhi::SrvDim::TextureCubeArray;
				a_out.cubeArray.mostDetailedMip = a_view.TextureCubeArray.MostDetailedMip;
				a_out.cubeArray.mipLevels = a_view.TextureCubeArray.MipLevels;
				a_out.cubeArray.first2DArrayFace = a_view.TextureCubeArray.First2DArrayFace;
				a_out.cubeArray.numCubes = a_view.TextureCubeArray.NumCubes;
				break;
			case D3D11_SRV_DIMENSION_TEXTURE3D:
				a_out.dimension = rhi::SrvDim::Texture3D;
				a_out.tex3D.mostDetailedMip = a_view.Texture3D.MostDetailedMip;
				a_out.tex3D.mipLevels = a_view.Texture3D.MipLevels;
				break;
			default:
				return false;  // not used by the eligible Lighting draws
			}
			return a_out.formatOverride != rhi::Format::Unknown;
		}
	}

	struct GpuTextures::Impl
	{
		struct Entry;
		struct Registry
		{
			/**
			 * @brief A view's import. Rejections are permanent per device generation and kept by key without holding the view.
			 * The view is tagged with the slot's serial, so a new view at a freed address is not mistaken for it.
			 */
			struct Slot
			{
				std::weak_ptr<Entry> live;
				bool rejected = false;
				std::uint64_t serial = 0;
			};
			std::mutex mutex;
			ankerl::unordered_dense::map<ID3D11ShaderResourceView*, Slot> entries;
			std::atomic<std::uint32_t> liveCount{ 0 };
			std::uint64_t nextSerial = 0;
			// Pinless imports by resource, plus recent relocations for imports described before a move and registered after it.
			ankerl::unordered_dense::map<std::uint64_t, std::vector<std::weak_ptr<Entry>>> byCookie;
			struct Unclaimed
			{
				std::uint64_t serial = 0;
				std::shared_ptr<const void> storage;
				std::chrono::steady_clock::time_point taken;
				std::uint64_t ticket = 0;  // imports with a ticket up to this began before it
			};
			ankerl::unordered_dense::map<std::uint64_t, std::vector<Unclaimed>> unclaimed;
			// An unclaimed move is kept while an import that began before it is still in flight. That import may have described the old
			// storage, and DXVK's CS thread can be slow during a load.
			std::uint64_t nextTicket = 0;
			std::set<std::uint64_t> importsInFlight;
			// Oldest first, covering [logEnd - log.size(), logEnd).
			std::deque<ID3D11ShaderResourceView*> log;
			std::uint64_t logEnd = 0;
			std::atomic<std::uint64_t> superseded{ 0 };
			std::atomic<std::uint32_t> pinned{ 0 };
		};
		enum class State : std::uint8_t
		{
			Pending,
			Ready,
			Rejected
		};
		struct ImportedBacking
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			rhi::ResourcePtr image;
			IUnknown* tracked = nullptr;  // untrack the same object
			// Old storages from relocations that superseded this import. Its image still reads them.
			std::vector<std::shared_ptr<const void>> storages;
			std::shared_ptr<const RenderGraph::Engine::Textures::Texture> engineTexture;
			// Set when the graph moves the image between layouts, so the descriptor must read the graph's own image.
			std::shared_ptr<org::Resource> graphResource;
			~ImportedBacking()
			{
				// Release order matters: the wrapper, then the import, then the storage behind it.
				image = {};
				if (tracked) {
					RenderGraphRuntime::Get().UntrackResources({ &tracked, 1 });
				}
				storages.clear();
			}
		};
		struct Entry
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			rhi::ResourcePtr image;
			org::OwnedDescriptorBinding binding;
			std::uint32_t index = kInvalid;
			Reject reject = Reject::None;
			std::weak_ptr<Registry> registry;
			ID3D11ShaderResourceView* key = nullptr;
			std::uint64_t serial = 0;
			std::uint64_t cookie = 0, storageSerial = 0;
			bool pinned = false;
			const void* window = nullptr;  // an identity
			std::shared_ptr<const void> graphWindow;
			std::uint32_t sourceTag = ~0u;  // diagnostics
			std::weak_ptr<ImportedBacking> backing;
			// The fields above belong to the importing thread until it stores state with release. Readers load state first.
			std::atomic<State> state{ State::Pending };
			std::atomic<bool> superseded{ false };  // under the registry's lock
			~Entry()
			{
				if (auto owner = registry.lock()) {
					if (binding || graphWindow)
						owner->liveCount.fetch_sub(1, std::memory_order_relaxed);
					std::scoped_lock lock(owner->mutex);
					if (const auto found = owner->entries.find(key); found != owner->entries.end() && found->second.serial == serial && !found->second.rejected)
						owner->entries.erase(found);
					if (const auto byCookie = owner->byCookie.find(cookie); cookie && byCookie != owner->byCookie.end()) {
						std::erase_if(byCookie->second, [](const auto& a_entry) { return a_entry.expired(); });
						if (byCookie->second.empty())
							owner->byCookie.erase(byCookie);
					}
				}
			}
		};
		// Written by both the import thread and the render thread.
		struct Counters
		{
			std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(Reject::Count)> rejected{};
			std::atomic<std::uint32_t> unsupportedLayouts{ 0 };
			std::atomic<std::uint64_t> importedAsync{ 0 };
		};
		// The descriptor service here is retained from the graph, not the epoch's, so imports can run outside any epoch.
		struct ImportContext
		{
			std::shared_ptr<org::runtime::IDescriptorService> service;
			std::shared_ptr<org::runtime::ResourceCleanupQueue> cleanup;
			rhi::Device device;
			std::shared_ptr<const void> deviceOwner;
		};
		struct Work
		{
			std::uint32_t sourceTag = ~0u;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			Replies* replies = nullptr;
			std::uint64_t cookie = 0;
		};
		// Published by the render thread, immutable.
		struct Published
		{
			ImportContext context;
			std::shared_ptr<Registry> registry;
			std::shared_ptr<Counters> counters;
			std::shared_ptr<const FixedBindings> fixed;  // never null
		};
		// The thread shares this, so the last owner only signals it and never joins.
		struct Importer
		{
			EventQueue<Work, 1024> queue;
			std::atomic<std::uint64_t> wake{ 0 };
			std::atomic<bool> stop{ false };
			std::atomic<std::shared_ptr<const Published>> published;
			std::atomic<std::uint32_t> requestsPending{ 0 };
			std::atomic<std::uint64_t> requestsAnswered{ 0 };

			void Notify()
			{
				wake.fetch_add(1, std::memory_order_release);
				wake.notify_one();
			}
		};
		enum class Found
		{
			Unknown,
			Ready,
			Pending,
			Rejected
		};
		std::shared_ptr<Registry> registry;
		std::shared_ptr<Counters> counters = std::make_shared<Counters>();
		std::shared_ptr<Importer> importer;
		std::weak_ptr<org::runtime::ResourceCleanupQueue> cleanup;
		std::array<std::uint32_t, kAddressModes * kFilterModes> samplers{};
		std::array<org::OwnedDescriptorBinding, kAddressModes * kFilterModes> samplerBindings{};
		std::array<bool, kAddressModes * kFilterModes> samplerFailed{};
		org::OwnedDescriptorBinding nullBinding;
		std::uint32_t nullIndex = kInvalid;
		// [0] structured or raw, [1] typed.
		std::array<org::OwnedDescriptorBinding, 2> nullBufferBindings{};
		std::array<std::uint32_t, 2> nullBufferIndices{ kInvalid, kInvalid };

		~Impl()
		{
			if (importer) {
				importer->stop.store(true, std::memory_order_release);
				importer->Notify();
			}
		}

		// The epoch's descriptor service while one prepares, else the graph's retained one for imports outside any epoch.
		static org::runtime::IDescriptorService* ServiceNow(std::shared_ptr<org::runtime::IDescriptorService>& a_retained)
		{
			if (auto* active = org::runtime::GetActiveDescriptorService())
				return active;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !host->Graph())
				return nullptr;
			a_retained = host->Graph()->RetainDescriptorService();
			return a_retained.get();
		}

		static bool MakeContext(ImportContext& a_context)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !host->Graph())
				return false;
			a_context.service = host->Graph()->RetainDescriptorService();
			auto* active = org::runtime::GetActiveDescriptorService();
			a_context.cleanup = active ? active->GetResourceCleanupQueue() : host->ResourceCleanup();
			a_context.device = host->Device();
			a_context.deviceOwner = RenderGraphRuntime::Get().DeviceOwner();
			return a_context.service && a_context.cleanup && a_context.deviceOwner;
		}

		// {8B1E3F52-6C0D-4E2A-9A61-3D5C2F7B4E10} tags a view DCLF rejected with its registry slot's serial.
		static constexpr GUID kRejectedTag = { 0x8b1e3f52, 0x6c0d, 0x4e2a, { 0x9a, 0x61, 0x3d, 0x5c, 0x2f, 0x7b, 0x4e, 0x10 } };
		/** @brief Record a rejection by tagging the view, without holding the entry. Call under the registry lock. */
		static void MarkRejected(Registry::Slot& a_slot, const Entry& a_entry)
		{
			if (a_slot.rejected)
				return;
			a_slot.rejected = true;
			if (a_entry.view)
				a_entry.view->SetPrivateData(kRejectedTag, sizeof(a_slot.serial), &a_slot.serial);
		}
		static bool Tagged(ID3D11ShaderResourceView* a_view, std::uint64_t a_serial)
		{
			std::uint64_t serial = 0;
			UINT size = sizeof(serial);
			return a_view && SUCCEEDED(a_view->GetPrivateData(kRejectedTag, &size, &serial)) && size == sizeof(serial) && serial == a_serial;
		}

		static constexpr std::size_t kRelocationLogEntries = std::size_t(1) << 14;

		/**
		 * @brief Apply the first relocation past the entry's storage. Its backing holds the old storage from then on, and the view leaves
		 * the registry. Call under the registry lock.
		 */
		static void Supersede(Registry& a_registry, Entry& a_entry, const std::shared_ptr<const void>& a_storage)
		{
			if (a_entry.superseded.exchange(true, std::memory_order_acq_rel))
				return;
			if (auto backing = a_entry.backing.lock())
				backing->storages.push_back(a_storage);
			a_registry.superseded.fetch_add(1, std::memory_order_relaxed);
			if (const auto found = a_registry.entries.find(a_entry.key); found != a_registry.entries.end() && found->second.serial == a_entry.serial)
				a_registry.entries.erase(found);
			a_registry.log.push_back(a_entry.key);
			++a_registry.logEnd;
			while (a_registry.log.size() > kRelocationLogEntries)
				a_registry.log.pop_front();
		}

		/** @brief Marks an import in flight from before its description until after its registration. */
		struct InFlight
		{
			explicit InFlight(std::shared_ptr<Registry> a_registry) :
				registry(std::move(a_registry))
			{
				std::scoped_lock lock(registry->mutex);
				ticket = ++registry->nextTicket;
				registry->importsInFlight.insert(ticket);
			}
			~InFlight()
			{
				std::scoped_lock lock(registry->mutex);
				registry->importsInFlight.erase(ticket);
			}
			InFlight(const InFlight&) = delete;
			InFlight& operator=(const InFlight&) = delete;

			std::shared_ptr<Registry> registry;
			std::uint64_t ticket = 0;
		};

		/** @brief Register a settled import. A relocation that came after it was described supersedes it now. Call under the registry lock. */
		static void Register(Registry& a_registry, const std::shared_ptr<Entry>& a_entry)
		{
			if (!a_entry->cookie)
				return;
			if (a_entry->pinned) {
				a_registry.pinned.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			a_registry.byCookie[a_entry->cookie].push_back(a_entry);
			if (const auto unclaimed = a_registry.unclaimed.find(a_entry->cookie); unclaimed != a_registry.unclaimed.end())
				for (const auto& relocation : unclaimed->second)
					if (relocation.serial > a_entry->storageSerial)
						Supersede(a_registry, *a_entry, relocation.storage);
		}

		static Found Find(Registry& a_registry, ID3D11ShaderResourceView* a_view, Binding& a_binding)
		{
			const auto found = a_registry.entries.find(a_view);
			if (found == a_registry.entries.end())
				return Found::Unknown;
			auto& slot = found->second;
			if (auto live = slot.live.lock()) {
				switch (live->state.load(std::memory_order_acquire)) {
				case State::Pending:
					a_binding.index = kInvalid;
					a_binding.owner = std::move(live);
					a_binding.pending = true;
					return Found::Pending;
				case State::Rejected:
					MarkRejected(slot, *live);
					a_binding = { kInvalid, {} };
					return Found::Rejected;
				case State::Ready:
					a_binding.index = live->index;
					a_binding.owner = live->index == kInvalid ? std::shared_ptr<const void>{} : std::move(live);
					return Found::Ready;
				}
			}
			if (slot.rejected) {
				if (Tagged(a_view, slot.serial)) {
					a_binding = { kInvalid, {} };
					return Found::Rejected;
				}
				// Another view at a released one's address.
				a_registry.entries.erase(found);
			}
			return Found::Unknown;
		}

		// A streamed shell has no storage of its own, so import its current window. An ungated view makes its base now. False if no window exists.
		static bool StreamedWindow(Entry& a_entry, winrt::com_ptr<ID3D11ShaderResourceView>& a_window)
		{
			a_window.attach(TextureStreaming::CurrentWindow(a_entry.view.get()));
			if (!a_window) {
				const auto streamed = TextureStreaming::IdOf(a_entry.view.get());
				if (!streamed)
					return true;
				TextureStreaming::BuildWindow(streamed, TextureStreaming::kBaseTop);
				a_window.attach(TextureStreaming::CurrentWindow(a_entry.view.get()));
				if (!a_window)
					return false;
			}
			a_entry.window = a_window.get();
			return true;
		}

		static void NoteImported(ID3D11ShaderResourceView* a_view, [[maybe_unused]] std::uint32_t a_sourceTag, [[maybe_unused]] const void* a_image)
		{
			TextureStreaming::Census::NoteRendererUse(a_view);
			TracyPlot("CS.DCLF.Texture.ImportSource", static_cast<std::int64_t>(a_sourceTag));
			if (!SwitchValue(Switch::TraceTexturePaths).empty()) {
				TracyPlot("CS.DCLF.Texture.ImportView", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(a_view)));
				TracyPlot("CS.DCLF.Texture.ImportImage", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(a_image)));
			}
		}

		/** @brief An engine texture ORG owns. The image is ORG's, so it never moves and needs no tracking or pin. */
		static Reject ImportEngineTexture(Entry& a_entry, std::shared_ptr<const RenderGraph::Engine::Textures::Texture> a_texture,
			const ImportContext& a_context, std::uint32_t a_sourceTag)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			a_entry.view->GetDesc(&viewDesc);
			rhi::SrvDesc srv{};
			if (!DescribeView(viewDesc, srv) || !a_texture->graph)
				return Reject::View;
			const VkComponentMapping components{ static_cast<VkComponentSwizzle>(a_texture->components[0]), static_cast<VkComponentSwizzle>(a_texture->components[1]),
				static_cast<VkComponentSwizzle>(a_texture->components[2]), static_cast<VkComponentSwizzle>(a_texture->components[3]) };
			if (!ViewMappingOf(a_context.device, srv.formatOverride, components, VK_FORMAT_UNDEFINED, srv.componentMapping))
				return Reject::View;
			const auto image = a_texture->graph->GetAPIResource();
			auto backing = a_context.cleanup->Make<ImportedBacking>();
			backing->view.copy_from(a_entry.view.get());
			backing->engineTexture = std::move(a_texture);
			a_entry.backing = backing;
			{
				DCLF_ZONE("CS.DCLF.Texture.Import.CreateView");
				a_entry.binding = a_context.service->CreateOwnedShaderResourceView(a_context.deviceOwner, image, backing, srv);
			}
			if (!a_entry.binding)
				return Reject::View;
			a_entry.index = a_entry.binding.Index();
			NoteImported(a_entry.view.get(), a_sourceTag, nullptr);
			return Reject::None;
		}

		// Marks the image stable in DXVK, which is a synchronous CS chunk, then imports it. The entry is the caller's until Settle.
		static Reject Import(Entry& a_entry, const ImportContext& a_context, Counters& a_counters, [[maybe_unused]] std::uint32_t a_sourceTag)
		{
			a_entry.sourceTag = a_sourceTag;
			// A view of an engine texture may be a tripwire handle, which DXVK must never see.
			if (auto engine = RenderGraph::Engine::Textures::Find(a_entry.view.get()))
				return ImportEngineTexture(a_entry, std::move(engine), a_context, a_sourceTag);
			winrt::com_ptr<ID3D11ShaderResourceView> window;
			if (!StreamedWindow(a_entry, window))
				return Reject::NotImage;
			if (TextureStreaming::GraphWindow graph; window && TextureStreaming::GraphWindowOf(a_entry.view.get(), window.get(), graph)) {
				a_entry.graphWindow = std::move(graph.owner);
				a_entry.index = graph.index;
				NoteImported(window.get(), a_sourceTag, nullptr);
				return Reject::None;
			}
			// A tripwire handle must be unwrapped before DXVK sees it.
			auto* view = RenderGraph::Engine::Tripwire::Unwrap(window ? window.get() : a_entry.view.get());
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			view->GetDesc(&viewDesc);
			rhi::SrvDesc srv{};
			winrt::com_ptr<ID3D11Resource> bufferResource;
			const bool structured = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER && viewDesc.Format == DXGI_FORMAT_UNKNOWN;
			if (structured) {
				view->GetResource(bufferResource.put());
				auto buffer = bufferResource.try_as<ID3D11Buffer>();
				D3D11_BUFFER_DESC desc{};
				if (!buffer)
					return Reject::View;
				buffer->GetDesc(&desc);
				if (!desc.StructureByteStride || desc.StructureByteStride % 4)
					return Reject::View;
				// Vulkan structured and raw SRVs both use storage-buffer descriptors. The range is in words so DXVK's suballocation offset
				// need not be a multiple of the structure stride.
				srv.dimension = rhi::SrvDim::Buffer;
				srv.buffer.kind = rhi::BufferViewKind::Raw;
				srv.buffer.firstElement = std::uint64_t(viewDesc.Buffer.FirstElement) * desc.StructureByteStride / 4;
				srv.buffer.numElements = viewDesc.Buffer.NumElements * (desc.StructureByteStride / 4);
			} else if (!DescribeView(viewDesc, srv)) {
				if (a_counters.rejected[static_cast<std::size_t>(Reject::View)].load(std::memory_order_relaxed) < 8)
					logger::info("[DCLF] Game texture view not supported: format {}, dimension {}", static_cast<int>(viewDesc.Format), static_cast<int>(viewDesc.ViewDimension));
				return Reject::View;
			}

			DxvkOrgInteropResourceInfo info{};
			std::shared_ptr<org::Resource> graphResource;
			// A pinless import is tracked. Its backing ends the tracking once made, otherwise this does.
			struct Tracking
			{
				IUnknown* object = nullptr;
				~Tracking()
				{
					if (object)
						RenderGraphRuntime::Get().UntrackResources({ &object, 1 });
				}
			} tracking;
			{
				DCLF_ZONE("CS.DCLF.Texture.Import.DescribeResource");
				auto& runtime = RenderGraphRuntime::Get();
				// DXVK accepts image views, but buffer imports must name the underlying resource.
				IUnknown* object = structured ? static_cast<IUnknown*>(bufferResource.get()) : view;
				if (runtime.PinlessImports()) {
					DxvkOrgInteropTrackedResource tracked{};
					HRESULT result = E_FAIL;
					if (!runtime.TrackResources({ &object, 1 }, { &tracked, 1 }, { &result, 1 }) || FAILED(result))
						return Reject::NotImage;
					tracking.object = object;
					info = tracked.info;
					a_entry.cookie = tracked.cookie;
					a_entry.storageSerial = tracked.storageSerial;
					a_entry.pinned = (tracked.flags & DXVK_ORG_TRACKED_PINNED) != 0;
				} else if (!runtime.DescribeResource(object, info)) {
					return Reject::NotImage;
				}
				if (info.kind != (structured ? DXVK_ORG_INTEROP_RESOURCE_BUFFER : DXVK_ORG_INTEROP_RESOURCE_IMAGE))
					return Reject::NotImage;
			}
			if (structured && ::RenderGraph::Backend::IsD3D12()) {
				// On D3D12 the description carries the ID3D12Resource in the VkBuffer's place.
				const auto& buffer = info.buffer;
				if (buffer.offset % 4)
					return Reject::Import;
				a_entry.image = RenderGraph::ImportD3D12Resource(a_context.device, reinterpret_cast<void*>(buffer.buffer), "DCLF imported structured buffer");
				if (!a_entry.image)
					return Reject::Import;
				srv.buffer.firstElement += buffer.offset / 4;
			} else if (structured) {
				const auto& buffer = info.buffer;
				if (buffer.offset % 4 || !(buffer.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
					return Reject::Import;
				rhi::vulkan::ImportedBufferDesc import{};
				import.buffer = buffer.buffer;
				import.size = buffer.offset + buffer.size;
				import.usage = buffer.usage;
				import.debugName = "DCLF imported structured buffer";
				if (rhi::vulkan::import_buffer(a_context.device, import, a_entry.image) != rhi::Result::Ok || !a_entry.image)
					return Reject::Import;
				srv.buffer.firstElement += buffer.offset / 4;
			} else {
				const auto& image = info.image;
				// The descriptor names the layout DXVK rests the image in. The graph never moves these images.
				if (!RenderGraph::HomeLayoutOf(image.layout)) {
					if (a_counters.unsupportedLayouts.fetch_add(1, std::memory_order_relaxed) == 0)
						logger::warn("[DCLF] A game texture is in Vulkan layout {}, which BasicRHI cannot import; such textures stay native",
							static_cast<int>(image.layout));
					return Reject::Import;
				}

				// Aftermath reports Vulkan object names at a fault, so name imports by DXVK image and game SRV.
				const auto importName = std::format("DCLF texture image {:#x} SRV {:#x}",
					reinterpret_cast<std::uintptr_t>(image.image), reinterpret_cast<std::uintptr_t>(view));
				// A raw import of a GENERAL image names GENERAL, but a graph that moves images holds it in a read-only layout during the read.
				// So the view is made on the graph resource's own image instead.
				if (image.layout == VK_IMAGE_LAYOUT_GENERAL && (RenderGraph::NativeGraphMovesImages() || RenderGraph::RawAccess::On()))
					if (auto graph = RenderGraph::GraphResourceOfImage(reinterpret_cast<std::uint64_t>(image.image)); graph && !graph->CommonLayoutOnly())
						graphResource = std::move(graph);
				if (graphResource && RenderGraph::NativeGraphMovesImages()) {
					RenderGraph::RawAccess::Count(RenderGraph::RawAccess::Kind::ViewFromGraph);
				} else {
					if (graphResource) {
						RenderGraph::RawAccess::Count(RenderGraph::RawAccess::Kind::ViewGeneral);
						if (RenderGraph::RawAccess::LogOnce(reinterpret_cast<std::uint64_t>(image.image)))
							logger::info(
								"[ORG raw access] DCLF's view of '{}' names GENERAL, but the graph moves the image between layouts; a pass that "
								"declares the read holds it in the read-only layout",
								graphResource->GetName());
						graphResource.reset();
					}
					DCLF_ZONE("CS.DCLF.Texture.Import.WrapImage");
					a_entry.image = RenderGraph::ImportDxvkImage(a_context.device, image, importName.c_str());
					if (!a_entry.image)
						return Reject::Import;
				}

				if (!ViewMappingOf(a_context.device, srv.formatOverride, image.components, image.viewFormat, srv.componentMapping))
					return Reject::View;
			}
			auto backing = a_context.cleanup->Make<ImportedBacking>();
			backing->view.copy_from(view);
			backing->image = std::move(a_entry.image);
			backing->graphResource = std::move(graphResource);
			backing->tracked = std::exchange(tracking.object, nullptr);
			a_entry.backing = backing;
			{
				DCLF_ZONE("CS.DCLF.Texture.Import.CreateView");
				const rhi::Resource viewed = backing->graphResource ? backing->graphResource->GetAPIResource() : backing->image.Get();
				a_entry.binding = a_context.service->CreateOwnedShaderResourceView(a_context.deviceOwner, viewed, backing, srv);
			}
			if (!a_entry.binding)
				return Reject::View;
			a_entry.index = a_entry.binding.Index();
			NoteImported(view, a_sourceTag, structured ? nullptr : reinterpret_cast<const void*>(info.image.image));
			return Reject::None;
		}

		// Store the fields first, then the state with release.
		static void Settle(Entry& a_entry, Reject a_reject, Counters& a_counters)
		{
			a_entry.reject = a_reject;
			if (a_reject != Reject::None)
				a_counters.rejected[static_cast<std::size_t>(a_reject)].fetch_add(1, std::memory_order_relaxed);
			else if (auto owner = a_entry.registry.lock())
				owner->liveCount.fetch_add(1, std::memory_order_relaxed);
			a_entry.state.store(a_reject == Reject::None ? State::Ready : State::Rejected, std::memory_order_release);
		}

		static Binding AnswerOf(const std::shared_ptr<Entry>& a_entry, Reject a_reject)
		{
			if (a_reject != Reject::None || a_entry->index == kInvalid)
				return { kInvalid, {} };
			return { a_entry->index, a_entry };
		}

		static void Answered(Importer& a_importer, Replies& a_replies, ID3D11ShaderResourceView* a_view, std::uint64_t a_cookie, Binding a_binding)
		{
			Reply reply;
			reply.view = a_view;
			reply.cookie = a_cookie;
			reply.binding = std::move(a_binding);
			a_replies.Push(std::move(reply));
			a_importer.requestsPending.fetch_sub(1, std::memory_order_relaxed);
			a_importer.requestsAnswered.fetch_add(1, std::memory_order_relaxed);
		}

		// The registry only holds a pending entry for the import this thread is doing.
		static void Answer(Importer& a_importer, Work& a_work, const Published& a_published)
		{
			DCLF_ZONE("CS.DCLF.Texture.Answer");
			auto* key = a_work.view.get();
			if (!key) {
				Answered(a_importer, *a_work.replies, nullptr, a_work.cookie, a_published.fixed->null);
				return;
			}
			auto& table = *a_published.registry;
			Binding binding{ kInvalid, {} };
			std::shared_ptr<Entry> entry;
			{
				std::scoped_lock lock(table.mutex);
				if (Find(table, key, binding) == Found::Unknown) {
					entry = a_published.context.cleanup->Make<Entry>();
					entry->view = std::move(a_work.view);  // keeps the SRV alive, so its address is not reused, while imported
					entry->registry = a_published.registry;
					entry->key = key;
					entry->serial = ++table.nextSerial;
					auto& slot = table.entries[key];
					slot = {};
					slot.live = entry;
					slot.serial = entry->serial;
				}
			}
			if (entry) {
				const InFlight inFlight(a_published.registry);
				Reject reject = Reject::Import;
				try {
					reject = Import(*entry, a_published.context, *a_published.counters, a_work.sourceTag);
				} catch (const std::exception& e) {
					logger::error("[DCLF] A game texture could not be imported; its draws stay native: {}", e.what());
				}
				if (reject == Reject::None)
					a_published.counters->importedAsync.fetch_add(1, std::memory_order_relaxed);
				Settle(*entry, reject, *a_published.counters);
				{
					std::scoped_lock lock(table.mutex);
					if (reject != Reject::None) {
						if (const auto found = table.entries.find(key); found != table.entries.end() && found->second.serial == entry->serial)
							MarkRejected(found->second, *entry);
					} else {
						Register(table, entry);
						// The window changed between the import and here, so no listener could supersede the entry.
						if (entry->window) {
							winrt::com_ptr<ID3D11ShaderResourceView> window;
							window.attach(TextureStreaming::CurrentWindow(entry->view.get()));
							if (window.get() != entry->window)
								Supersede(table, *entry, {});
						}
					}
				}
				binding = AnswerOf(entry, reject);
			}
			Answered(a_importer, *a_work.replies, key, a_work.cookie, std::move(binding));
		}

		static void RunImporter(Importer& a_importer)
		{
#if defined(TRACY_ENABLE)
			tracy::SetThreadName("CS DCLF texture import");
#endif
			// Requests that came before a context was published.
			std::vector<Work> parked;
			for (;;) {
				const std::uint64_t observed = a_importer.wake.load(std::memory_order_acquire);
				if (a_importer.stop.load(std::memory_order_acquire))
					return;
				const auto published = a_importer.published.load(std::memory_order_acquire);
				bool worked = false;
				if (published && !parked.empty()) {
					for (auto& work : parked)
						Answer(a_importer, work, *published);
					parked.clear();
					worked = true;
				}
				// References the requests held are let go here, on the import thread.
				worked |= a_importer.queue.Drain([&](Work&& a_work) {
					if (published)
						Answer(a_importer, a_work, *published);
					else
						parked.push_back(std::move(a_work));
				}) != 0;
				if (!worked)
					a_importer.wake.wait(observed, std::memory_order_acquire);
			}
		}

		void Queue(Work&& a_work)
		{
			importer->queue.Push(std::move(a_work));
			importer->Notify();
		}
	};

	GpuTextures::GpuTextures() :
		impl(std::make_unique<Impl>())
	{
		impl->samplers.fill(kInvalid);
		impl->samplerFailed.fill(false);
		// Started up front so Request never races a lazy start.
		impl->importer = std::make_shared<Impl::Importer>();
		std::thread([state = impl->importer] { Impl::RunImporter(*state); }).detach();
	}

	GpuTextures::~GpuTextures() = default;

	std::string GpuTextures::SourceName(std::uint32_t a_tag)
	{
		// Highest first, since a tag is its source's first value plus a slot.
		static constexpr std::pair<Source, const char*> kSources[] = { { Source::Scaleform, "Scaleform" }, { Source::ImGui, "ImGui" },
			{ Source::ShadowDiffuse, "shadow diffuse" }, { Source::FamilyNull, "family null view" }, { Source::FamilyHeader, "family header" },
			{ Source::FrameSlot, "frame slot" }, { Source::Mask, "pipeline mask" }, { Source::Projected, "projected decal" },
			{ Source::Feature, "material feature" }, { Source::Material, "material slot" } };
		if (a_tag == ~0u)
			return "ResolveBinding";
		for (const auto& [source, name] : kSources) {
			const auto first = static_cast<std::uint32_t>(source);
			if (a_tag >= first)
				return a_tag > first ? fmt::format("ResolveBinding {} {}", name, a_tag - first) : fmt::format("ResolveBinding {}", name);
		}
		return fmt::format("ResolveBinding tag {}", a_tag);
	}

	GpuTextures& GpuTextures::Get()
	{
		static GpuTextures textures;
		return textures;
	}

	GpuTextures::Binding GpuTextures::ResolveBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, ResolveFailure* a_failure,
		std::source_location a_where)
	{
		DCLF_ZONE("CS.DCLF.Texture.ResolveBinding");
		ResolveFailure ignored = ResolveFailure::None;
		auto& failure = a_failure ? *a_failure : ignored;
		failure = ResolveFailure::None;
		if (!a_view)
			return NullBinding();
		// A second import means no pass declared what the index reads.
		if (RenderGraph::UndeclaredAccess::On())
			RenderGraph::UndeclaredAccess::NoteView(a_view, SourceName(a_sourceTag), a_where);
		// These holders keep the full window and do not follow a swap. They ask again until it exists.
		if (!TextureStreaming::PinFull(a_view)) {
			failure = ResolveFailure::StreamedWindow;
			return { kInvalid, {} };
		}
		auto registry = impl->registry;
		if (registry) {
			DCLF_ZONE("CS.DCLF.Texture.RegistryLookup");
			Binding binding;
			std::scoped_lock lock(registry->mutex);
			// A pending import is not waited for. The caller asks again.
			switch (impl->Find(*registry, a_view, binding)) {
			case Impl::Found::Unknown:
				break;
			case Impl::Found::Pending:
				failure = ResolveFailure::Pending;
				return binding;
			case Impl::Found::Rejected:
				failure = ResolveFailure::Rejected;
				return binding;
			case Impl::Found::Ready:
				return binding;
			}
		}
		// Imports are legal only inside an epoch. Do not cache that as a rejection, since the view may import fine at the next epoch.
		Impl::ImportContext context;
		if (!Impl::MakeContext(context)) {
			failure = ResolveFailure::NoContext;
			return { kInvalid, {} };
		}
		{
			DCLF_ZONE("CS.DCLF.Texture.Import");
			impl->cleanup = context.cleanup;
			if (!registry)
				impl->registry = registry = context.cleanup->Make<Impl::Registry>();
			auto entry = context.cleanup->Make<Impl::Entry>();
			entry->view.copy_from(a_view);
			entry->registry = registry;
			entry->key = a_view;
			const Impl::InFlight inFlight(registry);
			const auto reject = Impl::Import(*entry, context, *impl->counters, a_sourceTag);
			Impl::Settle(*entry, reject, *impl->counters);
			{
				// The import thread may have imported the view meanwhile. Both results are valid.
				DCLF_ZONE("CS.DCLF.Texture.Import.Register");
				std::scoped_lock lock(registry->mutex);
				entry->serial = ++registry->nextSerial;
				auto& slot = registry->entries[a_view];
				slot = {};
				slot.live = entry;
				slot.serial = entry->serial;
				if (reject != Reject::None)
					Impl::MarkRejected(slot, *entry);
				else
					Impl::Register(*registry, entry);
			}
			if (reject != Reject::None) {
				failure = ResolveFailure::Rejected;
				return { kInvalid, {} };
			}
			return { entry->index, entry };
		}
	}

	GpuTextures::Binding GpuTextures::NullOf(ViewKind a_kind)
	{
		if (a_kind == ViewKind::Buffer)
			return { .index = NullBufferIndex(false) };
		// Made once on the render thread. The null descriptor is a 2D view.
		struct NullViews
		{
			winrt::com_ptr<ID3D11ShaderResourceView> texture2D, texture2DArray, texture3D, cube;
		};
		static const NullViews nulls = [] {
			NullViews out;
			auto* device = globals::d3d::device;
			if (!device)
				return out;
			const float zero[4 * 6]{};
			std::array<D3D11_SUBRESOURCE_DATA, 6> data{};
			for (auto& slice : data)
				slice = { .pSysMem = zero, .SysMemPitch = 16, .SysMemSlicePitch = 16 };
			const auto make2D = [&](std::uint32_t a_slices, std::uint32_t a_misc, const D3D11_SHADER_RESOURCE_VIEW_DESC* a_view, const char* a_name,
									winrt::com_ptr<ID3D11ShaderResourceView>& a_out) {
				const D3D11_TEXTURE2D_DESC desc{ .Width = 1,
					.Height = 1,
					.MipLevels = 1,
					.ArraySize = a_slices,
					.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
					.SampleDesc = { .Count = 1 },
					.Usage = D3D11_USAGE_DEFAULT,
					.BindFlags = D3D11_BIND_SHADER_RESOURCE,
					.MiscFlags = a_misc };
				winrt::com_ptr<ID3D11Texture2D> texture;
				if (FAILED(device->CreateTexture2D(&desc, data.data(), texture.put())) || FAILED(device->CreateShaderResourceView(texture.get(), a_view, a_out.put())))
					return;
				Util::SetResourceName(texture.get(), a_name);
			};
			make2D(1, 0, nullptr, "DCLF::GpuTextures::Null2D", out.texture2D);
			D3D11_SHADER_RESOURCE_VIEW_DESC arrayView{ .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY };
			arrayView.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0, .ArraySize = 1 };
			make2D(1, 0, &arrayView, "DCLF::GpuTextures::Null2DArray", out.texture2DArray);
			make2D(6, D3D11_RESOURCE_MISC_TEXTURECUBE, nullptr, "DCLF::GpuTextures::NullCube", out.cube);
			const D3D11_TEXTURE3D_DESC volume{ .Width = 1,
				.Height = 1,
				.Depth = 1,
				.MipLevels = 1,
				.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE };
			winrt::com_ptr<ID3D11Texture3D> texture3D;
			if (SUCCEEDED(device->CreateTexture3D(&volume, data.data(), texture3D.put())) &&
				SUCCEEDED(device->CreateShaderResourceView(texture3D.get(), nullptr, out.texture3D.put())))
				Util::SetResourceName(texture3D.get(), "DCLF::GpuTextures::Null3D");
			if (!out.texture2D || !out.texture2DArray || !out.cube || !out.texture3D)
				logger::error("[DCLF] textures: the null views by kind could not be made; an engine draw with an empty slot stays the engine's");
			return out;
		}();
		ID3D11ShaderResourceView* view = a_kind == ViewKind::Cube           ? nulls.cube.get() :
		                                 a_kind == ViewKind::Texture3D      ? nulls.texture3D.get() :
		                                 a_kind == ViewKind::Texture2DArray ? nulls.texture2DArray.get() :
		                                                                      nulls.texture2D.get();
		return view ? ResolveBinding(view, Tag(Source::FamilyNull, static_cast<std::uint32_t>(a_kind))) : Binding{};
	}

	std::string GpuTextures::DescribeFailure(ID3D11ShaderResourceView* a_view, std::uint32_t a_slot, ResolveFailure a_failure)
	{
		if (!a_view)
			return std::format("t{}: no view", a_slot);
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		a_view->GetDesc(&view);
		std::uint32_t width = 0, height = 0, depth = 0, mips = 0;
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		if (const auto texture = resource.try_as<ID3D11Texture2D>()) {
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			width = desc.Width, height = desc.Height, depth = desc.ArraySize, mips = desc.MipLevels;
		} else if (const auto volume = resource.try_as<ID3D11Texture3D>()) {
			D3D11_TEXTURE3D_DESC desc{};
			volume->GetDesc(&desc);
			width = desc.Width, height = desc.Height, depth = desc.Depth, mips = desc.MipLevels;
		}
		const char* why = "no reason recorded";
		switch (a_failure) {
		case ResolveFailure::Rejected:
			why = "GpuTextures rejected it (see its reject counts in the status)";
			break;
		case ResolveFailure::Pending:
			why = "the import thread is importing it (a later batch takes it)";
			break;
		case ResolveFailure::StreamedWindow:
			why = "a streamed texture without its full window yet (asked for; a later batch takes it)";
			break;
		case ResolveFailure::NoContext:
			why = "no import context (outside an epoch's descriptor service)";
			break;
		default:
			break;
		}
		return std::format("t{}: format {}, dimension {}, {}x{}x{}, {} mips: {}", a_slot, static_cast<std::uint32_t>(view.Format),
			static_cast<std::uint32_t>(view.ViewDimension), width, height, depth, mips, why);
	}

	void GpuTextures::Request(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, Replies& a_replies, std::uint64_t a_cookie)
	{
		Impl::Work work;
		work.view.copy_from(a_view);  // held until the import takes it or the request is answered
		work.sourceTag = a_sourceTag;
		work.replies = &a_replies;
		work.cookie = a_cookie;
		impl->importer->requestsPending.fetch_add(1, std::memory_order_relaxed);
		// A streamed texture is answered once its base window exists. Until then its objects are not admitted.
		if (const auto streamed = TextureStreaming::IdOf(a_view); streamed && !TextureStreaming::BaseReady(a_view)) {
			TextureStreaming::RequestBase(a_view);
			auto gated = std::make_shared<Impl::Work>(std::move(work));
			if (SceneScheduler::Graph().PostTextureGate(streamed, TextureStreaming::kBaseRevision, [impl = impl.get(), gated] { impl->Queue(std::move(*gated)); }))
				return;
			work = std::move(*gated);
		}
		impl->Queue(std::move(work));
	}

	void GpuTextures::PublishImportContext()
	{
		Impl::ImportContext context;
		if (!Impl::MakeContext(context))
			return;
		// Must run before any material row is packed. A row is written once and names the feedback buffer.
		if (TextureStreaming::Enabled() && !TextureStreaming::FullMode())
			TextureFeedback::Ensure();
		impl->cleanup = context.cleanup;
		if (!impl->registry)
			impl->registry = context.cleanup->Make<Impl::Registry>();
		// The samplers read the engine's table, which is a render thread read. The scene work only takes these from Fixed.
		FixedBindings fixed;
		fixed.null = NullBinding();
		for (std::uint32_t address = 0; address < kAddressModes; ++address)
			for (std::uint32_t filter = 0; filter < kFilterModes; ++filter)
				fixed.samplers[address * kFilterModes + filter] = SamplerBinding(address, filter);
		auto same = [](const Binding& a_a, const Binding& a_b) { return a_a.index == a_b.index && a_a.owner == a_b.owner; };
		auto& importer = *impl->importer;
		const auto current = importer.published.load(std::memory_order_acquire);
		const bool sameFixed = current && same(current->fixed->null, fixed.null) &&
		                       std::equal(fixed.samplers.begin(), fixed.samplers.end(), current->fixed->samplers.begin(), same);
		if (current && sameFixed && current->context.service == context.service && current->context.cleanup == context.cleanup &&
			current->context.deviceOwner == context.deviceOwner && current->registry == impl->registry && current->counters == impl->counters)
			return;
		auto next = std::make_shared<Impl::Published>();
		next->context = std::move(context);
		next->registry = impl->registry;
		next->counters = impl->counters;
		next->fixed = sameFixed ? current->fixed : std::make_shared<const FixedBindings>(std::move(fixed));
		importer.published.store(std::move(next), std::memory_order_release);
		// Wake requests parked for want of a context.
		importer.Notify();
	}

	std::shared_ptr<const GpuTextures::FixedBindings> GpuTextures::Fixed() const
	{
		const auto published = impl->importer->published.load(std::memory_order_acquire);
		return published ? published->fixed : nullptr;
	}

	std::shared_ptr<const void> GpuTextures::Seal(std::vector<std::shared_ptr<const void>> a_owners) const
	{
		const auto published = impl->importer->published.load(std::memory_order_acquire);
		if (!published)
			return {};  // nothing was answered yet, so nothing holds an owner
		return org::ExecutionResourceLease::Create(published->context.cleanup, std::move(a_owners)).Owner();
	}

	std::uint32_t GpuTextures::NullIndex()
	{
		if (impl->nullIndex != kInvalid)
			return impl->nullIndex;
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Impl::ServiceNow(retained);
		if (!service)
			return kInvalid;
		rhi::SrvDesc srv{};
		srv.dimension = rhi::SrvDim::Texture2D;
		srv.formatOverride = rhi::Format::R8G8B8A8_UNorm;
		srv.tex2D.mipLevels = 1;
		impl->nullBinding = service->CreateOwnedShaderResourceView(RenderGraphRuntime::Get().DeviceOwner(), {}, {}, srv);
		if (!impl->nullBinding) {
			logger::warn("[DCLF] Null texture descriptors are unsupported (VK_EXT_robustness2 nullDescriptor)");
			return kInvalid;
		}
		impl->nullIndex = impl->nullBinding.Index();
		return impl->nullIndex;
	}

	std::uint32_t GpuTextures::NullBufferIndex(bool a_typed)
	{
		auto& index = impl->nullBufferIndices[a_typed ? 1 : 0];
		if (index != kInvalid)
			return index;
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Impl::ServiceNow(retained);
		if (!service)
			return kInvalid;
		rhi::SrvDesc srv{};
		srv.dimension = rhi::SrvDim::Buffer;
		srv.buffer.kind = a_typed ? rhi::BufferViewKind::Typed : rhi::BufferViewKind::Structured;
		if (a_typed)
			srv.formatOverride = rhi::Format::R32_UInt;
		else
			srv.buffer.structureByteStride = sizeof(std::uint32_t);
		auto& binding = impl->nullBufferBindings[a_typed ? 1 : 0];
		binding = service->CreateOwnedShaderResourceView(RenderGraphRuntime::Get().DeviceOwner(), {}, {}, srv);
		if (!binding) {
			logger::warn("[DCLF] Null buffer descriptors are unsupported (VK_EXT_robustness2 nullDescriptor)");
			return kInvalid;
		}
		index = binding.Index();
		return index;
	}

	std::uint32_t GpuTextures::NullIndexFor(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return NullIndex();
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		a_view->GetDesc(&desc);
		if (desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX)
			return NullBufferIndex(false);
		if (desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
			return NullBufferIndex(desc.Format != DXGI_FORMAT_UNKNOWN);
		return NullIndex();
	}

	GpuTextures::Binding GpuTextures::NullBinding()
	{
		return { NullIndex(), impl->nullBinding.Owner() };
	}

	std::uint32_t GpuTextures::Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
	{
		if (a_addressMode >= kAddressModes || a_filterMode >= kFilterModes)
			return kInvalid;
		auto& index = impl->samplers[a_addressMode * kFilterModes + a_filterMode];
		if (index != kInvalid)
			return index;
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Impl::ServiceNow(retained);
		const auto* table = reinterpret_cast<ID3D11SamplerState* const*>(REL::Offset(kSamplerTable).address());
		auto* state = table[a_addressMode * kFilterModes + a_filterMode];
		if (!service || !state)
			return kInvalid;
		D3D11_SAMPLER_DESC desc{};
		state->GetDesc(&desc);
		// A sampler the backend cannot create is never handed out, so its draws are skipped. Not retried, since the engine's table is fixed.
		const std::size_t key = a_addressMode * kFilterModes + a_filterMode;
		if (impl->samplerFailed[key])
			return kInvalid;
		try {
			impl->samplerBindings[key] = service->CreateOwnedSampler(RenderGraphRuntime::Get().DeviceOwner(), ShaderBindings::SamplerFromD3D11(desc));
			if (!impl->samplerBindings[key])
				throw std::runtime_error("owned sampler creation failed");
			index = impl->samplerBindings[key].Index();
		} catch (const std::exception& e) {
			impl->samplerFailed[key] = true;
			logger::error("[DCLF] Engine sampler (address mode {}, filter mode {}) could not be created; draws that sample with it are skipped: {}", a_addressMode, a_filterMode, e.what());
			return kInvalid;
		}
		++stats.samplers;
		return index;
	}

	GpuTextures::Binding GpuTextures::SamplerBinding(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
	{
		const auto index = Sampler(a_addressMode, a_filterMode);
		return { index, index == kInvalid ? std::shared_ptr<const void>{} : impl->samplerBindings[a_addressMode * kFilterModes + a_filterMode].Owner() };
	}

	void GpuTextures::NoteRelocations(std::span<const ImportRelocations::Relocation> a_relocations)
	{
		const auto registry = impl->registry;
		if (!registry)
			return;
		DCLF_ZONE("CS.DCLF.Texture.NoteRelocations");
		// Kept a while for imports described before the move and registered after it, and longer while such an import is in flight.
		constexpr auto kUnclaimedFor = std::chrono::seconds(2);
		const auto now = std::chrono::steady_clock::now();
		std::scoped_lock lock(registry->mutex);
		for (const auto& relocation : a_relocations) {
			registry->unclaimed[relocation.cookie].push_back({ relocation.serial, relocation.storage, now, registry->nextTicket });
			if (const auto found = registry->byCookie.find(relocation.cookie); found != registry->byCookie.end())
				for (const auto& weak : found->second)
					if (const auto entry = weak.lock(); entry && entry->storageSerial < relocation.serial)
						Impl::Supersede(*registry, *entry, relocation.storage);
		}
		std::erase_if(registry->unclaimed, [&](auto& a_unclaimed) {
			std::erase_if(a_unclaimed.second, [&](const auto& a_relocation) {
				const bool olderImport = !registry->importsInFlight.empty() && *registry->importsInFlight.begin() <= a_relocation.ticket;
				return now - a_relocation.taken > kUnclaimedFor && !olderImport;
			});
			return a_unclaimed.second.empty();
		});
	}

	bool GpuTextures::RelocatedViews(std::uint64_t& a_cursor, std::vector<ID3D11ShaderResourceView*>& a_out) const
	{
		const auto published = impl->importer->published.load(std::memory_order_acquire);
		if (!published || !published->registry)
			return true;
		auto& registry = *published->registry;
		std::scoped_lock lock(registry.mutex);
		const std::uint64_t first = registry.logEnd - registry.log.size();
		const bool complete = a_cursor >= first;
		for (std::uint64_t at = std::max(a_cursor, first); at < registry.logEnd; ++at)
			a_out.push_back(registry.log[static_cast<std::size_t>(at - first)]);
		a_cursor = registry.logEnd;
		return complete;
	}

	void GpuTextures::NoteWindowChanged(const ID3D11ShaderResourceView* a_shell)
	{
		const auto published = impl->importer->published.load(std::memory_order_acquire);
		if (!published || !published->registry)
			return;
		auto& registry = *published->registry;
		std::scoped_lock lock(registry.mutex);
		if (const auto found = registry.entries.find(const_cast<ID3D11ShaderResourceView*>(a_shell)); found != registry.entries.end())
			if (const auto entry = found->second.live.lock())
				Impl::Supersede(registry, *entry, {});
	}

	bool GpuTextures::Superseded(const void* a_owner)
	{
		return a_owner && static_cast<const Impl::Entry*>(a_owner)->superseded.load(std::memory_order_acquire);
	}

	const void* GpuTextures::ImportedWindow(const void* a_owner)
	{
		return a_owner ? static_cast<const Impl::Entry*>(a_owner)->window : nullptr;
	}

	std::string GpuTextures::DescribeSuperseded() const
	{
		const auto registry = impl->registry;
		if (!registry)
			return {};
		std::map<std::uint32_t, std::uint32_t> counts;
		std::uint32_t total = 0;
		std::scoped_lock lock(registry->mutex);
		for (const auto& [cookie, entries] : registry->byCookie) {
			for (const auto& weak : entries) {
				if (const auto entry = weak.lock(); entry && entry->superseded.load(std::memory_order_acquire)) {
					++counts[entry->sourceTag];
					++total;
				}
			}
		}
		std::string text = std::format("{} alive, by first requester's tag:", total);
		for (const auto& [tag, count] : counts)
			text += std::format(" {}={}", tag, count);
		return text;
	}

	GpuTextures::Stats GpuTextures::GetStats() const
	{
		auto result = stats;
		const auto& counters = *impl->counters;
		for (std::size_t i = 0; i < result.rejected.size(); ++i)
			result.rejected[i] = counters.rejected[i].load(std::memory_order_relaxed);
		result.unsupportedLayouts = counters.unsupportedLayouts.load(std::memory_order_relaxed);
		result.importedAsync = counters.importedAsync.load(std::memory_order_relaxed);
		result.requestsPending = impl->importer->requestsPending.load(std::memory_order_relaxed);
		result.requestsAnswered = impl->importer->requestsAnswered.load(std::memory_order_relaxed);
		if (const auto registry = impl->registry) {
			result.cached = registry->liveCount.load(std::memory_order_relaxed);
			result.superseded = registry->superseded.load(std::memory_order_relaxed);
			result.pinned = registry->pinned.load(std::memory_order_relaxed);
			std::scoped_lock lock(registry->mutex);
			result.registrySlots = static_cast<std::uint32_t>(registry->entries.size());
		}
		if (const auto cleanup = impl->cleanup.lock())
			result.cleanupPending = cleanup->Pending();
		return result;
	}

	void GpuTextures::Clear()
	{
		++generation;
		impl->registry.reset();
		// Imports in flight settle into the old registry and counters, which their requests hold.
		impl->counters = std::make_shared<Impl::Counters>();
		impl->samplers.fill(kInvalid);
		impl->samplerBindings = {};
		impl->samplerFailed.fill(false);
		impl->nullIndex = kInvalid;
		impl->nullBinding = {};
		impl->nullBufferIndices.fill(kInvalid);
		impl->nullBufferBindings = {};
		stats = {};
	}
}

