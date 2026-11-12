#include "RenderGraph/Engine/Tripwire/TripwireHandles.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include "RenderGraph/Engine/Tripwire/TripwireSelfTest.h"
#include "RenderGraph/Switches.h"
#include "Utils/D3D.h"
#include "RenderGraph/Engine/Native/Native.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/RenderGraphRuntime.h"
#endif

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

		// CS_ORG_TRIPWIRE_WATCH_ALL=1: guest resources, views, shaders, states and queries log the first use of every method per
		// call site, whatever their WatchSet says. The device, the context and the swap chain keep theirs: their calls are the hot path.
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

		/** @brief The address range of the game executable's image. */
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

		// The Address Library ID of the function holding a game offset, as DCLF's HookSites names one: the nearest ID at or before it,
		// within the longest function the engine has.
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
		// Never destroyed: a guest object's tag can die after static destruction began (DXVK releasing what it still holds).
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

		// {6B4C2A7E-3D51-4F0B-9E27-8A1C5D0F3B64}: the private-data slot, and the IID, of the tag AttachGraphResource puts on an object.
		constexpr GUID kGraphTagGuid = { 0x6b4c2a7e, 0x3d51, 0x4f0b, { 0x9e, 0x27, 0x8a, 0x1c, 0x5d, 0x0f, 0x3b, 0x64 } };

		/** @brief Holds an ORG resource and its owner for as long as the D3D11 object that keeps it in private data lives. */
		class GraphTag final : public IUnknown
		{
		public:
			GraphTag(std::shared_ptr<org::Resource> a_graph, std::shared_ptr<const void> a_owner) :
				graph(std::move(a_graph)), owner(std::move(a_owner)) {}
			GraphTag(const GraphTag&) = delete;
			GraphTag& operator=(const GraphTag&) = delete;
			// The object that held the tag is gone, on whichever thread dropped it. The GPU free waits for ServiceRetired.
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

		/** @brief a_object's ID3D11DeviceChild interface (a reference added), or null when it has none. a_object is a COM object. */
		winrt::com_ptr<ID3D11DeviceChild> AsDeviceChild(const void* a_object)
		{
			winrt::com_ptr<ID3D11DeviceChild> child;
			if (a_object)
				(void)static_cast<IUnknown*>(const_cast<void*>(a_object))->QueryInterface(__uuidof(ID3D11DeviceChild), child.put_void());
			return child;
		}

		/** @brief D3D11 object interfaces outside a handle's chain: the real object refuses them too, so a refusal is no tripwire. */
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

		// a_full lists every site (at exit). Otherwise only the sites that are new or were called again since the last report are
		// listed, so a long tour's periodic reports stay short.
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

		/** @brief The periodic report's thread, stopped and joined by Stop. */
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

	WatchSet WatchSet::All()
	{
		WatchSet set;
		set.bits.set();
		return set;
	}

	WatchSet WatchSet::Of(Kind a_kind, std::initializer_list<std::string_view> a_methods)
	{
		WatchSet set;
		const auto names = SlotNames(a_kind);
		for (const auto method : a_methods) {
			const auto found = std::find_if(names.begin(), names.end(), [&](const char* a_name) { return method == a_name; });
			if (found == names.end()) {
				logger::warn("[ORG] tripwire: {} has no method {} to watch", InterfaceName(a_kind), method);
				continue;
			}
			set.bits.set(static_cast<std::size_t>(found - names.begin()));
		}
		return set;
	}

	namespace
	{
		/**
		 * @brief Later versions of the device, context and swap chain interfaces. A native handle implements only the first version,
		 * and every caller CS has checks for a refusal (GrassOptimizations, EngineConstants, HDRDisplay), so native mode refuses them
		 * without a tripwire line.
		 */
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
		std::atomic<ContextFilter> contextFilter{ nullptr };
		std::atomic<const NativeServices*> nativeServices{ nullptr };
	}

	void SetContextFilter(ContextFilter a_filter)
	{
		Detail::contextFilter.store(a_filter, std::memory_order_release);
	}

	void SetNativeServices(const NativeServices& a_services)
	{
		// Kept for the process: handles read it from any thread without a lock.
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
		if (wrapped)
			wrapped->Release();
		Retire(std::move(graph), std::move(owner));
	}

	void HandleCore::Init(Kind a_kind, Options&& a_options, IUnknown* a_wrap)
	{
		kind = a_kind;
		name = std::move(a_options.name);
		graph = std::move(a_options.graph);
		owner = std::move(a_options.owner);
		watch = a_options.watch;
		wrapped = a_wrap;
		if (!wrapped)
			return;
		wrapped->AddRef();
		const bool hotPath = kind == Kind::Device || kind == Kind::DeviceContext || kind == Kind::SwapChain;
		if (WatchAllRequested() && !hotPath)
			watch = WatchSet::All();
		// The wrapped DXVK object carries the ORG resource too. It can outlive the handle (bound in DXVK's context, or handed out by
		// a forwarded getter), and GraphResourceOf finds the resource from it.
		if ((graph || owner) && !hotPath && !Detail::CoreOf(wrapped))
			(void)AttachGraphResource(wrapped, graph, owner);
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
		// Counts a call at its site. The first one is logged with the text a_detail() makes, which is made only then.
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
			// Native mode (P6.3): another plugin's D3D11 call has nothing behind it. Named as an error, with the module, once per call
			// site. CS's own calls and the game's are the census's business; the warning above names them.
			if (!a_forwarded && !sites.selfTest && NativeServicesInstalled() && !IsOwnCode(a_caller) && !IsGameCode(a_caller)) {
				const HMODULE module = ModuleAt(a_caller);
				logger::error("[ORG native] {} calls {}::{} ({}): native mode (CS_ORG_NATIVE=1) has no D3D11, so the call failed and this "
							  "plugin's rendering does not work. Disable the plugin for native runs, or run on DXVK. Recorded in "
							  "docs/development/org-extension-api-notes.md.",
					module ? ModuleName(module) : std::string("code outside any module"), InterfaceName(a_core.kind), a_method, entry->second.where);
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
			// Every handle's vtable is in CS's image. DXVK's objects and the engine's have theirs elsewhere.
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
			// Another D3D11 object interface: the real object refuses it too, so this is no tripwire. (The engine's texture save
			// helper asks GetType first, then for the matching texture interface.)
			if (IsOtherObjectIid(a_riid) || (!a_core.wrapped && NativeServicesInstalled() && IsNativeUpgradeIid(a_riid))) {
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			// Another interface escapes the handle: always logged, forwarded in guest mode.
			a_core.Record(0, "QueryInterface", a_caller, a_core.wrapped != nullptr, IidText(a_riid));
			if (a_core.wrapped)
				return a_core.wrapped->QueryInterface(a_riid, a_out);
			*a_out = nullptr;
			return E_NOINTERFACE;
		}

		// a_call on the wrapped object as the interface that declares the private-data methods for its kind.
		template <class F>
		HRESULT OnWrapped(const HandleCore& a_core, F&& a_call)
		{
			switch (a_core.kind) {
			case Kind::Device:
				return a_call(static_cast<ID3D11Device*>(a_core.wrapped));
			case Kind::SwapChain:
				return a_call(static_cast<IDXGIObject*>(a_core.wrapped));
			default:
				return a_call(static_cast<ID3D11DeviceChild*>(a_core.wrapped));
			}
		}

		HRESULT SetPrivateData(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, UINT a_size, const void* a_data, void* a_caller)
		{
			const auto forward = [&](auto* a_object) { return a_object->SetPrivateData(a_guid, a_size, a_data); };
			if (IsDebugName(a_guid)) {
				a_core.SetName(a_data ? std::string(static_cast<const char*>(a_data), strnlen(static_cast<const char*>(a_data), a_size)) : std::string());
				if (a_core.wrapped)
					OnWrapped(a_core, forward);
				return S_OK;
			}
			if (a_core.Enter(a_slot, "SetPrivateData", a_caller))
				return OnWrapped(a_core, forward);
			return E_FAIL;
		}

		HRESULT SetPrivateDataInterface(HandleCore& a_core, std::uint16_t a_slot, REFGUID a_guid, const IUnknown* a_data, void* a_caller)
		{
			if (a_core.wrapped || !NativeServicesInstalled()) {
				if (a_core.Enter(a_slot, "SetPrivateDataInterface", a_caller))
					return OnWrapped(a_core, [&](auto* a_object) { return a_object->SetPrivateDataInterface(a_guid, a_data); });
				return E_FAIL;
			}
			// A native handle keeps it, as D3D11 does: replaced by a later set of the GUID, released with the handle.
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
			if (!a_core.wrapped && a_size && NativeServicesInstalled()) {
				// A native handle's private-data interface: the pointer, with a reference for the caller.
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
				// Not set: in native mode an answer, not a tripwire (GraphResourceOf and the descriptions ask every object).
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
			info.graph = core->graph;
			info.wrapped = core->wrapped;
		}
		return info;
	}

	namespace
	{
		/** @brief The native services for a native handle (nothing wrapped), null for a guest handle or before native mode set them. */
		const NativeServices* NativeOf(const HandleCore& a_core)
		{
			return a_core.wrapped ? nullptr : Detail::nativeServices.load(std::memory_order_acquire);
		}
	}

	std::shared_ptr<const void> OwnerOf(const void* a_object)
	{
		const auto* core = Detail::CoreOf(a_object);
		return core ? core->owner : nullptr;
	}

	DeviceHandle::~DeviceHandle()
	{
		// The context may outlive this device: it must not answer GetDevice with it any more.
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
			static_cast<ID3D11Device*>(core.wrapped)->GetImmediateContext(a_out);
			return;
		}
		Detail::Zero(a_out);
	}

	namespace
	{
		/** @brief The calling thread's ScopedCreateTexture2DHook, if any. */
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
			// The hook runs with no scope set, so a texture it makes through a device handle is a plain one.
			const ScopedCreateTexture2DHook none(nullptr);
			if (hook(*a_desc, a_data, a_out))
				return S_OK;
		}
		if (const auto* native = NativeOf(core); native && native->createTexture2D && a_desc && a_out)
			return native->createTexture2D(*a_desc, a_data, a_out);
		if (core.Enter(static_cast<std::uint16_t>(Slot::CreateTexture2D), "CreateTexture2D", _ReturnAddress()))
			return static_cast<ID3D11Device*>(core.wrapped)->CreateTexture2D(a_desc, a_data, a_out);
		Detail::Zero(a_out);
		return Detail::Failure<HRESULT>();
	}

	namespace
	{
		/** @brief The description of a 2D texture behind a_resource, false when it is something else. */
		bool Texture2DOf(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_out)
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			a_resource->GetType(&dimension);
			if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
				return false;
			static_cast<ID3D11Texture2D*>(a_resource)->GetDesc(&a_out);
			return true;
		}

		/** @name A view of a_resource with a null description: the whole 2D texture in its own format, as D3D11 makes it. */
		/** @{ */
		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_SHADER_RESOURCE_VIEW_DESC& a_out)
		{
			D3D11_TEXTURE2D_DESC texture{};
			if (!Texture2DOf(a_resource, texture))
				return false;
			a_out = {};
			a_out.Format = texture.Format;
			const bool multisampled = texture.SampleDesc.Count > 1;
			if (texture.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
				a_out.ViewDimension = texture.ArraySize > 6 ? D3D11_SRV_DIMENSION_TEXTURECUBEARRAY : D3D11_SRV_DIMENSION_TEXTURECUBE;
				a_out.TextureCubeArray = { 0, texture.MipLevels, 0, texture.ArraySize / 6 };
			} else if (texture.ArraySize > 1) {
				a_out.ViewDimension = multisampled ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				if (multisampled)
					a_out.Texture2DMSArray = { 0, texture.ArraySize };
				else
					a_out.Texture2DArray = { 0, texture.MipLevels, 0, texture.ArraySize };
			} else {
				a_out.ViewDimension = multisampled ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
				a_out.Texture2D = { 0, texture.MipLevels };
			}
			return true;
		}

		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_RENDER_TARGET_VIEW_DESC& a_out)
		{
			D3D11_TEXTURE2D_DESC texture{};
			if (!Texture2DOf(a_resource, texture))
				return false;
			a_out = {};
			a_out.Format = texture.Format;
			const bool multisampled = texture.SampleDesc.Count > 1;
			if (texture.ArraySize > 1) {
				a_out.ViewDimension = multisampled ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				if (multisampled)
					a_out.Texture2DMSArray = { 0, texture.ArraySize };
				else
					a_out.Texture2DArray = { 0, 0, texture.ArraySize };
			} else {
				a_out.ViewDimension = multisampled ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
			}
			return true;
		}

		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_DEPTH_STENCIL_VIEW_DESC& a_out)
		{
			D3D11_TEXTURE2D_DESC texture{};
			if (!Texture2DOf(a_resource, texture))
				return false;
			a_out = {};
			a_out.Format = texture.Format;
			const bool multisampled = texture.SampleDesc.Count > 1;
			if (texture.ArraySize > 1) {
				a_out.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				if (multisampled)
					a_out.Texture2DMSArray = { 0, texture.ArraySize };
				else
					a_out.Texture2DArray = { 0, 0, texture.ArraySize };
			} else {
				a_out.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
			}
			return true;
		}

		bool DefaultViewDesc(ID3D11Resource* a_resource, D3D11_UNORDERED_ACCESS_VIEW_DESC& a_out)
		{
			D3D11_TEXTURE2D_DESC texture{};
			if (!Texture2DOf(a_resource, texture))
				return false;
			a_out = {};
			a_out.Format = texture.Format;
			if (texture.ArraySize > 1) {
				a_out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
				a_out.Texture2DArray = { 0, 0, texture.ArraySize };
			} else {
				a_out.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			}
			return true;
		}
		/** @} */

		/** @brief What a native view is made with: its resource's ORG resource and owner, so lookups through the view find them. */
		Options NativeViewOptions(const char* a_kind, ID3D11Resource* a_resource)
		{
			Options options;
			const auto* resource = Detail::CoreOf(a_resource);
			options.name = resource ? fmt::format("{} {}", resource->Name(), a_kind) : fmt::format("Native::{}", a_kind);
			options.graph = GraphResourceOf(a_resource);
			options.owner = OwnerOf(a_resource);
			return options;
		}

		/**
		 * @brief A native view: a_make with the description given, or D3D11's default for the resource. E_INVALIDARG for a null
		 * resource, or a null description on a resource that has no default here (a buffer, a 1D or 3D texture).
		 */
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

		/** @brief The options of a native object other than a view: a name, which the engine's debug name replaces later. */
		Options NativeOptions(const char* a_kind)
		{
			Options options;
			options.name = fmt::format("Native::{}", a_kind);
			return options;
		}

		std::span<const std::uint8_t> Bytes(const void* a_data, SIZE_T a_length)
		{
			return { static_cast<const std::uint8_t*>(a_data), a_data ? static_cast<std::size_t>(a_length) : 0 };
		}
	}

	// The device's creation methods in both modes. TW_DEVICE_SITE starts one: the caller, the call site a guest handle unwraps its
	// arguments at, and the native services (null for a guest handle). TW_FORWARD_OR_FAIL ends one: a guest handle forwards the
	// call, and a native handle without a service records it at its caller and fails it, as every tripwire does.
