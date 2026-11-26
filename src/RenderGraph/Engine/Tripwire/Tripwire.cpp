#include "RenderGraph/Engine/Tripwire/TripwireHandles.h"
#include "Snapshot/EngineReads.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Switches.h"
#include "Utils/D3D.h"

#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace RenderGraph::Engine::Tripwire
{
	namespace
	{
		constexpr std::size_t kKinds = static_cast<std::size_t>(Kind::Count);

		bool LogRequested()
		{
			static const bool requested = Switches::On("CS_ORG_TRIPWIRE_LOG");
			return requested;
		}

		// Watch every method of guest resources, views, shaders, states and queries. Device, context and swap chain are the hot path and keep theirs.
		bool WatchAllRequested()
		{
			static const bool requested = Switches::On("CS_ORG_TRIPWIRE_WATCH_ALL");
			return requested;
		}

		HMODULE ModuleAt(const void* a_address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_address), &module))
				return nullptr;
			return module;
		}

		/** @brief The address range of CS's own image, where every handle class's vtable lives. */
		struct ImageRange
		{
			std::uintptr_t begin = 0;
			std::uintptr_t end = 0;
		};

		const ImageRange& OwnImage()
		{
			static const ImageRange range = [] {
				ImageRange out;
				const HMODULE module = ModuleAt(reinterpret_cast<const void*>(&OwnImage));
				if (!module)
					return out;
				const auto base = reinterpret_cast<std::uintptr_t>(module);
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
				out.begin = base;
				out.end = base + nt->OptionalHeader.SizeOfImage;
				return out;
			}();
			return range;
		}

		const ImageRange& GameImage()
		{
			static const ImageRange range = [] {
				ImageRange out;
				const auto base = REL::Module::get().base();
				if (!base)
					return out;
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
				out.begin = base;
				out.end = base + nt->OptionalHeader.SizeOfImage;
				return out;
			}();
			return range;
		}

		std::string ModuleName(HMODULE a_module)
		{
			wchar_t path[MAX_PATH]{};
			const DWORD length = GetModuleFileNameW(a_module, path, MAX_PATH);
			if (!length)
				return "?";
			std::wstring_view file(path, length);
			if (const auto slash = file.find_last_of(L"\\/"); slash != std::wstring_view::npos)
				file.remove_prefix(slash + 1);
			return stl::utf16_to_utf8(file).value_or("?");
		}

		// The nearest Address Library ID at or before the offset, within the engine's longest function.
		bool GameId(std::uint64_t a_offset, std::uint64_t& a_id, std::uint64_t& a_within)
		{
			constexpr std::uint64_t kLongestFunction = 0x4000;
			static const REL::Offset2ID table;
			auto after = std::upper_bound(table.begin(), table.end(), a_offset, [](std::uint64_t a_value, const auto& a_entry) { return a_value < a_entry.offset; });
			if (after == table.begin())
				return false;
			--after;
			if (a_offset - after->offset >= kLongestFunction)
				return false;
			a_id = after->id;
			a_within = a_offset - after->offset;
			return true;
		}

		struct SiteKey
		{
			void* caller;
			std::uint16_t slot;
			Kind kind;
			bool operator==(const SiteKey&) const = default;
		};
		struct SiteKeyHash
		{
			std::size_t operator()(const SiteKey& a_key) const noexcept
			{
				return std::hash<void*>{}(a_key.caller) ^ (static_cast<std::size_t>(a_key.slot) << 40) ^ (static_cast<std::size_t>(a_key.kind) << 56);
			}
		};
		struct Site
		{
			const char* method = nullptr;
			std::string where;
			std::uint64_t calls = 0;
			std::uint64_t reported = 0;  // calls at the last report
			bool forwarded = false;
		};

		struct Sites
		{
			std::mutex lock;
			std::unordered_map<SiteKey, Site, SiteKeyHash> sites;
			std::uint64_t calls = 0;
			std::uint64_t reportedCalls = 0;
			std::atomic<bool> selfTest{ false };
		};
		Sites& GetSites()
		{
			static Sites sites;
			return sites;
		}

		struct Classes
		{
			std::array<std::atomic<const void*>, kKinds> vtables{};
			std::array<std::atomic<HandleCore* (*)(void*)>, kKinds> cores{};
		};
		Classes& GetClasses()
		{
			static Classes classes;
			return classes;
		}

		struct Retired
		{
			std::mutex lock;
			std::vector<std::shared_ptr<const void>> owners;
		};
		// Never destroyed, since a guest object's tag can die after static destruction began.
		Retired& GetRetired()
		{
			static auto* retired = new Retired;
			return *retired;
		}

		void Retire(std::shared_ptr<org::Resource> a_graph, std::shared_ptr<const void> a_owner)
		{
			if (!a_graph && !a_owner)
				return;
			auto& retired = GetRetired();
			std::lock_guard guard(retired.lock);
			if (a_graph)
				retired.owners.push_back(std::move(a_graph));
			if (a_owner)
				retired.owners.push_back(std::move(a_owner));
		}

		/** @brief Queues one object for ServiceRetired. Any thread. */
		void RetireObject(std::shared_ptr<const void> a_object)
		{
			if (!a_object)
				return;
			auto& retired = GetRetired();
			std::lock_guard guard(retired.lock);
			retired.owners.push_back(std::move(a_object));
		}

		// Private-data slot and IID of the tag AttachGraphResource puts on an object.
		constexpr GUID kGraphTagGuid = { 0x6b4c2a7e, 0x3d51, 0x4f0b, { 0x9e, 0x27, 0x8a, 0x1c, 0x5d, 0x0f, 0x3b, 0x64 } };

		/** @brief Holds an ORG resource and its owner for as long as the D3D11 object that keeps it lives. */
		class GraphTag final : public IUnknown
		{
		public:
			GraphTag(std::shared_ptr<org::Resource> a_graph, std::shared_ptr<const void> a_owner) :
				graph(std::move(a_graph)), owner(std::move(a_owner)) {}
			GraphTag(const GraphTag&) = delete;
			GraphTag& operator=(const GraphTag&) = delete;
			// Can run on any thread. The GPU free waits for ServiceRetired.
			~GraphTag() { Retire(std::move(graph), std::move(owner)); }

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (a_riid == __uuidof(IUnknown) || a_riid == kGraphTagGuid) {
					AddRef();
					*a_out = static_cast<IUnknown*>(this);
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = --refs;
				if (left == 0)
					delete this;
				return left;
			}

			const std::shared_ptr<org::Resource>& Graph() const { return graph; }

		private:
			std::atomic<ULONG> refs{ 1 };
			std::shared_ptr<org::Resource> graph;
			std::shared_ptr<const void> owner;
		};

		winrt::com_ptr<ID3D11DeviceChild> AsDeviceChild(const void* a_object)
		{
			winrt::com_ptr<ID3D11DeviceChild> child;
			if (a_object)
				(void)static_cast<IUnknown*>(const_cast<void*>(a_object))->QueryInterface(__uuidof(ID3D11DeviceChild), child.put_void());
			return child;
		}

		/** @brief Interfaces the real object refuses too, so refusing them is no tripwire. */
		bool IsOtherObjectIid(REFIID a_riid)
		{
			static const IID kIids[] = { __uuidof(ID3D11Resource), __uuidof(ID3D11View), __uuidof(ID3D11Asynchronous), __uuidof(ID3D11Texture1D),
				__uuidof(ID3D11Texture2D), __uuidof(ID3D11Texture3D), __uuidof(ID3D11Buffer), __uuidof(ID3D11ShaderResourceView),
				__uuidof(ID3D11RenderTargetView), __uuidof(ID3D11DepthStencilView), __uuidof(ID3D11UnorderedAccessView), __uuidof(ID3D11VertexShader),
				__uuidof(ID3D11PixelShader), __uuidof(ID3D11ComputeShader), __uuidof(ID3D11GeometryShader), __uuidof(ID3D11HullShader),
				__uuidof(ID3D11DomainShader), __uuidof(ID3D11InputLayout), __uuidof(ID3D11BlendState), __uuidof(ID3D11DepthStencilState),
				__uuidof(ID3D11RasterizerState), __uuidof(ID3D11SamplerState), __uuidof(ID3D11Query), __uuidof(ID3D11Predicate),
				__uuidof(ID3D11Counter) };
			return std::find(std::begin(kIids), std::end(kIids), a_riid) != std::end(kIids);
		}

		std::string IidText(REFIID a_iid)
		{
			return fmt::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}", a_iid.Data1, a_iid.Data2, a_iid.Data3, a_iid.Data4[0],
				a_iid.Data4[1], a_iid.Data4[2], a_iid.Data4[3], a_iid.Data4[4], a_iid.Data4[5], a_iid.Data4[6], a_iid.Data4[7]);
		}

		// Periodic reports list only new or recalled sites so long tours stay short. The exit report lists all.
		void ReportLocked(Sites& a_sites, std::string_view a_reason, bool a_full)
		{
			struct Totals
			{
				std::uint64_t sites = 0, calls = 0, failedSites = 0;
				std::vector<const std::pair<const SiteKey, Site>*> entries;
			};
			std::array<Totals, kKinds> perKind{};
			std::size_t changed = 0;
			for (const auto& entry : a_sites.sites) {
				auto& totals = perKind[static_cast<std::size_t>(entry.first.kind)];
				++totals.sites;
				totals.calls += entry.second.calls;
				totals.failedSites += entry.second.forwarded ? 0 : 1;
				const bool active = entry.second.calls != entry.second.reported;
				changed += active ? 1 : 0;
				if (a_full || active)
					totals.entries.push_back(&entry);
			}
			logger::info("[ORG] tripwire report ({}): {} call sites, {} calls, {} sites new or called since the last report{}", a_reason,
				a_sites.sites.size(), a_sites.calls, changed, a_full ? "" : " (only those are listed)");
			for (std::size_t kind = 0; kind < kKinds; ++kind) {
				auto& totals = perKind[kind];
				if (!totals.sites)
					continue;
				logger::info("[ORG] tripwire   {}: {} sites ({} failed, {} forwarded), {} calls", InterfaceName(static_cast<Kind>(kind)), totals.sites,
					totals.failedSites, totals.sites - totals.failedSites, totals.calls);
				std::sort(totals.entries.begin(), totals.entries.end(), [](const auto* a_left, const auto* a_right) { return a_left->second.calls > a_right->second.calls; });
				for (const auto* entry : totals.entries) {
					const auto& site = entry->second;
					logger::info("[ORG] tripwire     {} x{}{} from {}, {}", site.method, site.calls,
						site.reported == 0 ? std::string(" (new)") : fmt::format(" (+{})", site.calls - site.reported), site.where,
						site.forwarded ? "forwarded" : "failed");
				}
			}
			for (auto& entry : a_sites.sites)
				entry.second.reported = entry.second.calls;
			a_sites.reportedCalls = a_sites.calls;
		}

		struct Reporter
		{
			std::mutex lock;
			std::condition_variable_any wake;
			std::jthread thread;
		};

		Reporter& GetReporter()
		{
			static Reporter s_reporter;
			return s_reporter;
		}

		void ReportLoop(std::stop_token a_stop, std::chrono::seconds a_interval)
		{
			auto& reporter = GetReporter();
			for (;;) {
				{
					std::unique_lock wait(reporter.lock);
					if (reporter.wake.wait_for(wait, a_stop, a_interval, [] { return false; }) || a_stop.stop_requested())
						return;
				}
				auto& sites = GetSites();
				std::lock_guard guard(sites.lock);
				if (sites.calls != sites.reportedCalls)
					ReportLocked(sites, "periodic", false);
			}
		}
	}

	const char* InterfaceName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::Texture1D:
			return "ID3D11Texture1D";
		case Kind::Texture2D:
			return "ID3D11Texture2D";
		case Kind::Texture3D:
			return "ID3D11Texture3D";
		case Kind::Buffer:
			return "ID3D11Buffer";
		case Kind::ShaderResourceView:
			return "ID3D11ShaderResourceView";
		case Kind::RenderTargetView:
			return "ID3D11RenderTargetView";
		case Kind::DepthStencilView:
			return "ID3D11DepthStencilView";
		case Kind::UnorderedAccessView:
			return "ID3D11UnorderedAccessView";
		case Kind::VertexShader:
			return "ID3D11VertexShader";
		case Kind::PixelShader:
			return "ID3D11PixelShader";
		case Kind::ComputeShader:
			return "ID3D11ComputeShader";
		case Kind::InputLayout:
			return "ID3D11InputLayout";
		case Kind::BlendState:
			return "ID3D11BlendState";
		case Kind::DepthStencilState:
			return "ID3D11DepthStencilState";
		case Kind::RasterizerState:
			return "ID3D11RasterizerState";
		case Kind::SamplerState:
			return "ID3D11SamplerState";
		case Kind::Query:
			return "ID3D11Query";
		case Kind::DeviceContext:
			return "ID3D11DeviceContext";
		case Kind::Device:
			return "ID3D11Device";
		case Kind::SwapChain:
			return "IDXGISwapChain";
		default:
			return "?";
		}
	}

	std::span<const char* const> SlotNames(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::Texture1D:
			return ID3D11Texture1DTrips::kSlotNames;
		case Kind::Texture2D:
			return ID3D11Texture2DTrips::kSlotNames;
		case Kind::Texture3D:
			return ID3D11Texture3DTrips::kSlotNames;
		case Kind::Buffer:
			return ID3D11BufferTrips::kSlotNames;
		case Kind::ShaderResourceView:
			return ID3D11ShaderResourceViewTrips::kSlotNames;
		case Kind::RenderTargetView:
			return ID3D11RenderTargetViewTrips::kSlotNames;
		case Kind::DepthStencilView:
			return ID3D11DepthStencilViewTrips::kSlotNames;
		case Kind::UnorderedAccessView:
			return ID3D11UnorderedAccessViewTrips::kSlotNames;
		case Kind::VertexShader:
			return ID3D11VertexShaderTrips::kSlotNames;
		case Kind::PixelShader:
			return ID3D11PixelShaderTrips::kSlotNames;
		case Kind::ComputeShader:
			return ID3D11ComputeShaderTrips::kSlotNames;
		case Kind::InputLayout:
			return ID3D11InputLayoutTrips::kSlotNames;
		case Kind::BlendState:
			return ID3D11BlendStateTrips::kSlotNames;
		case Kind::DepthStencilState:
			return ID3D11DepthStencilStateTrips::kSlotNames;
		case Kind::RasterizerState:
			return ID3D11RasterizerStateTrips::kSlotNames;
		case Kind::SamplerState:
			return ID3D11SamplerStateTrips::kSlotNames;
		case Kind::Query:
			return ID3D11QueryTrips::kSlotNames;
		case Kind::DeviceContext:
			return ID3D11DeviceContextTrips::kSlotNames;
		case Kind::Device:
			return ID3D11DeviceTrips::kSlotNames;
		case Kind::SwapChain:
			return IDXGISwapChainTrips::kSlotNames;
		default:
			return {};
		}
	}

	namespace
	{
		/** @brief Later versions of the device, context and swap chain interfaces. Every CS caller handles a refusal, so it is not logged. */
		bool IsNativeUpgradeIid(REFIID a_riid)
		{
			static const IID kIids[] = { __uuidof(ID3D11Device1), __uuidof(ID3D11Device2), __uuidof(ID3D11Device3), __uuidof(ID3D11Device4),
				__uuidof(ID3D11Device5), __uuidof(ID3D11DeviceContext1), __uuidof(ID3D11DeviceContext2), __uuidof(ID3D11DeviceContext3),
				__uuidof(ID3D11DeviceContext4), __uuidof(IDXGISwapChain1), __uuidof(IDXGISwapChain2), __uuidof(IDXGISwapChain3), __uuidof(IDXGISwapChain4) };
			return std::find(std::begin(kIids), std::end(kIids), a_riid) != std::end(kIids);
		}
	}

	namespace Detail
	{
		std::atomic<ContextObserver> contextObserver{ nullptr };
		std::atomic<const NativeServices*> nativeServices{ nullptr };
		std::atomic<const MapHooks*> mapHooks{ nullptr };
		std::atomic<GuestCopyHook> guestCopyHook{ nullptr };
	}

	void SetGuestCopyHook(GuestCopyHook a_hook)
	{
		Detail::guestCopyHook.store(a_hook, std::memory_order_release);
	}

	void SetContextObserver(ContextObserver a_observer)
	{
		Detail::contextObserver.store(a_observer, std::memory_order_release);
	}

	void SetMapHooks(const MapHooks& a_hooks)
	{
		// Kept for the process, since the context reads it without a lock.
		static const MapHooks hooks = a_hooks;
		Detail::mapHooks.store(&hooks, std::memory_order_release);
	}

	void SetNativeServices(const NativeServices& a_services)
	{
		// Kept for the process, since handles read it from any thread without a lock.
		static NativeServices s_services;
		if (Detail::nativeServices.load(std::memory_order_acquire))
			return;
		s_services = a_services;
		Detail::nativeServices.store(&s_services, std::memory_order_release);
	}

	bool NativeServicesInstalled()
	{
		return Detail::nativeServices.load(std::memory_order_acquire) != nullptr;
	}

	bool IsOwnCode(const void* a_address)
	{
		const auto& image = OwnImage();
		const auto address = reinterpret_cast<std::uintptr_t>(a_address);
		return address >= image.begin && address < image.end;
	}

	bool IsGameCode(const void* a_address)
	{
		const auto& image = GameImage();
		const auto address = reinterpret_cast<std::uintptr_t>(a_address);
		return address >= image.begin && address < image.end;
	}

	bool GameFunction(const void* a_address, std::uint64_t& a_id, std::uint64_t& a_within)
	{
		if (!IsGameCode(a_address))
			return false;
		return GameId(reinterpret_cast<std::uintptr_t>(a_address) - GameImage().begin, a_id, a_within);
	}

	std::string DescribeCaller(const void* a_address)
	{
		const HMODULE module = ModuleAt(a_address);
		if (!module)
			return fmt::format("{:#x} (no module)", reinterpret_cast<std::uintptr_t>(a_address));
		const auto offset = reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module);
		const auto name = ModuleName(module);
		if (reinterpret_cast<std::uintptr_t>(module) == REL::Module::get().base()) {
			std::uint64_t id = 0, within = 0;
			if (GameId(offset, id, within))
				return fmt::format("{}+{:#x} (ID {}+{:#x})", name, offset, id, within);
			return fmt::format("{}+{:#x} (no Address Library ID)", name, offset);
		}
		static const HMODULE self = ModuleAt(reinterpret_cast<const void*>(&DescribeCaller));
		if (module != self)
			return fmt::format("{}+{:#x} (another plugin calls D3D11: see org-extension-api-notes.md)", name, offset);
		return fmt::format("{}+{:#x}", name, offset);
	}

	HandleCore::~HandleCore()
	{
		if (auto* object = wrapped.load(std::memory_order_acquire))
			object->Release();
		if (const std::unique_ptr<const HandleState> current{ state.load(std::memory_order_acquire) })
			Retire(current->graph, current->owner);
	}

	void HandleCore::Init(Kind a_kind, Options&& a_options, IUnknown* a_wrap)
	{
		kind = a_kind;
		name = std::move(a_options.name);
		if (a_options.graph || a_options.owner)
			state.store(new HandleState{ std::move(a_options.graph), std::move(a_options.owner) }, std::memory_order_release);
		wrapped.store(a_wrap, std::memory_order_release);
		if (!a_wrap)
			return;
		a_wrap->AddRef();
		const bool hotPath = kind == Kind::Device || kind == Kind::DeviceContext || kind == Kind::SwapChain;
		watchAll = WatchAllRequested() && !hotPath;
		// The wrapped object carries the ORG resource too, since it can outlive the handle in DXVK's bindings or a getter's result.
		if (const auto* current = state.load(std::memory_order_acquire); current && !hotPath && !Detail::CoreOf(a_wrap))
			(void)AttachGraphResource(a_wrap, current->graph, current->owner);
	}

	std::string HandleCore::Name() const
	{
		std::shared_lock guard(nameLock);
		return name;
	}

	void HandleCore::SetName(std::string a_name)
	{
		std::unique_lock guard(nameLock);
		name = std::move(a_name);
	}

	namespace
	{
		// The detail text is only built for the first call at a site.
		template <class D>
		void RecordSite(const HandleCore& a_core, std::uint16_t a_slot, const char* a_method, void* a_caller, bool a_forwarded, D&& a_detail)
		{
			auto& sites = GetSites();
			std::lock_guard guard(sites.lock);
			++sites.calls;
			auto [entry, added] = sites.sites.try_emplace(SiteKey{ a_caller, a_slot, a_core.kind });
			++entry->second.calls;
			if (!added)
				return;
			entry->second.method = a_method;
			entry->second.forwarded = a_forwarded;
			entry->second.where = DescribeCaller(a_caller);
			const std::string detail = a_detail();
			const auto message = fmt::format("[ORG] tripwire: {}::{}{} from {}, {} (handle \"{}\"){}", InterfaceName(a_core.kind), a_method,
				detail.empty() ? std::string() : fmt::format("({})", detail), entry->second.where, a_forwarded ? "forwarded to D3D11" : "failed",
				a_core.Name(), sites.selfTest ? " [self-test]" : "");
			if (sites.selfTest)
				logger::info("{}", message);
			else
				logger::warn("{}", message);
			// In native mode another plugin's D3D11 call has nothing behind it, so name it as an error once per site.
			if (!a_forwarded && !sites.selfTest && NativeServicesInstalled() && !IsOwnCode(a_caller) && !IsGameCode(a_caller)) {
				const HMODULE module = ModuleAt(a_caller);
				logger::error("[ORG native] {} calls {}::{} ({}): native mode (CS_ORG_NATIVE=1) has no D3D11, so the call failed and this "
							  "plugin's rendering does not work. Disable the plugin for native runs, or run on DXVK. Recorded in "
							  "docs/development/org-extension-api-notes.md.",
					module ? ModuleName(module) : std::string("code outside any module"), InterfaceName(a_core.kind), a_method, entry->second.where);
			} else if (!a_forwarded && !sites.selfTest && a_core.kind == Kind::DeviceContext && !IsOwnCode(a_caller) && !IsGameCode(a_caller)) {
				// The guest context draws nothing either.
				const HMODULE module = ModuleAt(a_caller);
				logger::error("[ORG] {} calls ID3D11DeviceContext::{} ({}) on the engine's context: in graph mode it keeps binds and draws "
							  "nothing, so this plugin's rendering does not work. Disable the plugin, or run with CS_ORG=0.",
					module ? ModuleName(module) : std::string("code outside any module"), a_method, entry->second.where);
			}
		}
	}

	void HandleCore::Record(std::uint16_t a_slot, const char* a_method, void* a_caller, bool a_forwarded, std::string_view a_detail) const
	{
		RecordSite(*this, a_slot, a_method, a_caller, a_forwarded, [&] { return std::string(a_detail); });
	}

	void HandleCore::RecordArgument(std::uint16_t a_slot, const char* a_method, void* a_caller, const HandleCore& a_argument) const
	{
		RecordSite(*this, a_slot, a_method, a_caller, true,
			[&] { return fmt::format("{} handle \"{}\" passed on", InterfaceName(a_argument.kind), a_argument.Name()); });
	}

	InputLayoutHandle::InputLayoutHandle(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::span<const std::uint8_t> a_vsBytecode) :
		elements(a_elements.begin(), a_elements.end()),
		vsBytecode(a_vsBytecode.begin(), a_vsBytecode.end())
	{
		semantics.reserve(elements.size());
		for (const auto& element : elements)
			semantics.emplace_back(element.SemanticName ? element.SemanticName : "");
		for (std::size_t i = 0; i < elements.size(); ++i)
			elements[i].SemanticName = semantics[i].c_str();
	}

	namespace Detail
	{
		void RegisterHandleClass(Kind a_kind, const void* a_vtable, HandleCore* (*a_core)(void*))
		{
			auto& classes = GetClasses();
			const auto index = static_cast<std::size_t>(a_kind);
			classes.cores[index].store(a_core, std::memory_order_relaxed);
			classes.vtables[index].store(a_vtable, std::memory_order_release);
		}

		HandleCore* CoreOf(const void* a_object)
		{
			if (!a_object)
				return nullptr;
			const void* vtable = *static_cast<const void* const*>(a_object);
			if (!vtable)
				return nullptr;  // a kind with no handle yet has a null entry
			// Every handle's vtable is in CS's image, unlike DXVK's or the engine's objects.
			const auto& image = OwnImage();
			const auto address = reinterpret_cast<std::uintptr_t>(vtable);
			if (address < image.begin || address >= image.end)
				return nullptr;
			auto& classes = GetClasses();
			for (std::size_t i = 0; i < kKinds; ++i) {
				if (classes.vtables[i].load(std::memory_order_acquire) == vtable)
					return classes.cores[i].load(std::memory_order_relaxed)(const_cast<void*>(a_object));
			}
			return nullptr;
		}

		void NoteHandleArgument(const CallSite& a_site, const void* a_handle)
		{
			const auto* argument = CoreOf(a_handle);
			if (a_site.core && argument)
				a_site.core->RecordArgument(a_site.slot, a_site.method, a_site.caller, *argument);
		}

		bool IsDebugName(REFGUID a_guid)
		{
			return a_guid == Util::WKPDID_D3DDebugObjectNameT;
		}

		HRESULT QueryInterface(HandleCore& a_core, IUnknown* a_self, std::span<const IID> a_iids, REFIID a_riid, void** a_out, void* a_caller)
		{
			if (!a_out)
				return E_POINTER;
			if (std::find(a_iids.begin(), a_iids.end(), a_riid) != a_iids.end()) {
				a_self->AddRef();
				*a_out = a_self;
				return S_OK;
			}
			// The real object refuses these too, so this is no tripwire. The engine's texture save helper probes this way.
			if (IsOtherObjectIid(a_riid) || (!a_core.Wrapped() && NativeServicesInstalled() && IsNativeUpgradeIid(a_riid))) {
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			// Another interface escapes the handle. Always logged, and forwarded in guest mode.
			a_core.Record(0, "QueryInterface", a_caller, a_core.Wrapped() != nullptr, IidText(a_riid));
			if (a_core.Wrapped())
				return a_core.Wrapped()->QueryInterface(a_riid, a_out);
			*a_out = nullptr;
			return E_NOINTERFACE;
		}

		template <class F>
		HRESULT OnWrapped(const HandleCore& a_core, F&& a_call)
		{
			switch (a_core.kind) {
			case Kind::Device:
				return a_call(static_cast<ID3D11Device*>(a_core.Wrapped()));
			case Kind::SwapChain:
				return a_call(static_cast<IDXGIObject*>(a_core.Wrapped()));
			default:
				return a_call(static_cast<ID3D11DeviceChild*>(a_core.Wrapped()));
			}
		}

		HRESULT SetPrivateData(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, UINT a_size, const void* a_data, void* a_caller)
		{
			const auto forward = [&](auto* a_object) { return a_object->SetPrivateData(a_guid, a_size, a_data); };
			if (IsDebugName(a_guid)) {
				a_core.SetName(a_data ? std::string(static_cast<const char*>(a_data), strnlen(static_cast<const char*>(a_data), a_size)) : std::string());
				if (a_core.Wrapped())
					OnWrapped(a_core, forward);
				return S_OK;
			}
			// A native handle keeps the bytes as D3D11 does, since the icon loader and Landscape Seams Fix use tags.
			if (!a_core.Wrapped() && NativeServicesInstalled()) {
				std::lock_guard guard(a_core.privateLock);
				auto& entries = a_core.privateData;
				std::erase_if(entries, [&](const auto& a_entry) { return a_entry.first == a_guid; });
				if (a_data && a_size)
					entries.emplace_back(a_guid, std::vector<std::uint8_t>(static_cast<const std::uint8_t*>(a_data), static_cast<const std::uint8_t*>(a_data) + a_size));
				return S_OK;
			}
			if (a_core.Enter(a_slot, "SetPrivateData", a_caller))
				return OnWrapped(a_core, forward);
			return E_FAIL;
		}

		HRESULT SetPrivateDataInterface(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, const IUnknown* a_data, void* a_caller)
		{
			if (a_core.Wrapped() || !NativeServicesInstalled()) {
				if (a_core.Enter(a_slot, "SetPrivateDataInterface", a_caller))
					return OnWrapped(a_core, [&](auto* a_object) { return a_object->SetPrivateDataInterface(a_guid, a_data); });
				return E_FAIL;
			}
			// A native handle keeps it as D3D11 does, released with the handle.
			winrt::com_ptr<IUnknown> held;
			held.copy_from(const_cast<IUnknown*>(a_data));
			std::lock_guard guard(a_core.privateLock);
			auto& entries = a_core.privateInterfaces;
			const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& a_entry) { return a_entry.first == a_guid; });
			if (found != entries.end()) {
				if (held)
					found->second = std::move(held);
				else
					entries.erase(found);
			} else if (held) {
				entries.emplace_back(a_guid, std::move(held));
			}
			return S_OK;
		}

		HRESULT GetPrivateData(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, UINT* a_size, void* a_data, void* a_caller)
		{
			if (!a_core.Wrapped() && a_size && NativeServicesInstalled()) {
				std::lock_guard guard(a_core.privateLock);
				const auto& entries = a_core.privateInterfaces;
				const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& a_entry) { return a_entry.first == a_guid; });
				if (found != entries.end()) {
					if (!a_data) {
						*a_size = sizeof(IUnknown*);
						return S_OK;
					}
					if (*a_size < sizeof(IUnknown*)) {
						*a_size = sizeof(IUnknown*);
						return DXGI_ERROR_MORE_DATA;
					}
					found->second.copy_to(static_cast<IUnknown**>(a_data));
					*a_size = sizeof(IUnknown*);
					return S_OK;
				}
				const auto& data = a_core.privateData;
				if (const auto bytes = std::find_if(data.begin(), data.end(), [&](const auto& a_entry) { return a_entry.first == a_guid; }); bytes != data.end()) {
					const auto size = static_cast<UINT>(bytes->second.size());
					if (!a_data) {
						*a_size = size;
						return S_OK;
					}
					if (*a_size < size) {
						*a_size = size;
						return DXGI_ERROR_MORE_DATA;
					}
					std::memcpy(a_data, bytes->second.data(), size);
					*a_size = size;
					return S_OK;
				}
				// In native mode a missing entry is an answer, not a tripwire, since many lookups ask every object.
				if (!IsDebugName(a_guid)) {
					*a_size = 0;
					return DXGI_ERROR_NOT_FOUND;
				}
			}
			if (IsDebugName(a_guid)) {
				if (!a_size)
					return E_INVALIDARG;
				const auto name = a_core.Name();
				const auto size = static_cast<UINT>(name.size());
				if (!a_data) {
					*a_size = size;
					return S_OK;
				}
				if (*a_size < size) {
					*a_size = size;
					return DXGI_ERROR_MORE_DATA;
				}
				std::memcpy(a_data, name.data(), size);
				*a_size = size;
				return S_OK;
			}
			if (a_core.Enter(a_slot, "GetPrivateData", a_caller))
				return OnWrapped(a_core, [&](auto* a_object) { return a_object->GetPrivateData(a_guid, a_size, a_data); });
			Zero(a_size);
			return E_FAIL;
		}

		std::uint64_t Calls(Kind a_kind, std::string_view a_method)
		{
			auto& sites = GetSites();
			std::lock_guard guard(sites.lock);
			std::uint64_t calls = 0;
			for (const auto& [key, site] : sites.sites)
				if (key.kind == a_kind && a_method == site.method)
					calls += site.calls;
			return calls;
		}

		void ClearSites()
		{
			auto& sites = GetSites();
			std::lock_guard guard(sites.lock);
			sites.sites.clear();
			sites.calls = sites.reportedCalls = 0;
		}

		void SetSelfTestRunning(bool a_running)
		{
			GetSites().selfTest = a_running;
		}
	}

	bool IsHandle(const void* a_object)
	{
		return Detail::CoreOf(a_object) != nullptr;
	}

	Info Describe(const void* a_object)
	{
		Info info;
		if (const auto* core = Detail::CoreOf(a_object)) {
			info.kind = core->kind;
			info.name = core->Name();
			info.graph = core->Graph();
			info.wrapped = core->Wrapped();
		}
		return info;
	}

	IUnknown* WrappedOf(const void* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		return core ? core->Wrapped() : nullptr;
	}

	Kind KindOf(const void* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		return core ? core->kind : Kind::Count;
	}

	namespace
	{
		/** @brief The native services for a native handle. Null for a guest handle or before native mode set them. */
		const NativeServices* NativeOf(const HandleCore& a_core)
		{
			return a_core.Wrapped() ? nullptr : Detail::nativeServices.load(std::memory_order_acquire);
		}
	}

	std::shared_ptr<const void> OwnerOf(const void* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		return core ? core->Owner() : nullptr;
	}

	DeviceHandle::~DeviceHandle()
	{
		// The context may outlive this device, so it must stop answering GetDevice with it.
		if (immediate)
			static_cast<DeviceContextHandle*>(immediate.get())->device.store(nullptr, std::memory_order_release);
	}

	void STDMETHODCALLTYPE DeviceHandle::GetImmediateContext(ID3D11DeviceContext** a_out)
	{
		if (immediate) {
			if (a_out)
				immediate.copy_to(a_out);
			return;
		}
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetImmediateContext), "GetImmediateContext", _ReturnAddress())) {
			static_cast<ID3D11Device*>(core.Wrapped())->GetImmediateContext(a_out);
			return;
		}
		Detail::Zero(a_out);
	}

	namespace
	{
		thread_local CreateTexture2DHook t_createTexture2D = nullptr;
	}

	ScopedCreateTexture2DHook::ScopedCreateTexture2DHook(CreateTexture2DHook a_hook) :
		previous(std::exchange(t_createTexture2D, a_hook))
	{}

	ScopedCreateTexture2DHook::~ScopedCreateTexture2DHook()
	{
		t_createTexture2D = previous;
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateTexture2D(const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out)
	{
		if (const auto hook = t_createTexture2D; hook && a_desc && a_out) {
			// The hook runs with no scope set, so a texture it makes through a device handle is plain.
			const ScopedCreateTexture2DHook none(nullptr);
			if (hook(*a_desc, a_data, a_out))
				return S_OK;
		}
		if (const auto* native = NativeOf(core); native && native->createTexture2D && a_desc && a_out)
			return native->createTexture2D(*a_desc, a_data, a_out);
		if (core.Enter(static_cast<std::uint16_t>(Slot::CreateTexture2D), "CreateTexture2D", _ReturnAddress()))
			return static_cast<ID3D11Device*>(core.Wrapped())->CreateTexture2D(a_desc, a_data, a_out);
		Detail::Zero(a_out);
		return Detail::Failure<HRESULT>();
	}

	namespace
	{
		/** @brief What a null view description defaults from. */
		struct ViewSource
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			UINT mips = 1, arraySize = 1, depth = 1, samples = 1;
			bool cube = false;
		};

		bool ViewSourceOf(ID3D11Resource* a_resource, ViewSource& a_out)
		{
			a_resource->GetType(&a_out.dimension);
			switch (a_out.dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				{
					D3D11_TEXTURE1D_DESC desc{};
					static_cast<ID3D11Texture1D*>(a_resource)->GetDesc(&desc);
					a_out.format = desc.Format;
					a_out.mips = desc.MipLevels ? desc.MipLevels : UINT(-1);  // 0 made the whole chain, -1 views all of it
					a_out.arraySize = desc.ArraySize;
					return true;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				{
					D3D11_TEXTURE2D_DESC desc{};
					static_cast<ID3D11Texture2D*>(a_resource)->GetDesc(&desc);
					a_out.format = desc.Format;
					a_out.mips = desc.MipLevels ? desc.MipLevels : UINT(-1);  // 0 made the whole chain, -1 views all of it
					a_out.arraySize = desc.ArraySize;
					a_out.samples = desc.SampleDesc.Count;
					a_out.cube = (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
					return true;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				{
					D3D11_TEXTURE3D_DESC desc{};
					static_cast<ID3D11Texture3D*>(a_resource)->GetDesc(&desc);
					a_out.format = desc.Format;
					a_out.mips = desc.MipLevels ? desc.MipLevels : UINT(-1);  // 0 made the whole chain, -1 views all of it
					a_out.depth = desc.Depth;
					return true;
				}
			default:
				return false;
			}
		}

		/** @name A view with a null description covers the whole texture in its own format, as D3D11 makes it. */
		/** @{ */
		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_SHADER_RESOURCE_VIEW_DESC& a_out)
		{
			ViewSource source;
			if (!ViewSourceOf(a_resource, source))
				return false;
			a_out = {};
			a_out.Format = source.format;
			const bool multisampled = source.samples > 1;
			switch (source.dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				if (source.arraySize > 1) {
					a_out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
					a_out.Texture1DArray = { 0, source.mips, 0, source.arraySize };
				} else {
					a_out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
					a_out.Texture1D = { 0, source.mips };
				}
				return true;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				a_out.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
				a_out.Texture3D = { 0, source.mips };
				return true;
			default:
				break;
			}
			if (source.cube) {
				a_out.ViewDimension = source.arraySize > 6 ? D3D11_SRV_DIMENSION_TEXTURECUBEARRAY : D3D11_SRV_DIMENSION_TEXTURECUBE;
				a_out.TextureCubeArray = { 0, source.mips, 0, source.arraySize / 6 };
			} else if (source.arraySize > 1) {
				a_out.ViewDimension = multisampled ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				if (multisampled)
					a_out.Texture2DMSArray = { 0, source.arraySize };
				else
					a_out.Texture2DArray = { 0, source.mips, 0, source.arraySize };
			} else {
				a_out.ViewDimension = multisampled ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
				a_out.Texture2D = { 0, source.mips };
			}
			return true;
		}

		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_RENDER_TARGET_VIEW_DESC& a_out)
		{
			ViewSource source;
			if (!ViewSourceOf(a_resource, source))
				return false;
			a_out = {};
			a_out.Format = source.format;
			const bool multisampled = source.samples > 1;
			switch (source.dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				if (source.arraySize > 1) {
					a_out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
					a_out.Texture1DArray = { 0, 0, source.arraySize };
				} else {
					a_out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
				}
				return true;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				a_out.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
				a_out.Texture3D = { 0, 0, source.depth };
				return true;
			default:
				break;
			}
			if (source.arraySize > 1) {
				a_out.ViewDimension = multisampled ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				if (multisampled)
					a_out.Texture2DMSArray = { 0, source.arraySize };
				else
					a_out.Texture2DArray = { 0, 0, source.arraySize };
			} else {
				a_out.ViewDimension = multisampled ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
			}
			return true;
		}

		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_DEPTH_STENCIL_VIEW_DESC& a_out)
		{
			ViewSource source;
			if (!ViewSourceOf(a_resource, source) || source.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
				return false;
			a_out = {};
			a_out.Format = source.format;
			const bool multisampled = source.samples > 1;
			if (source.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
				if (source.arraySize > 1) {
					a_out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
					a_out.Texture1DArray = { 0, 0, source.arraySize };
				} else {
					a_out.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
				}
				return true;
			}
			if (source.arraySize > 1) {
				a_out.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				if (multisampled)
					a_out.Texture2DMSArray = { 0, source.arraySize };
				else
					a_out.Texture2DArray = { 0, 0, source.arraySize };
			} else {
				a_out.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
			}
			return true;
		}

		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_UNORDERED_ACCESS_VIEW_DESC& a_out)
		{
			ViewSource source;
			if (!ViewSourceOf(a_resource, source) || source.samples > 1)
				return false;
			a_out = {};
			a_out.Format = source.format;
			switch (source.dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				if (source.arraySize > 1) {
					a_out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
					a_out.Texture1DArray = { 0, 0, source.arraySize };
				} else {
					a_out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
				}
				return true;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				a_out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
				a_out.Texture3D = { 0, 0, source.depth };
				return true;
			default:
				break;
			}
			if (source.arraySize > 1) {
				a_out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
				a_out.Texture2DArray = { 0, 0, source.arraySize };
			} else {
				a_out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			}
			return true;
		}
		/** @} */

		/** @brief A native view carries its resource's ORG resource and owner, so lookups through the view find them. */
		Options NativeViewOptions(const char* a_kind, ID3D11Resource* a_resource)
		{
			Options options;
			const auto* resource = Detail::CoreOf(a_resource);
			options.name = resource ? fmt::format("{} {}", resource->Name(), a_kind) : fmt::format("Native::{}", a_kind);
			options.graph = GraphResourceOf(a_resource);
			options.owner = OwnerOf(a_resource);
			return options;
		}

		/** @brief A native view. A null description works only on resources with a default here, else E_INVALIDARG. */
		template <class ViewDesc, class View, class Make>
		HRESULT MakeNativeView(ID3D11Resource* a_resource, const ViewDesc* a_desc, View** a_out, const char* a_kind, Make&& a_make)
		{
			if (!a_out || !a_resource)
				return E_INVALIDARG;
			*a_out = nullptr;
			ViewDesc desc{};
			if (a_desc)
				desc = *a_desc;
			else if (!DefaultViewDesc(a_resource, desc))
				return E_INVALIDARG;
			*a_out = a_make(a_resource, desc, NativeViewOptions(a_kind, a_resource)).detach();
			return S_OK;
		}

		/** @brief Options for a native non-view object. The engine's debug name replaces the name later. */
		Options NativeOptions(const char* a_kind)
		{
			Options options;
			options.name = fmt::format("Native::{}", a_kind);
			return options;
		}

		/**
		 * @name State descriptions stored as D3D11 stores them. Readers convert every target and face, so ignored parts must hold
		 * defaults rather than the zeros the engine leaves.
		 */
		/** @{ */
		D3D11_BLEND_DESC NormalizedBlend(D3D11_BLEND_DESC a_desc)
		{
			for (UINT i = 0; i < 8; ++i) {
				auto& target = a_desc.RenderTarget[i];
				// Without independent blending every target is the first one's.
				if (!a_desc.IndependentBlendEnable && i > 0)
					target = a_desc.RenderTarget[0];
				if (!target.BlendEnable) {
					target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
					target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
					target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
				}
				target.RenderTargetWriteMask &= D3D11_COLOR_WRITE_ENABLE_ALL;
			}
			return a_desc;
		}

		D3D11_DEPTH_STENCIL_DESC NormalizedDepthStencil(D3D11_DEPTH_STENCIL_DESC a_desc)
		{
			if (!a_desc.DepthEnable) {
				a_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
				a_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
			}
			if (!a_desc.StencilEnable) {
				a_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
				a_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
				for (auto* face : { &a_desc.FrontFace, &a_desc.BackFace })
					*face = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
			}
			return a_desc;
		}

		D3D11_RASTERIZER_DESC NormalizedRasterizer(D3D11_RASTERIZER_DESC a_desc)
		{
			if (!a_desc.FillMode)
				a_desc.FillMode = D3D11_FILL_SOLID;
			if (!a_desc.CullMode)
				a_desc.CullMode = D3D11_CULL_BACK;
			return a_desc;
		}

		D3D11_SAMPLER_DESC NormalizedSampler(D3D11_SAMPLER_DESC a_desc)
		{
			for (auto* address : { &a_desc.AddressU, &a_desc.AddressV, &a_desc.AddressW })
				if (!*address)
					*address = D3D11_TEXTURE_ADDRESS_CLAMP;
			if (!a_desc.ComparisonFunc)
				a_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			a_desc.MaxAnisotropy = (std::clamp)(a_desc.MaxAnisotropy, 1u, 16u);
			return a_desc;
		}
		/** @} */

		std::span<const std::uint8_t> Bytes(const void* a_data, SIZE_T a_length)
		{
			return { static_cast<const std::uint8_t*>(a_data), a_data ? static_cast<std::size_t>(a_length) : 0 };
		}
	}

	// TW_DEVICE_SITE starts a device creation method and TW_FORWARD_OR_FAIL ends it. A guest handle forwards, and a native handle
	// without a service fails like every tripwire.
#define TW_DEVICE_SITE(Name)                                                                                                     \
	void* const twCaller = _ReturnAddress();                                                                                     \
	[[maybe_unused]] const Detail::CallSite twSite{ &core, static_cast<std::uint16_t>(Slot::Name), #Name, twCaller };           \
	const NativeServices* const twNative = NativeOf(core)
#define TW_FORWARD_OR_FAIL(Name, Out, ...)                                         \
	if (core.Enter(static_cast<std::uint16_t>(Slot::Name), #Name, twCaller))      \
		return static_cast<ID3D11Device*>(core.Wrapped())->Name(__VA_ARGS__);        \
	Detail::Zero(Out);                                                             \
	return E_FAIL

	HRESULT STDMETHODCALLTYPE DeviceHandle::QueryInterface(REFIID a_riid, void** a_out)
	{
		if (const auto* native = NativeOf(core); native && native->queryDxgiDevice && a_out &&
												 (a_riid == __uuidof(IDXGIDevice) || a_riid == __uuidof(IDXGIDevice1)))
			return native->queryDxgiDevice(a_riid, a_out);
		return Detail::QueryInterface(core, static_cast<ID3D11Device*>(this), kIids, a_riid, a_out, _ReturnAddress());
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateBuffer(const D3D11_BUFFER_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Buffer** a_out)
	{
		TW_DEVICE_SITE(CreateBuffer);
		if (twNative && twNative->createBuffer && a_desc && a_out)
			return twNative->createBuffer(*a_desc, a_data, a_out);
		TW_FORWARD_OR_FAIL(CreateBuffer, a_out, a_desc, a_data, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateTexture1D(const D3D11_TEXTURE1D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture1D** a_out)
	{
		TW_DEVICE_SITE(CreateTexture1D);
		if (twNative && twNative->createTexture1D && a_desc && a_out)
			return twNative->createTexture1D(*a_desc, a_data, a_out);
		TW_FORWARD_OR_FAIL(CreateTexture1D, a_out, a_desc, a_data, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateTexture3D(const D3D11_TEXTURE3D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture3D** a_out)
	{
		TW_DEVICE_SITE(CreateTexture3D);
		if (twNative && twNative->createTexture3D && a_desc && a_out)
			return twNative->createTexture3D(*a_desc, a_data, a_out);
		TW_FORWARD_OR_FAIL(CreateTexture3D, a_out, a_desc, a_data, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateShaderResourceView(ID3D11Resource* a_resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* a_desc,
		ID3D11ShaderResourceView** a_out)
	{
		TW_DEVICE_SITE(CreateShaderResourceView);
		if (twNative && IsHandle(a_resource))
			return MakeNativeView(a_resource, a_desc, a_out, "SRV", [](ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC& d, Options&& o) {
				return ::RenderGraph::Engine::Tripwire::CreateShaderResourceView(r, d, std::move(o));
			});
		TW_FORWARD_OR_FAIL(CreateShaderResourceView, a_out, Detail::UnwrapAt(a_resource, twSite), a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateUnorderedAccessView(ID3D11Resource* a_resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* a_desc,
		ID3D11UnorderedAccessView** a_out)
	{
		TW_DEVICE_SITE(CreateUnorderedAccessView);
		if (twNative && IsHandle(a_resource))
			return MakeNativeView(a_resource, a_desc, a_out, "UAV", [](ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC& d, Options&& o) {
				return ::RenderGraph::Engine::Tripwire::CreateUnorderedAccessView(r, d, std::move(o));
			});
		TW_FORWARD_OR_FAIL(CreateUnorderedAccessView, a_out, Detail::UnwrapAt(a_resource, twSite), a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateRenderTargetView(ID3D11Resource* a_resource, const D3D11_RENDER_TARGET_VIEW_DESC* a_desc,
		ID3D11RenderTargetView** a_out)
	{
		TW_DEVICE_SITE(CreateRenderTargetView);
		if (twNative && IsHandle(a_resource))
			return MakeNativeView(a_resource, a_desc, a_out, "RTV", [](ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC& d, Options&& o) {
				return ::RenderGraph::Engine::Tripwire::CreateRenderTargetView(r, d, std::move(o));
			});
		TW_FORWARD_OR_FAIL(CreateRenderTargetView, a_out, Detail::UnwrapAt(a_resource, twSite), a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateDepthStencilView(ID3D11Resource* a_resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* a_desc,
		ID3D11DepthStencilView** a_out)
	{
		TW_DEVICE_SITE(CreateDepthStencilView);
		if (twNative && IsHandle(a_resource))
			return MakeNativeView(a_resource, a_desc, a_out, "DSV", [](ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC& d, Options&& o) {
				return ::RenderGraph::Engine::Tripwire::CreateDepthStencilView(r, d, std::move(o));
			});
		TW_FORWARD_OR_FAIL(CreateDepthStencilView, a_out, Detail::UnwrapAt(a_resource, twSite), a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* a_elements, UINT a_count, const void* a_bytecode,
		SIZE_T a_length, ID3D11InputLayout** a_out)
	{
		TW_DEVICE_SITE(CreateInputLayout);
		if (twNative && a_out && (a_elements || !a_count)) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateInputLayout(std::span<const D3D11_INPUT_ELEMENT_DESC>(a_elements, a_count), Bytes(a_bytecode, a_length),
				NativeOptions("InputLayout"))
			             .detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateInputLayout, a_out, a_elements, a_count, a_bytecode, a_length, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateVertexShader(const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage, ID3D11VertexShader** a_out)
	{
		TW_DEVICE_SITE(CreateVertexShader);
		if (twNative && a_out && a_bytecode) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateVertexShader(Bytes(a_bytecode, a_length), NativeOptions("VertexShader")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateVertexShader, a_out, a_bytecode, a_length, Detail::UnwrapAt(a_linkage, twSite), a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreatePixelShader(const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage, ID3D11PixelShader** a_out)
	{
		TW_DEVICE_SITE(CreatePixelShader);
		if (twNative && a_out && a_bytecode) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreatePixelShader(Bytes(a_bytecode, a_length), NativeOptions("PixelShader")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreatePixelShader, a_out, a_bytecode, a_length, Detail::UnwrapAt(a_linkage, twSite), a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateComputeShader(const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage, ID3D11ComputeShader** a_out)
	{
		TW_DEVICE_SITE(CreateComputeShader);
		if (twNative && a_out && a_bytecode) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateComputeShader(Bytes(a_bytecode, a_length), NativeOptions("ComputeShader")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateComputeShader, a_out, a_bytecode, a_length, Detail::UnwrapAt(a_linkage, twSite), a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateBlendState(const D3D11_BLEND_DESC* a_desc, ID3D11BlendState** a_out)
	{
		TW_DEVICE_SITE(CreateBlendState);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateBlendState(NormalizedBlend(*a_desc), NativeOptions("BlendState")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateBlendState, a_out, a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* a_desc, ID3D11DepthStencilState** a_out)
	{
		TW_DEVICE_SITE(CreateDepthStencilState);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateDepthStencilState(NormalizedDepthStencil(*a_desc), NativeOptions("DepthStencilState")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateDepthStencilState, a_out, a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateRasterizerState(const D3D11_RASTERIZER_DESC* a_desc, ID3D11RasterizerState** a_out)
	{
		TW_DEVICE_SITE(CreateRasterizerState);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateRasterizerState(NormalizedRasterizer(*a_desc), NativeOptions("RasterizerState")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateRasterizerState, a_out, a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateSamplerState(const D3D11_SAMPLER_DESC* a_desc, ID3D11SamplerState** a_out)
	{
		TW_DEVICE_SITE(CreateSamplerState);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateSamplerState(NormalizedSampler(*a_desc), NativeOptions("SamplerState")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateSamplerState, a_out, a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateQuery(const D3D11_QUERY_DESC* a_desc, ID3D11Query** a_out)
	{
		TW_DEVICE_SITE(CreateQuery);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateQuery(*a_desc, NativeOptions("Query")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateQuery, a_out, a_desc, a_out);
	}

#undef TW_FORWARD_OR_FAIL

	D3D_FEATURE_LEVEL STDMETHODCALLTYPE DeviceHandle::GetFeatureLevel()
	{
		TW_DEVICE_SITE(GetFeatureLevel);
		if (twNative)
			return D3D_FEATURE_LEVEL_11_1;
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetFeatureLevel), "GetFeatureLevel", twCaller))
			return static_cast<ID3D11Device*>(core.Wrapped())->GetFeatureLevel();
		return D3D_FEATURE_LEVEL{};
	}

	UINT STDMETHODCALLTYPE DeviceHandle::GetCreationFlags()
	{
		TW_DEVICE_SITE(GetCreationFlags);
		if (twNative)
			return 0;
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetCreationFlags), "GetCreationFlags", twCaller))
			return static_cast<ID3D11Device*>(core.Wrapped())->GetCreationFlags();
		return 0;
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::GetDeviceRemovedReason()
	{
		TW_DEVICE_SITE(GetDeviceRemovedReason);
		// A lost Vulkan device shows in BasicRHI's device-loss report and the graph's stop, not here.
		if (twNative)
			return S_OK;
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetDeviceRemovedReason), "GetDeviceRemovedReason", twCaller))
			return static_cast<ID3D11Device*>(core.Wrapped())->GetDeviceRemovedReason();
		return E_FAIL;
	}

#undef TW_DEVICE_SITE

	// The forwarding branch serves a wrapping swap chain handle.
#define TW_SWAP_CHAIN_SITE(Name)                       \
	void* const twCaller = _ReturnAddress();           \
	const NativeServices* const twNative = NativeOf(core); \
	constexpr auto twSlot = static_cast<std::uint16_t>(Slot::Name)

	HRESULT STDMETHODCALLTYPE SwapChainHandle::GetDesc(DXGI_SWAP_CHAIN_DESC* a_out)
	{
		if (!a_out)
			return E_INVALIDARG;
		// After a resize the native swap chain's size and format are the current ones.
		if (const auto* native = NativeOf(core); native && native->getDesc)
			return native->getDesc(a_out);
		*a_out = desc;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::Present(UINT a_syncInterval, UINT a_flags)
	{
		TW_SWAP_CHAIN_SITE(Present);
		if (twNative && twNative->present)
			return twNative->present(a_syncInterval, a_flags);
		if (core.Enter(twSlot, "Present", twCaller))
			return static_cast<IDXGISwapChain*>(core.Wrapped())->Present(a_syncInterval, a_flags);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::GetBuffer(UINT a_buffer, REFIID a_riid, void** a_out)
	{
		TW_SWAP_CHAIN_SITE(GetBuffer);
		if (twNative && twNative->getBuffer)
			return twNative->getBuffer(a_buffer, a_riid, a_out);
		if (core.Enter(twSlot, "GetBuffer", twCaller))
			return static_cast<IDXGISwapChain*>(core.Wrapped())->GetBuffer(a_buffer, a_riid, a_out);
		Detail::Zero(a_out);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::ResizeBuffers(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags)
	{
		TW_SWAP_CHAIN_SITE(ResizeBuffers);
		if (twNative && twNative->resizeBuffers)
			return twNative->resizeBuffers(a_count, a_width, a_height, a_format, a_flags);
		if (core.Enter(twSlot, "ResizeBuffers", twCaller))
			return static_cast<IDXGISwapChain*>(core.Wrapped())->ResizeBuffers(a_count, a_width, a_height, a_format, a_flags);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::GetFullscreenState(BOOL* a_fullscreen, IDXGIOutput** a_target)
	{
		TW_SWAP_CHAIN_SITE(GetFullscreenState);
		// Native mode presents to a borderless window only, so the engine never tries exclusive fullscreen.
		if (twNative) {
			if (a_fullscreen)
				*a_fullscreen = FALSE;
			Detail::Zero(a_target);
			return S_OK;
		}
		if (core.Enter(twSlot, "GetFullscreenState", twCaller))
			return static_cast<IDXGISwapChain*>(core.Wrapped())->GetFullscreenState(a_fullscreen, a_target);
		Detail::Zero(a_fullscreen);
		Detail::Zero(a_target);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::SetFullscreenState(BOOL a_fullscreen, IDXGIOutput* a_target)
	{
		TW_SWAP_CHAIN_SITE(SetFullscreenState);
		if (twNative) {
			if (a_fullscreen) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::info("[ORG native] the engine asked for exclusive fullscreen; native mode stays in the borderless window");
			}
			return S_OK;
		}
		if (core.Enter(twSlot, "SetFullscreenState", twCaller))
			return static_cast<IDXGISwapChain*>(core.Wrapped())->SetFullscreenState(a_fullscreen, Unwrap(a_target));
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::ResizeTarget(const DXGI_MODE_DESC* a_parameters)
	{
		TW_SWAP_CHAIN_SITE(ResizeTarget);
		// The window keeps its size. The engine's next resize follows it via WM_WINDOWPOSCHANGED, as with fake fullscreen.
		if (twNative)
			return S_OK;
		if (core.Enter(twSlot, "ResizeTarget", twCaller))
			return static_cast<IDXGISwapChain*>(core.Wrapped())->ResizeTarget(a_parameters);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::GetContainingOutput(IDXGIOutput** a_out)
	{
		TW_SWAP_CHAIN_SITE(GetContainingOutput);
		if (twNative && twNative->getContainingOutput)
			return twNative->getContainingOutput(a_out);
		if (core.Enter(twSlot, "GetContainingOutput", twCaller))
			return static_cast<IDXGISwapChain*>(core.Wrapped())->GetContainingOutput(a_out);
		Detail::Zero(a_out);
		return E_FAIL;
	}

#undef TW_SWAP_CHAIN_SITE

	HRESULT STDMETHODCALLTYPE DeviceHandle::CheckFeatureSupport(D3D11_FEATURE a_feature, void* a_data, UINT a_size)
	{
		void* const twCaller = _ReturnAddress();
		if (const auto* native = NativeOf(core); native && native->checkFeatureSupport)
			return native->checkFeatureSupport(a_feature, a_data, a_size);
		if (core.Enter(static_cast<std::uint16_t>(Slot::CheckFeatureSupport), "CheckFeatureSupport", twCaller))
			return static_cast<ID3D11Device*>(core.Wrapped())->CheckFeatureSupport(a_feature, a_data, a_size);
		Detail::ZeroBytes(a_data, a_size);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CheckFormatSupport(DXGI_FORMAT a_format, UINT* a_support)
	{
		void* const twCaller = _ReturnAddress();
		if (const auto* native = NativeOf(core); native && native->checkFormatSupport)
			return native->checkFormatSupport(a_format, a_support);
		if (core.Enter(static_cast<std::uint16_t>(Slot::CheckFormatSupport), "CheckFormatSupport", twCaller))
			return static_cast<ID3D11Device*>(core.Wrapped())->CheckFormatSupport(a_format, a_support);
		Detail::Zero(a_support);
		return E_FAIL;
	}

	namespace
	{
		bool DropsUnbacked(const char* a_method, bool a_clear, std::initializer_list<const void*> a_objects);
	}

	// The work outside draws. A guest handle forwards it to DXVK's context; with nothing to forward to, it trips.
#define TW_CONTEXT_SITE(Name)                                                                                                    \
	void* const twCaller = _ReturnAddress();                                                                                     \
	[[maybe_unused]] const Detail::CallSite twSite{ &core, static_cast<std::uint16_t>(Slot::Name), #Name, twCaller };           \
	const NativeServices* const twNative = NativeOf(core)
#define TW_CONTEXT_FORWARD(Name, ...)                                                       \
	if (auto* const twWrapped = static_cast<ID3D11DeviceContext*>(core.Wrapped())) {       \
		if (core.watchAll)                                                                  \
			core.Record(static_cast<std::uint16_t>(Slot::Name), #Name, twCaller, true, {}); \
		return twWrapped->Name(__VA_ARGS__);                                                \
	}                                                                                       \
	core.Trip(static_cast<std::uint16_t>(Slot::Name), #Name, twCaller)

	HRESULT STDMETHODCALLTYPE DeviceContextHandle::QueryInterface(REFIID a_riid, void** a_out)
	{
		if (const auto* native = NativeOf(core); native && native->queryContext && a_out && a_riid == __uuidof(ID3DUserDefinedAnnotation))
			return native->queryContext(a_riid, a_out);
		// A later context interface would bind and draw on DXVK's context past the handle, so it is refused in both modes.
		if (a_out && IsNativeUpgradeIid(a_riid)) {
			*a_out = nullptr;
			return E_NOINTERFACE;
		}
		return Detail::QueryInterface(core, static_cast<ID3D11DeviceContext*>(this), kIids, a_riid, a_out, _ReturnAddress());
	}

	HRESULT STDMETHODCALLTYPE DeviceContextHandle::Map(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, UINT a_flags,
		D3D11_MAPPED_SUBRESOURCE* a_out)
	{
		TW_CONTEXT_SITE(Map);
		if (DropsUnbacked("Map", false, { a_resource })) {
			Detail::Zero(a_out);
			return E_INVALIDARG;
		}
		if (const auto* hooks = Detail::mapHooks.load(std::memory_order_acquire); hooks && hooks->map && (twNative || core.Wrapped()) && a_out &&
			hooks->map(twNative ? a_resource : Unwrap(a_resource), a_subresource, a_type, a_flags, a_out))
			return S_OK;
		if (twNative && twNative->map)
			return twNative->map(a_resource, a_subresource, a_type, a_flags, a_out);
		TW_CONTEXT_FORWARD(Map, Detail::UnwrapAt(a_resource, twSite), a_subresource, a_type, a_flags, a_out);
		Detail::Zero(a_out);
		return E_FAIL;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::Unmap(ID3D11Resource* a_resource, UINT a_subresource)
	{
		TW_CONTEXT_SITE(Unmap);
		if (DropsUnbacked("Unmap", false, { a_resource }))
			return;
		if (const auto* hooks = Detail::mapHooks.load(std::memory_order_acquire); hooks && hooks->unmap && (twNative || core.Wrapped()) &&
			hooks->unmap(twNative ? a_resource : Unwrap(a_resource), a_subresource))
			return;
		if (twNative && twNative->unmap)
			return twNative->unmap(a_resource, a_subresource);
		TW_CONTEXT_FORWARD(Unmap, Detail::UnwrapAt(a_resource, twSite), a_subresource);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::UpdateSubresource(ID3D11Resource* a_resource, UINT a_subresource, const D3D11_BOX* a_box, const void* a_data,
		UINT a_rowPitch, UINT a_depthPitch)
	{
		TW_CONTEXT_SITE(UpdateSubresource);
		if (DropsUnbacked("UpdateSubresource", false, { a_resource }))
			return;
		if (twNative && twNative->updateSubresource)
			return twNative->updateSubresource(a_resource, a_subresource, a_box, a_data, a_rowPitch, a_depthPitch);
		TW_CONTEXT_FORWARD(UpdateSubresource, Detail::UnwrapAt(a_resource, twSite), a_subresource, a_box, a_data, a_rowPitch, a_depthPitch);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CopyResource(ID3D11Resource* a_destination, ID3D11Resource* a_source)
	{
		TW_CONTEXT_SITE(CopyResource);
		if (DropsUnbacked("CopyResource", false, { a_destination, a_source }))
			return;
		if (twNative && twNative->copyResource)
			return twNative->copyResource(a_destination, a_source);
		if (const auto hook = Detail::guestCopyHook.load(std::memory_order_acquire); hook && core.Wrapped() && hook(a_destination, a_source))
			return;
		TW_CONTEXT_FORWARD(CopyResource, Detail::UnwrapAt(a_destination, twSite), Detail::UnwrapAt(a_source, twSite));
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CopySubresourceRegion(ID3D11Resource* a_destination, UINT a_destinationSubresource, UINT a_x, UINT a_y, UINT a_z,
		ID3D11Resource* a_source, UINT a_sourceSubresource, const D3D11_BOX* a_box)
	{
		TW_CONTEXT_SITE(CopySubresourceRegion);
		if (DropsUnbacked("CopySubresourceRegion", false, { a_destination, a_source }))
			return;
		if (twNative && twNative->copySubresourceRegion)
			return twNative->copySubresourceRegion(a_destination, a_destinationSubresource, a_x, a_y, a_z, a_source, a_sourceSubresource, a_box);
		TW_CONTEXT_FORWARD(CopySubresourceRegion, Detail::UnwrapAt(a_destination, twSite), a_destinationSubresource, a_x, a_y, a_z,
			Detail::UnwrapAt(a_source, twSite), a_sourceSubresource, a_box);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::GenerateMips(ID3D11ShaderResourceView* a_view)
	{
		TW_CONTEXT_SITE(GenerateMips);
		if (DropsUnbacked("GenerateMips", false, { a_view }))
			return;
		if (twNative && twNative->generateMips)
			return twNative->generateMips(a_view);
		TW_CONTEXT_FORWARD(GenerateMips, Detail::UnwrapAt(a_view, twSite));
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearRenderTargetView(ID3D11RenderTargetView* a_view, const FLOAT a_color[4])
	{
		TW_CONTEXT_SITE(ClearRenderTargetView);
		if (DropsUnbacked("ClearRenderTargetView", true, { a_view }))
			return;
		if (twNative && twNative->clearRenderTargetView)
			return twNative->clearRenderTargetView(a_view, a_color);
		TW_CONTEXT_FORWARD(ClearRenderTargetView, Detail::UnwrapAt(a_view, twSite), a_color);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* a_view, const UINT a_values[4])
	{
		TW_CONTEXT_SITE(ClearUnorderedAccessViewUint);
		if (DropsUnbacked("ClearUnorderedAccessViewUint", true, { a_view }))
			return;
		if (twNative && twNative->clearUnorderedAccessViewUint)
			return twNative->clearUnorderedAccessViewUint(a_view, a_values);
		TW_CONTEXT_FORWARD(ClearUnorderedAccessViewUint, Detail::UnwrapAt(a_view, twSite), a_values);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* a_view, const FLOAT a_values[4])
	{
		TW_CONTEXT_SITE(ClearUnorderedAccessViewFloat);
		if (DropsUnbacked("ClearUnorderedAccessViewFloat", true, { a_view }))
			return;
		if (twNative && twNative->clearUnorderedAccessViewFloat)
			return twNative->clearUnorderedAccessViewFloat(a_view, a_values);
		TW_CONTEXT_FORWARD(ClearUnorderedAccessViewFloat, Detail::UnwrapAt(a_view, twSite), a_values);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearDepthStencilView(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
	{
		TW_CONTEXT_SITE(ClearDepthStencilView);
		if (DropsUnbacked("ClearDepthStencilView", true, { a_view }))
			return;
		if (twNative && twNative->clearDepthStencilView)
			return twNative->clearDepthStencilView(a_view, a_flags, a_depth, a_stencil);
		TW_CONTEXT_FORWARD(ClearDepthStencilView, Detail::UnwrapAt(a_view, twSite), a_flags, a_depth, a_stencil);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::Begin(ID3D11Asynchronous* a_query)
	{
		TW_CONTEXT_SITE(Begin);
		// Native mode does not measure intervals, so Begin records nothing.
		if (twNative && twNative->end)
			return;
		TW_CONTEXT_FORWARD(Begin, Detail::UnwrapAt(a_query, twSite));
	}

	void STDMETHODCALLTYPE DeviceContextHandle::End(ID3D11Asynchronous* a_query)
	{
		TW_CONTEXT_SITE(End);
		if (twNative && twNative->end)
			return twNative->end(a_query);
		TW_CONTEXT_FORWARD(End, Detail::UnwrapAt(a_query, twSite));
	}

	HRESULT STDMETHODCALLTYPE DeviceContextHandle::GetData(ID3D11Asynchronous* a_query, void* a_data, UINT a_size, UINT a_flags)
	{
		TW_CONTEXT_SITE(GetData);
		if (twNative && twNative->getData)
			return twNative->getData(a_query, a_data, a_size, a_flags);
		TW_CONTEXT_FORWARD(GetData, Detail::UnwrapAt(a_query, twSite), a_data, a_size, a_flags);
		Detail::ZeroBytes(a_data, a_size);
		return E_FAIL;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSSetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_items)
	{
		Store(bindings.vs.srvs, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSGetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView** a_out)
	{
		Load(bindings.vs.srvs, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSSetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_items)
	{
		Store(bindings.ps.srvs, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSGetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView** a_out)
	{
		Load(bindings.ps.srvs, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSSetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_items)
	{
		Store(bindings.cs.srvs, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSGetShaderResources(UINT a_start, UINT a_count, ID3D11ShaderResourceView** a_out)
	{
		Load(bindings.cs.srvs, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSSetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_items)
	{
		Store(bindings.vs.cbs, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSGetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_out)
	{
		Load(bindings.vs.cbs, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSSetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_items)
	{
		Store(bindings.ps.cbs, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSGetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_out)
	{
		Load(bindings.ps.cbs, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSSetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_items)
	{
		Store(bindings.cs.cbs, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSGetConstantBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_out)
	{
		Load(bindings.cs.cbs, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSSetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState* const* a_items)
	{
		Store(bindings.vs.samplers, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSGetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState** a_out)
	{
		Load(bindings.vs.samplers, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSSetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState* const* a_items)
	{
		Store(bindings.ps.samplers, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSGetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState** a_out)
	{
		Load(bindings.ps.samplers, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSSetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState* const* a_items)
	{
		Store(bindings.cs.samplers, a_start, a_count, a_items);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSGetSamplers(UINT a_start, UINT a_count, ID3D11SamplerState** a_out)
	{
		Load(bindings.cs.samplers, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSSetShader(ID3D11VertexShader* a_shader, ID3D11ClassInstance* const*, UINT)
	{
		std::lock_guard guard(bindings.lock);
		bindings.vsShader.copy_from(a_shader);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::VSGetShader(ID3D11VertexShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount)
	{
		// The handle has no class linkage.
		if (a_instances && a_instanceCount)
			Detail::ZeroN(a_instances, *a_instanceCount);
		Detail::Zero(a_instanceCount);
		std::lock_guard guard(bindings.lock);
		if (a_out)
			*a_out = nullptr;
		if (a_out && bindings.vsShader)
			bindings.vsShader.copy_to(a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSSetShader(ID3D11PixelShader* a_shader, ID3D11ClassInstance* const*, UINT)
	{
		std::lock_guard guard(bindings.lock);
		bindings.psShader.copy_from(a_shader);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::PSGetShader(ID3D11PixelShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount)
	{
		// The handle has no class linkage.
		if (a_instances && a_instanceCount)
			Detail::ZeroN(a_instances, *a_instanceCount);
		Detail::Zero(a_instanceCount);
		std::lock_guard guard(bindings.lock);
		if (a_out)
			*a_out = nullptr;
		if (a_out && bindings.psShader)
			bindings.psShader.copy_to(a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSSetShader(ID3D11ComputeShader* a_shader, ID3D11ClassInstance* const*, UINT)
	{
		std::lock_guard guard(bindings.lock);
		bindings.csShader.copy_from(a_shader);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSGetShader(ID3D11ComputeShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount)
	{
		// The handle has no class linkage.
		if (a_instances && a_instanceCount)
			Detail::ZeroN(a_instances, *a_instanceCount);
		Detail::Zero(a_instanceCount);
		std::lock_guard guard(bindings.lock);
		if (a_out)
			*a_out = nullptr;
		if (a_out && bindings.csShader)
			bindings.csShader.copy_to(a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::GSSetShader(ID3D11GeometryShader* a_shader, ID3D11ClassInstance* const*, UINT)
	{
		std::lock_guard guard(bindings.lock);
		bindings.gsShader.copy_from(a_shader);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::GSGetShader(ID3D11GeometryShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount)
	{
		// The handle has no class linkage.
		if (a_instances && a_instanceCount)
			Detail::ZeroN(a_instances, *a_instanceCount);
		Detail::Zero(a_instanceCount);
		std::lock_guard guard(bindings.lock);
		if (a_out)
			*a_out = nullptr;
		if (a_out && bindings.gsShader)
			bindings.gsShader.copy_to(a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::HSSetShader(ID3D11HullShader* a_shader, ID3D11ClassInstance* const*, UINT)
	{
		std::lock_guard guard(bindings.lock);
		bindings.hsShader.copy_from(a_shader);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::HSGetShader(ID3D11HullShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount)
	{
		// The handle has no class linkage.
		if (a_instances && a_instanceCount)
			Detail::ZeroN(a_instances, *a_instanceCount);
		Detail::Zero(a_instanceCount);
		std::lock_guard guard(bindings.lock);
		if (a_out)
			*a_out = nullptr;
		if (a_out && bindings.hsShader)
			bindings.hsShader.copy_to(a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::DSSetShader(ID3D11DomainShader* a_shader, ID3D11ClassInstance* const*, UINT)
	{
		std::lock_guard guard(bindings.lock);
		bindings.dsShader.copy_from(a_shader);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::DSGetShader(ID3D11DomainShader** a_out, ID3D11ClassInstance** a_instances, UINT* a_instanceCount)
	{
		// The handle has no class linkage.
		if (a_instances && a_instanceCount)
			Detail::ZeroN(a_instances, *a_instanceCount);
		Detail::Zero(a_instanceCount);
		std::lock_guard guard(bindings.lock);
		if (a_out)
			*a_out = nullptr;
		if (a_out && bindings.dsShader)
			bindings.dsShader.copy_to(a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSSetUnorderedAccessViews(UINT a_start, UINT a_count, ID3D11UnorderedAccessView* const* a_views, const UINT*)
	{
		Store(bindings.csUavs, a_start, a_count, a_views);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CSGetUnorderedAccessViews(UINT a_start, UINT a_count, ID3D11UnorderedAccessView** a_out)
	{
		Load(bindings.csUavs, a_start, a_count, a_out);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::OMSetRenderTargets(UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth)
	{
		// D3D11 unbinds every target past a_count.
		std::lock_guard guard(bindings.lock);
		for (UINT i = 0; i < bindings.rtvs.size(); ++i)
			bindings.rtvs[i].copy_from(a_views && i < a_count ? a_views[i] : nullptr);
		bindings.dsv.copy_from(a_depth);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::OMGetRenderTargets(UINT a_count, ID3D11RenderTargetView** a_views, ID3D11DepthStencilView** a_depth)
	{
		Load(bindings.rtvs, 0, a_count, a_views);
		std::lock_guard guard(bindings.lock);
		if (a_depth) {
			*a_depth = nullptr;
			if (bindings.dsv)
				bindings.dsv.copy_to(a_depth);
		}
	}

	void STDMETHODCALLTYPE DeviceContextHandle::OMSetBlendState(ID3D11BlendState* a_state, const FLOAT a_factor[4], UINT a_sampleMask)
	{
		std::lock_guard guard(bindings.lock);
		bindings.blend.copy_from(a_state);
		for (int i = 0; i < 4; ++i)
			bindings.blendFactor[i] = a_factor ? a_factor[i] : 1.0f;
		bindings.sampleMask = a_sampleMask;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::OMGetBlendState(ID3D11BlendState** a_state, FLOAT a_factor[4], UINT* a_sampleMask)
	{
		std::lock_guard guard(bindings.lock);
		if (a_state) {
			*a_state = nullptr;
			if (bindings.blend)
				bindings.blend.copy_to(a_state);
		}
		if (a_factor)
			for (int i = 0; i < 4; ++i)
				a_factor[i] = bindings.blendFactor[i];
		if (a_sampleMask)
			*a_sampleMask = bindings.sampleMask;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::OMSetDepthStencilState(ID3D11DepthStencilState* a_state, UINT a_stencilRef)
	{
		std::lock_guard guard(bindings.lock);
		bindings.depthStencil.copy_from(a_state);
		bindings.stencilRef = a_stencilRef;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::OMGetDepthStencilState(ID3D11DepthStencilState** a_state, UINT* a_stencilRef)
	{
		std::lock_guard guard(bindings.lock);
		if (a_state) {
			*a_state = nullptr;
			if (bindings.depthStencil)
				bindings.depthStencil.copy_to(a_state);
		}
		if (a_stencilRef)
			*a_stencilRef = bindings.stencilRef;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::RSSetState(ID3D11RasterizerState* a_state)
	{
		std::lock_guard guard(bindings.lock);
		bindings.rasterizer.copy_from(a_state);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::RSGetState(ID3D11RasterizerState** a_state)
	{
		std::lock_guard guard(bindings.lock);
		if (a_state) {
			*a_state = nullptr;
			if (bindings.rasterizer)
				bindings.rasterizer.copy_to(a_state);
		}
	}

	void STDMETHODCALLTYPE DeviceContextHandle::RSSetViewports(UINT a_count, const D3D11_VIEWPORT* a_viewports)
	{
		std::lock_guard guard(bindings.lock);
		bindings.viewportCount = a_viewports ? (std::min)(a_count, static_cast<UINT>(bindings.viewports.size())) : 0;
		for (UINT i = 0; i < bindings.viewportCount; ++i)
			bindings.viewports[i] = a_viewports[i];
	}

	void STDMETHODCALLTYPE DeviceContextHandle::RSGetViewports(UINT* a_count, D3D11_VIEWPORT* a_viewports)
	{
		std::lock_guard guard(bindings.lock);
		if (!a_count)
			return;
		if (!a_viewports) {
			*a_count = bindings.viewportCount;
			return;
		}
		for (UINT i = 0; i < *a_count; ++i)
			a_viewports[i] = i < bindings.viewportCount ? bindings.viewports[i] : D3D11_VIEWPORT{};
		*a_count = (std::min)(*a_count, bindings.viewportCount);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::RSSetScissorRects(UINT a_count, const D3D11_RECT* a_rects)
	{
		std::lock_guard guard(bindings.lock);
		bindings.scissorCount = a_rects ? (std::min)(a_count, static_cast<UINT>(bindings.scissors.size())) : 0;
		for (UINT i = 0; i < bindings.scissorCount; ++i)
			bindings.scissors[i] = a_rects[i];
	}

	void STDMETHODCALLTYPE DeviceContextHandle::RSGetScissorRects(UINT* a_count, D3D11_RECT* a_rects)
	{
		std::lock_guard guard(bindings.lock);
		if (!a_count)
			return;
		if (!a_rects) {
			*a_count = bindings.scissorCount;
			return;
		}
		for (UINT i = 0; i < *a_count; ++i)
			a_rects[i] = i < bindings.scissorCount ? bindings.scissors[i] : D3D11_RECT{};
		*a_count = (std::min)(*a_count, bindings.scissorCount);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IASetInputLayout(ID3D11InputLayout* a_layout)
	{
		std::lock_guard guard(bindings.lock);
		bindings.inputLayout.copy_from(a_layout);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IAGetInputLayout(ID3D11InputLayout** a_layout)
	{
		std::lock_guard guard(bindings.lock);
		if (a_layout) {
			*a_layout = nullptr;
			if (bindings.inputLayout)
				bindings.inputLayout.copy_to(a_layout);
		}
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY a_topology)
	{
		bindings.topology = a_topology;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* a_topology)
	{
		if (a_topology)
			*a_topology = bindings.topology;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IASetVertexBuffers(UINT a_start, UINT a_count, ID3D11Buffer* const* a_buffers, const UINT* a_strides,
		const UINT* a_offsets)
	{
		std::lock_guard guard(bindings.lock);
		for (UINT i = 0; i < a_count && a_start + i < bindings.vertexBuffers.size(); ++i) {
			bindings.vertexBuffers[a_start + i].copy_from(a_buffers ? a_buffers[i] : nullptr);
			bindings.strides[a_start + i] = a_strides ? a_strides[i] : 0;
			bindings.offsets[a_start + i] = a_offsets ? a_offsets[i] : 0;
		}
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IAGetVertexBuffers(UINT a_start, UINT a_count, ID3D11Buffer** a_buffers, UINT* a_strides, UINT* a_offsets)
	{
		Load(bindings.vertexBuffers, a_start, a_count, a_buffers);
		std::lock_guard guard(bindings.lock);
		for (UINT i = 0; i < a_count; ++i) {
			const bool bound = a_start + i < bindings.vertexBuffers.size();
			if (a_strides)
				a_strides[i] = bound ? bindings.strides[a_start + i] : 0;
			if (a_offsets)
				a_offsets[i] = bound ? bindings.offsets[a_start + i] : 0;
		}
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IASetIndexBuffer(ID3D11Buffer* a_buffer, DXGI_FORMAT a_format, UINT a_offset)
	{
		std::lock_guard guard(bindings.lock);
		bindings.indexBuffer.copy_from(a_buffer);
		bindings.indexFormat = a_format;
		bindings.indexOffset = a_offset;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::IAGetIndexBuffer(ID3D11Buffer** a_buffer, DXGI_FORMAT* a_format, UINT* a_offset)
	{
		std::lock_guard guard(bindings.lock);
		if (a_buffer) {
			*a_buffer = nullptr;
			if (bindings.indexBuffer)
				bindings.indexBuffer.copy_to(a_buffer);
		}
		if (a_format)
			*a_format = bindings.indexFormat;
		if (a_offset)
			*a_offset = bindings.indexOffset;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearState()
	{
		// Only the kept binds. DXVK's context holds CS's own state in guest mode.
		bindings.Reset();
	}

	void DeviceContextHandle::Bindings::Reset()
	{
		std::lock_guard guard(lock);
		for (auto* stage : { &vs, &ps, &cs }) {
			for (auto& view : stage->srvs)
				view = nullptr;
			for (auto& buffer : stage->cbs)
				buffer = nullptr;
			for (auto& sampler : stage->samplers)
				sampler = nullptr;
		}
		vsShader = nullptr;
		psShader = nullptr;
		csShader = nullptr;
		gsShader = nullptr;
		hsShader = nullptr;
		dsShader = nullptr;
		for (auto& view : csUavs)
			view = nullptr;
		for (auto& view : rtvs)
			view = nullptr;
		dsv = nullptr;
		blend = nullptr;
		blendFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
		sampleMask = 0xffffffffu;
		depthStencil = nullptr;
		stencilRef = 0;
		rasterizer = nullptr;
		viewportCount = scissorCount = 0;
		inputLayout = nullptr;
		topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		for (auto& buffer : vertexBuffers)
			buffer = nullptr;
		strides = {};
		offsets = {};
		indexBuffer = nullptr;
		indexFormat = DXGI_FORMAT_UNKNOWN;
		indexOffset = 0;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::Flush()
	{
		TW_CONTEXT_SITE(Flush);
		if (twNative)
			return;
		TW_CONTEXT_FORWARD(Flush);
	}

#undef TW_CONTEXT_FORWARD
#undef TW_CONTEXT_SITE

	void STDMETHODCALLTYPE DeviceContextHandle::GetDevice(ID3D11Device** a_out)
	{
		if (auto* paired = device.load(std::memory_order_acquire)) {
			if (a_out) {
				paired->AddRef();
				*a_out = paired;
			}
			return;
		}
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetDevice), "GetDevice", _ReturnAddress())) {
			static_cast<ID3D11DeviceContext*>(core.Wrapped())->GetDevice(a_out);
			return;
		}
		Detail::Zero(a_out);
	}

	bool PairDeviceAndContext(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		const auto* deviceCore = Detail::CoreOf(a_device);
		const auto* contextCore = Detail::CoreOf(a_context);
		if (!deviceCore || deviceCore->kind != Kind::Device || !contextCore || contextCore->kind != Kind::DeviceContext)
			return false;
		static_cast<DeviceHandle*>(a_device)->immediate.copy_from(a_context);
		static_cast<DeviceContextHandle*>(a_context)->device.store(a_device, std::memory_order_release);
		return true;
	}

	std::shared_ptr<org::Resource> GraphResourceOf(const void* a_object)
	{
		if (!a_object)
			return {};
		// A native handle without its own resource may carry one from AttachGraphResource.
		if (const auto* core = Detail::CoreOf(a_object); core && (core->Graph() || core->Wrapped() || !NativeServicesInstalled()))
			return core->Graph();
		const auto child = AsDeviceChild(a_object);
		if (!child)
			return {};
		winrt::com_ptr<IUnknown> stored;
		UINT size = sizeof(IUnknown*);
		if (FAILED(child->GetPrivateData(kGraphTagGuid, &size, stored.put_void())) || size != sizeof(IUnknown*) || !stored)
			return {};
		winrt::com_ptr<IUnknown> tag;
		if (FAILED(stored->QueryInterface(kGraphTagGuid, tag.put_void())) || !tag)
			return {};
		return static_cast<GraphTag*>(tag.get())->Graph();
	}

	bool AttachGraphResource(void* a_object, std::shared_ptr<org::Resource> a_graph, std::shared_ptr<const void> a_owner)
	{
		if (!a_object || (!a_graph && !a_owner))
			return false;
		const auto child = AsDeviceChild(a_object);
		if (!child) {
			logger::warn("[ORG] tripwire: AttachGraphResource got an object that is not a D3D11 device child");
			return false;
		}
		winrt::com_ptr<IUnknown> tag;
		tag.attach(new GraphTag(std::move(a_graph), std::move(a_owner)));
		// If the object refuses the tag, it dies here and what it holds still goes through ServiceRetired.
		if (FAILED(child->SetPrivateDataInterface(kGraphTagGuid, tag.get()))) {
			logger::warn("[ORG] tripwire: AttachGraphResource: the object refused private data");
			return false;
		}
		return true;
	}

	std::span<const std::uint8_t> ShaderBytecode(const void* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		if (!core)
			return {};
		auto* object = const_cast<IUnknown*>(static_cast<const IUnknown*>(a_object));
		switch (core->kind) {
		case Kind::VertexShader:
			return static_cast<VertexShaderHandle*>(static_cast<ID3D11VertexShader*>(object))->bytecode;
		case Kind::PixelShader:
			return static_cast<PixelShaderHandle*>(static_cast<ID3D11PixelShader*>(object))->bytecode;
		case Kind::ComputeShader:
			return static_cast<ComputeShaderHandle*>(static_cast<ID3D11ComputeShader*>(object))->bytecode;
		default:
			return {};
		}
	}

	std::span<const D3D11_INPUT_ELEMENT_DESC> InputElements(const void* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		if (!core || core->kind != Kind::InputLayout)
			return {};
		auto* object = const_cast<IUnknown*>(static_cast<const IUnknown*>(a_object));
		return static_cast<InputLayoutHandle*>(static_cast<ID3D11InputLayout*>(object))->elements;
	}

	IUnknown* UnwrapObject(IUnknown* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		if (!core)
			return a_object;
		if (!core->Wrapped()) {
			// Native mode has no D3D11 below the handles, so the handle is the object.
			if (Native::Active())
				return a_object;
			// D3D11 gets null rather than a native handle it would cast to its own class.
			static std::array<std::atomic<bool>, kKinds> warned{};
			if (!warned[static_cast<std::size_t>(core->kind)].exchange(true))
				logger::warn("[ORG] tripwire: a native {} (\"{}\") was passed to D3D11, which gets null instead", InterfaceName(core->kind), core->Name());
		}
		return core->Wrapped();
	}

	namespace
	{
		/** @brief A view made over an unbacked resource with nothing of its own to forward to is unbacked too. */
		template <class I>
		winrt::com_ptr<I> MarkUnbackedView(winrt::com_ptr<I> a_view, ID3D11Resource* a_resource, const void* a_wrap)
		{
			if (a_view && !a_wrap && a_resource && IsUnbacked(a_resource))
				Detail::CoreOf(a_view.get())->unbacked.store(true, std::memory_order_release);
			return a_view;
		}
	}

	winrt::com_ptr<ID3D11Texture1D> CreateTexture1D(const D3D11_TEXTURE1D_DESC& a_desc, Options a_options, ID3D11Texture1D* a_wrap)
	{
		return Texture1DHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11Texture2D> CreateTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, Options a_options, ID3D11Texture2D* a_wrap)
	{
		return Texture2DHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11Texture3D> CreateTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, Options a_options, ID3D11Texture3D* a_wrap)
	{
		return Texture3DHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11Buffer> CreateBuffer(const D3D11_BUFFER_DESC& a_desc, Options a_options, ID3D11Buffer* a_wrap)
	{
		return BufferHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11ShaderResourceView> CreateShaderResourceView(ID3D11Resource* a_resource, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_desc, Options a_options,
		ID3D11ShaderResourceView* a_wrap)
	{
		return MarkUnbackedView(ShaderResourceViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc), a_resource, a_wrap);
	}
	winrt::com_ptr<ID3D11RenderTargetView> CreateRenderTargetView(ID3D11Resource* a_resource, const D3D11_RENDER_TARGET_VIEW_DESC& a_desc, Options a_options,
		ID3D11RenderTargetView* a_wrap)
	{
		return MarkUnbackedView(RenderTargetViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc), a_resource, a_wrap);
	}
	winrt::com_ptr<ID3D11DepthStencilView> CreateDepthStencilView(ID3D11Resource* a_resource, const D3D11_DEPTH_STENCIL_VIEW_DESC& a_desc, Options a_options,
		ID3D11DepthStencilView* a_wrap)
	{
		return MarkUnbackedView(DepthStencilViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc), a_resource, a_wrap);
	}
	winrt::com_ptr<ID3D11UnorderedAccessView> CreateUnorderedAccessView(ID3D11Resource* a_resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc, Options a_options,
		ID3D11UnorderedAccessView* a_wrap)
	{
		return MarkUnbackedView(UnorderedAccessViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc), a_resource, a_wrap);
	}
	winrt::com_ptr<ID3D11VertexShader> CreateVertexShader(std::span<const std::uint8_t> a_bytecode, Options a_options, ID3D11VertexShader* a_wrap)
	{
		return VertexShaderHandle::Make(std::move(a_options), a_wrap, a_bytecode);
	}
	winrt::com_ptr<ID3D11PixelShader> CreatePixelShader(std::span<const std::uint8_t> a_bytecode, Options a_options, ID3D11PixelShader* a_wrap)
	{
		return PixelShaderHandle::Make(std::move(a_options), a_wrap, a_bytecode);
	}
	winrt::com_ptr<ID3D11ComputeShader> CreateComputeShader(std::span<const std::uint8_t> a_bytecode, Options a_options, ID3D11ComputeShader* a_wrap)
	{
		return ComputeShaderHandle::Make(std::move(a_options), a_wrap, a_bytecode);
	}
	winrt::com_ptr<ID3D11InputLayout> CreateInputLayout(std::span<const D3D11_INPUT_ELEMENT_DESC> a_elements, std::span<const std::uint8_t> a_vsBytecode,
		Options a_options, ID3D11InputLayout* a_wrap)
	{
		return InputLayoutHandle::Make(std::move(a_options), a_wrap, a_elements, a_vsBytecode);
	}
	winrt::com_ptr<ID3D11BlendState> CreateBlendState(const D3D11_BLEND_DESC& a_desc, Options a_options, ID3D11BlendState* a_wrap)
	{
		return BlendStateHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11DepthStencilState> CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC& a_desc, Options a_options, ID3D11DepthStencilState* a_wrap)
	{
		return DepthStencilStateHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11RasterizerState> CreateRasterizerState(const D3D11_RASTERIZER_DESC& a_desc, Options a_options, ID3D11RasterizerState* a_wrap)
	{
		return RasterizerStateHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11SamplerState> CreateSamplerState(const D3D11_SAMPLER_DESC& a_desc, Options a_options, ID3D11SamplerState* a_wrap)
	{
		return SamplerStateHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11Query> CreateQuery(const D3D11_QUERY_DESC& a_desc, Options a_options, ID3D11Query* a_wrap)
	{
		return QueryHandle::Make(std::move(a_options), a_wrap, a_desc);
	}
	winrt::com_ptr<ID3D11DeviceContext> CreateDeviceContext(Options a_options, ID3D11DeviceContext* a_wrap)
	{
		return DeviceContextHandle::Make(std::move(a_options), a_wrap);
	}
	winrt::com_ptr<ID3D11Device> CreateDevice(Options a_options, ID3D11Device* a_wrap)
	{
		return DeviceHandle::Make(std::move(a_options), a_wrap);
	}
	winrt::com_ptr<IDXGISwapChain> CreateSwapChain(const DXGI_SWAP_CHAIN_DESC& a_desc, Options a_options, IDXGISwapChain* a_wrap)
	{
		return SwapChainHandle::Make(std::move(a_options), a_wrap, a_desc);
	}

	winrt::com_ptr<ID3D11Texture2D> CreateUnbackedTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, Options a_options)
	{
		auto handle = Texture2DHandle::Make(std::move(a_options), nullptr, a_desc);
		Detail::CoreOf(handle.get())->unbacked.store(true, std::memory_order_release);
		return handle;
	}

	winrt::com_ptr<ID3D11Texture3D> CreateUnbackedTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, Options a_options)
	{
		auto handle = Texture3DHandle::Make(std::move(a_options), nullptr, a_desc);
		Detail::CoreOf(handle.get())->unbacked.store(true, std::memory_order_release);
		return handle;
	}

	bool IsUnbacked(const void* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		if (!core)
			return false;
		if (core->unbacked.load(std::memory_order_acquire))
			return true;
		return false;
	}

	namespace
	{
		std::atomic<std::uint64_t> g_unbackedDrops{ 0 };

		/** @brief Whether the calling thread is the render owner inside the frame's adoption point. The self-test is let through. */
		bool AtAdoptionPoint()
		{
			if (GetSites().selfTest.load(std::memory_order_relaxed))
				return true;
			return Frame::Boundary::InAdoptionPoint();
		}

		/** @brief Whether a plain D3D11 object already carries a graph tag, which a second AttachGraphResource would replace. */
		bool HasGraphTag(const void* a_object)
		{
			const auto child = AsDeviceChild(a_object);
			if (!child)
				return false;
			winrt::com_ptr<IUnknown> stored;
			UINT size = sizeof(IUnknown*);
			return SUCCEEDED(child->GetPrivateData(kGraphTagGuid, &size, stored.put_void())) && stored;
		}

		template <class HandleT, class Interface, class Desc>
		void RetargetAs(Interface* a_handle, Target<Desc>&& a_target)
		{
			auto* core = Detail::CoreOf(a_handle);
			if (!core)
				RenderGraph::Fatal("tripwire retarget", "the object is not a tripwire handle");
			if (core->kind != HandleT::kKind)
				RenderGraph::Fatal("tripwire retarget", fmt::format("handle \"{}\" is a {}, not a {}", core->Name(), InterfaceName(core->kind), InterfaceName(HandleT::kKind)));
			if (!AtAdoptionPoint())
				RenderGraph::Fatal("tripwire retarget", fmt::format("handle \"{}\" was retargeted outside the frame's adoption point, or off the render owner", core->Name()));
			IUnknown* next = a_target.wrap;
			if (next && Detail::CoreOf(next) == core)
				RenderGraph::Fatal("tripwire retarget", fmt::format("handle \"{}\" was retargeted at itself", core->Name()));
			if (next) {
				next->AddRef();
				// As Init: the wrapped object carries the ORG resource too, unless it has a tag of its own already.
				if ((a_target.graph || a_target.owner) && !Detail::CoreOf(next) && !HasGraphTag(next))
					(void)AttachGraphResource(next, a_target.graph, a_target.owner);
			}
			const HandleState* fresh = a_target.graph || a_target.owner ? new HandleState{ std::move(a_target.graph), std::move(a_target.owner) } : nullptr;
			static_cast<HandleT*>(a_handle)->SetDesc(a_target.desc);
			const HandleState* oldState = core->state.exchange(fresh, std::memory_order_acq_rel);
			IUnknown* oldWrap = core->wrapped.exchange(next, std::memory_order_acq_rel);
			core->unbacked.store(next == nullptr, std::memory_order_release);
			// DXVK and the GPU may still use the old ones, so they go through ServiceRetired.
			if (oldWrap)
				RetireObject(std::shared_ptr<const void>(oldWrap, [](const void* a_object) { static_cast<IUnknown*>(const_cast<void*>(a_object))->Release(); }));
			if (oldState)
				RetireObject(std::shared_ptr<const HandleState>(oldState));
		}

		/**
		 * @brief Drops a context call that passes an unbacked handle, in both graph modes, before it reaches DXVK or a native
		 * service. Clears go silently, other work is a tap error.
		 */
		bool DropsUnbacked(const char* a_method, bool a_clear, std::initializer_list<const void*> a_objects)
		{
			for (const void* object : a_objects) {
				if (!object || !IsUnbacked(object))
					continue;
				g_unbackedDrops.fetch_add(1, std::memory_order_relaxed);
				if (!a_clear) {
					const auto name = Describe(object).name;
					Frame::Driver::NoteUnbackedUse(name, fmt::format("{} on the unbacked \"{}\" through the {} context", a_method, name, Frame::Driver::ModeName()));
				}
				return true;
			}
			return false;
		}
	}

	void Retarget(ID3D11Texture2D* a_handle, Target<D3D11_TEXTURE2D_DESC> a_target) { RetargetAs<Texture2DHandle>(a_handle, std::move(a_target)); }
	void Retarget(ID3D11Texture3D* a_handle, Target<D3D11_TEXTURE3D_DESC> a_target) { RetargetAs<Texture3DHandle>(a_handle, std::move(a_target)); }
	void Retarget(ID3D11ShaderResourceView* a_handle, Target<D3D11_SHADER_RESOURCE_VIEW_DESC> a_target)
	{
		RetargetAs<ShaderResourceViewHandle>(a_handle, std::move(a_target));
	}
	void Retarget(ID3D11RenderTargetView* a_handle, Target<D3D11_RENDER_TARGET_VIEW_DESC> a_target)
	{
		RetargetAs<RenderTargetViewHandle>(a_handle, std::move(a_target));
	}
	void Retarget(ID3D11DepthStencilView* a_handle, Target<D3D11_DEPTH_STENCIL_VIEW_DESC> a_target)
	{
		RetargetAs<DepthStencilViewHandle>(a_handle, std::move(a_target));
	}
	void Retarget(ID3D11UnorderedAccessView* a_handle, Target<D3D11_UNORDERED_ACCESS_VIEW_DESC> a_target)
	{
		RetargetAs<UnorderedAccessViewHandle>(a_handle, std::move(a_target));
	}

	void MarkEngineBindingsDirty()
	{
		if (!CS_ENGINE_READ(ShadowState, RecordTime, globals::game::shadowState))
			return;
		auto& state = CS_ENGINE_READ(ShadowState, RecordTime, globals::game::shadowState)->GetRuntimeData();
		state.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET, RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
		state.PSResourceModifiedBits = 0xFFFF;
		state.CSResourceModifiedBits = 0xFFFF;
		state.CSUAVModifiedBits = 0xFF;
	}

	std::uint64_t UnbackedDrops()
	{
		return g_unbackedDrops.load(std::memory_order_relaxed);
	}

	std::vector<std::shared_ptr<const void>> TakeRetired()
	{
		auto& retired = GetRetired();
		std::lock_guard guard(retired.lock);
		return std::exchange(retired.owners, {});
	}

	void ServiceRetired()
	{
		auto owners = TakeRetired();
		auto& runtime = RenderGraphRuntime::Get();
		for (auto& owner : owners)
			runtime.ReleaseAfterD3D11(std::move(owner));
	}

	void Report(std::string_view a_reason)
	{
		if (!LogRequested())
			return;
		auto& sites = GetSites();
		std::lock_guard guard(sites.lock);
		ReportLocked(sites, a_reason, true);
	}

	void Start()
	{
		if (SelfTest::Requested())
			SelfTest::Run();
		if (!LogRequested())
			return;
		const auto seconds = Switches::Number("CS_ORG_TRIPWIRE_LOG_INTERVAL", 60);
		logger::info("[ORG] tripwire log on (CS_ORG_TRIPWIRE_LOG=1): a report {}", seconds ? fmt::format("every {} s when calls changed, and at exit", seconds) : std::string("at exit"));
		if (seconds)
			GetReporter().thread = std::jthread(ReportLoop, std::chrono::seconds(seconds));
	}

	void Stop()
	{
		auto& reporter = GetReporter();
		if (reporter.thread.joinable()) {
			reporter.thread.request_stop();
			reporter.thread.join();
		}
		Report("exit");
	}
}