#define TW_DEVICE_SITE(Name)                                                                                                     \
	void* const twCaller = _ReturnAddress();                                                                                     \
	[[maybe_unused]] const Detail::CallSite twSite{ &core, static_cast<std::uint16_t>(Slot::Name), #Name, twCaller };           \
	const NativeServices* const twNative = NativeOf(core)
#define TW_FORWARD_OR_FAIL(Name, Out, ...)                                         \
	if (core.Enter(static_cast<std::uint16_t>(Slot::Name), #Name, twCaller))      \
		return static_cast<ID3D11Device*>(core.wrapped)->Name(__VA_ARGS__);        \
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
			*a_out = ::RenderGraph::Engine::Tripwire::CreateBlendState(*a_desc, NativeOptions("BlendState")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateBlendState, a_out, a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* a_desc, ID3D11DepthStencilState** a_out)
	{
		TW_DEVICE_SITE(CreateDepthStencilState);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateDepthStencilState(*a_desc, NativeOptions("DepthStencilState")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateDepthStencilState, a_out, a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateRasterizerState(const D3D11_RASTERIZER_DESC* a_desc, ID3D11RasterizerState** a_out)
	{
		TW_DEVICE_SITE(CreateRasterizerState);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateRasterizerState(*a_desc, NativeOptions("RasterizerState")).detach();
			return S_OK;
		}
		TW_FORWARD_OR_FAIL(CreateRasterizerState, a_out, a_desc, a_out);
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CreateSamplerState(const D3D11_SAMPLER_DESC* a_desc, ID3D11SamplerState** a_out)
	{
		TW_DEVICE_SITE(CreateSamplerState);
		if (twNative && a_out && a_desc) {
			*a_out = ::RenderGraph::Engine::Tripwire::CreateSamplerState(*a_desc, NativeOptions("SamplerState")).detach();
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
		// The level native mode's device creation reported.
		if (twNative)
			return D3D_FEATURE_LEVEL_11_1;
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetFeatureLevel), "GetFeatureLevel", twCaller))
			return static_cast<ID3D11Device*>(core.wrapped)->GetFeatureLevel();
		return D3D_FEATURE_LEVEL{};
	}

	UINT STDMETHODCALLTYPE DeviceHandle::GetCreationFlags()
	{
		TW_DEVICE_SITE(GetCreationFlags);
		if (twNative)
			return 0;
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetCreationFlags), "GetCreationFlags", twCaller))
			return static_cast<ID3D11Device*>(core.wrapped)->GetCreationFlags();
		return 0;
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::GetDeviceRemovedReason()
	{
		TW_DEVICE_SITE(GetDeviceRemovedReason);
		// A lost Vulkan device shows in BasicRHI's device-loss report and the graph's stop, not here.
		if (twNative)
			return S_OK;
		if (core.Enter(static_cast<std::uint16_t>(Slot::GetDeviceRemovedReason), "GetDeviceRemovedReason", twCaller))
			return static_cast<ID3D11Device*>(core.wrapped)->GetDeviceRemovedReason();
		return E_FAIL;
	}

#undef TW_DEVICE_SITE

	// The swap chain: native mode's services, else a tripwire (the forwarding branch serves a wrapping swap chain handle).
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
			return static_cast<IDXGISwapChain*>(core.wrapped)->Present(a_syncInterval, a_flags);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::GetBuffer(UINT a_buffer, REFIID a_riid, void** a_out)
	{
		TW_SWAP_CHAIN_SITE(GetBuffer);
		if (twNative && twNative->getBuffer)
			return twNative->getBuffer(a_buffer, a_riid, a_out);
		if (core.Enter(twSlot, "GetBuffer", twCaller))
			return static_cast<IDXGISwapChain*>(core.wrapped)->GetBuffer(a_buffer, a_riid, a_out);
		Detail::Zero(a_out);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::ResizeBuffers(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags)
	{
		TW_SWAP_CHAIN_SITE(ResizeBuffers);
		if (twNative && twNative->resizeBuffers)
			return twNative->resizeBuffers(a_count, a_width, a_height, a_format, a_flags);
		if (core.Enter(twSlot, "ResizeBuffers", twCaller))
			return static_cast<IDXGISwapChain*>(core.wrapped)->ResizeBuffers(a_count, a_width, a_height, a_format, a_flags);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::GetFullscreenState(BOOL* a_fullscreen, IDXGIOutput** a_target)
	{
		TW_SWAP_CHAIN_SITE(GetFullscreenState);
		// Native mode presents to a borderless window only. Never exclusive fullscreen, so the engine does not try to enter it.
		if (twNative) {
			if (a_fullscreen)
				*a_fullscreen = FALSE;
			Detail::Zero(a_target);
			return S_OK;
		}
		if (core.Enter(twSlot, "GetFullscreenState", twCaller))
			return static_cast<IDXGISwapChain*>(core.wrapped)->GetFullscreenState(a_fullscreen, a_target);
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
			return static_cast<IDXGISwapChain*>(core.wrapped)->SetFullscreenState(a_fullscreen, Unwrap(a_target));
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::ResizeTarget(const DXGI_MODE_DESC* a_parameters)
	{
		TW_SWAP_CHAIN_SITE(ResizeTarget);
		// The window keeps its size. The engine's next resize follows the window (WM_WINDOWPOSCHANGED), as with fake fullscreen.
		if (twNative)
			return S_OK;
		if (core.Enter(twSlot, "ResizeTarget", twCaller))
			return static_cast<IDXGISwapChain*>(core.wrapped)->ResizeTarget(a_parameters);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE SwapChainHandle::GetContainingOutput(IDXGIOutput** a_out)
	{
		TW_SWAP_CHAIN_SITE(GetContainingOutput);
		if (twNative && twNative->getContainingOutput)
			return twNative->getContainingOutput(a_out);
		if (core.Enter(twSlot, "GetContainingOutput", twCaller))
			return static_cast<IDXGISwapChain*>(core.wrapped)->GetContainingOutput(a_out);
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
			return static_cast<ID3D11Device*>(core.wrapped)->CheckFeatureSupport(a_feature, a_data, a_size);
		Detail::ZeroBytes(a_data, a_size);
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE DeviceHandle::CheckFormatSupport(DXGI_FORMAT a_format, UINT* a_support)
	{
		void* const twCaller = _ReturnAddress();
		if (const auto* native = NativeOf(core); native && native->checkFormatSupport)
			return native->checkFormatSupport(a_format, a_support);
		if (core.Enter(static_cast<std::uint16_t>(Slot::CheckFormatSupport), "CheckFormatSupport", twCaller))
			return static_cast<ID3D11Device*>(core.wrapped)->CheckFormatSupport(a_format, a_support);
		Detail::Zero(a_support);
		return E_FAIL;
	}

	// The context's resource methods in both modes. TW_CONTEXT_SITE starts one like TW_DEVICE_SITE. A guest handle forwards
	// through Enter, so its filter sees the real caller as with every generated method.
#define TW_CONTEXT_SITE(Name)                                                                                                    \
	void* const twCaller = _ReturnAddress();                                                                                     \
	[[maybe_unused]] const Detail::CallSite twSite{ &core, static_cast<std::uint16_t>(Slot::Name), #Name, twCaller };           \
	const NativeServices* const twNative = NativeOf(core)
#define TW_CONTEXT_FORWARD(Name, ...)                                                    \
	if (core.Enter(static_cast<std::uint16_t>(Slot::Name), #Name, twCaller))            \
		return static_cast<ID3D11DeviceContext*>(core.wrapped)->Name(__VA_ARGS__)

	HRESULT STDMETHODCALLTYPE DeviceContextHandle::QueryInterface(REFIID a_riid, void** a_out)
	{
		if (const auto* native = NativeOf(core); native && native->queryContext && a_out && a_riid == __uuidof(ID3DUserDefinedAnnotation))
			return native->queryContext(a_riid, a_out);
		return Detail::QueryInterface(core, static_cast<ID3D11DeviceContext*>(this), kIids, a_riid, a_out, _ReturnAddress());
	}

	HRESULT STDMETHODCALLTYPE DeviceContextHandle::Map(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, UINT a_flags,
		D3D11_MAPPED_SUBRESOURCE* a_out)
	{
		TW_CONTEXT_SITE(Map);
		if (twNative && twNative->map)
			return twNative->map(a_resource, a_subresource, a_type, a_flags, a_out);
		TW_CONTEXT_FORWARD(Map, Detail::UnwrapAt(a_resource, twSite), a_subresource, a_type, a_flags, a_out);
		Detail::Zero(a_out);
		return E_FAIL;
	}

	void STDMETHODCALLTYPE DeviceContextHandle::Unmap(ID3D11Resource* a_resource, UINT a_subresource)
	{
		TW_CONTEXT_SITE(Unmap);
		if (twNative && twNative->unmap)
			return twNative->unmap(a_resource, a_subresource);
		TW_CONTEXT_FORWARD(Unmap, Detail::UnwrapAt(a_resource, twSite), a_subresource);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::UpdateSubresource(ID3D11Resource* a_resource, UINT a_subresource, const D3D11_BOX* a_box, const void* a_data,
		UINT a_rowPitch, UINT a_depthPitch)
	{
		TW_CONTEXT_SITE(UpdateSubresource);
		if (twNative && twNative->updateSubresource)
			return twNative->updateSubresource(a_resource, a_subresource, a_box, a_data, a_rowPitch, a_depthPitch);
		TW_CONTEXT_FORWARD(UpdateSubresource, Detail::UnwrapAt(a_resource, twSite), a_subresource, a_box, a_data, a_rowPitch, a_depthPitch);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CopyResource(ID3D11Resource* a_destination, ID3D11Resource* a_source)
	{
		TW_CONTEXT_SITE(CopyResource);
		if (twNative && twNative->copyResource)
			return twNative->copyResource(a_destination, a_source);
		TW_CONTEXT_FORWARD(CopyResource, Detail::UnwrapAt(a_destination, twSite), Detail::UnwrapAt(a_source, twSite));
	}

	void STDMETHODCALLTYPE DeviceContextHandle::CopySubresourceRegion(ID3D11Resource* a_destination, UINT a_destinationSubresource, UINT a_x, UINT a_y, UINT a_z,
		ID3D11Resource* a_source, UINT a_sourceSubresource, const D3D11_BOX* a_box)
	{
		TW_CONTEXT_SITE(CopySubresourceRegion);
		if (twNative && twNative->copySubresourceRegion)
			return twNative->copySubresourceRegion(a_destination, a_destinationSubresource, a_x, a_y, a_z, a_source, a_sourceSubresource, a_box);
		TW_CONTEXT_FORWARD(CopySubresourceRegion, Detail::UnwrapAt(a_destination, twSite), a_destinationSubresource, a_x, a_y, a_z,
			Detail::UnwrapAt(a_source, twSite), a_sourceSubresource, a_box);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::GenerateMips(ID3D11ShaderResourceView* a_view)
	{
		TW_CONTEXT_SITE(GenerateMips);
		if (twNative && twNative->generateMips)
			return twNative->generateMips(a_view);
		TW_CONTEXT_FORWARD(GenerateMips, Detail::UnwrapAt(a_view, twSite));
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearRenderTargetView(ID3D11RenderTargetView* a_view, const FLOAT a_color[4])
	{
		TW_CONTEXT_SITE(ClearRenderTargetView);
		if (twNative && twNative->clearRenderTargetView)
			return twNative->clearRenderTargetView(a_view, a_color);
		TW_CONTEXT_FORWARD(ClearRenderTargetView, Detail::UnwrapAt(a_view, twSite), a_color);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* a_view, const UINT a_values[4])
	{
		TW_CONTEXT_SITE(ClearUnorderedAccessViewUint);
		if (twNative && twNative->clearUnorderedAccessViewUint)
			return twNative->clearUnorderedAccessViewUint(a_view, a_values);
		TW_CONTEXT_FORWARD(ClearUnorderedAccessViewUint, Detail::UnwrapAt(a_view, twSite), a_values);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* a_view, const FLOAT a_values[4])
	{
		TW_CONTEXT_SITE(ClearUnorderedAccessViewFloat);
		if (twNative && twNative->clearUnorderedAccessViewFloat)
			return twNative->clearUnorderedAccessViewFloat(a_view, a_values);
		TW_CONTEXT_FORWARD(ClearUnorderedAccessViewFloat, Detail::UnwrapAt(a_view, twSite), a_values);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::ClearDepthStencilView(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
	{
		TW_CONTEXT_SITE(ClearDepthStencilView);
		if (twNative && twNative->clearDepthStencilView)
			return twNative->clearDepthStencilView(a_view, a_flags, a_depth, a_stencil);
		TW_CONTEXT_FORWARD(ClearDepthStencilView, Detail::UnwrapAt(a_view, twSite), a_flags, a_depth, a_stencil);
	}

	void STDMETHODCALLTYPE DeviceContextHandle::Begin(ID3D11Asynchronous* a_query)
	{
		TW_CONTEXT_SITE(Begin);
		// Native mode: a query's interval is not measured, so Begin has nothing to record.
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
			static_cast<ID3D11DeviceContext*>(core.wrapped)->GetDevice(a_out);
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
		// A native handle without its own resource may carry one AttachGraphResource tied to it (its private data, below).
		if (const auto* core = Detail::CoreOf(a_object); core && (core->graph || core->wrapped || !NativeServicesInstalled()))
			return core->graph;
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
		// The object takes a reference to the tag and drops it when it is destroyed. If it refuses, the tag dies here and what it
		// holds still goes through ServiceRetired.
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
		if (!core->wrapped) {
			// Native mode has no D3D11 below the handles: the handle is the object every caller talks to.
			if (Native::Active())
				return a_object;
			// A native handle handed to D3D11: D3D11 gets null rather than an object it would cast to its own class.
			static std::array<std::atomic<bool>, kKinds> warned{};
			if (!warned[static_cast<std::size_t>(core->kind)].exchange(true))
				logger::warn("[ORG] tripwire: a native {} (\"{}\") was passed to D3D11, which gets null instead", InterfaceName(core->kind), core->Name());
		}
		return core->wrapped;
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
		return ShaderResourceViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc);
	}
	winrt::com_ptr<ID3D11RenderTargetView> CreateRenderTargetView(ID3D11Resource* a_resource, const D3D11_RENDER_TARGET_VIEW_DESC& a_desc, Options a_options,
		ID3D11RenderTargetView* a_wrap)
	{
		return RenderTargetViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc);
	}
	winrt::com_ptr<ID3D11DepthStencilView> CreateDepthStencilView(ID3D11Resource* a_resource, const D3D11_DEPTH_STENCIL_VIEW_DESC& a_desc, Options a_options,
		ID3D11DepthStencilView* a_wrap)
	{
		return DepthStencilViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc);
	}
	winrt::com_ptr<ID3D11UnorderedAccessView> CreateUnorderedAccessView(ID3D11Resource* a_resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc, Options a_options,
		ID3D11UnorderedAccessView* a_wrap)
	{
		return UnorderedAccessViewHandle::Make(std::move(a_options), a_wrap, a_resource, a_desc);
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

	std::vector<std::shared_ptr<const void>> TakeRetired()
	{
		auto& retired = GetRetired();
		std::lock_guard guard(retired.lock);
		return std::exchange(retired.owners, {});
	}

	void ServiceRetired()
	{
		auto owners = TakeRetired();
#if defined(CS_HAS_RENDER_GRAPH)
		auto& runtime = RenderGraphRuntime::Get();
		for (auto& owner : owners)
			runtime.ReleaseAfterD3D11(std::move(owner));
#endif
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
