#include "MemAB.h"

#include "MemProbe.h"

#include <detours/detours.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_map>

namespace MemAB
{
	namespace
	{
		std::string Switch(const char* a_name)
		{
			char value[8192]{};
			const DWORD length = GetEnvironmentVariableA(a_name, value, sizeof(value));
			return length && length < sizeof(value) ? std::string(value, length) : std::string{};
		}

		struct Tracked
		{
			const char* type;
			std::uint64_t bytes;
			std::string name;
			std::string detail;
			std::string module;
			UINT bind;
		};
		std::mutex lock;
		std::unordered_map<ID3D11DeviceChild*, Tracked> tracked;

		// {7F2C1E74-3B4A-4C8E-9A61-0D2E5B7A1C33}
		constexpr GUID kNotifierGuid = { 0x7f2c1e74, 0x3b4a, 0x4c8e, { 0x9a, 0x61, 0x0d, 0x2e, 0x5b, 0x7a, 0x1c, 0x33 } };

		// The resource's private data holds the only reference, so its release is the resource's destruction.
		class Notifier final : public IUnknown
		{
		public:
			explicit Notifier(ID3D11DeviceChild* a_resource) :
				resource(a_resource) {}
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_out) override
			{
				if (a_iid == __uuidof(IUnknown)) {
					*a_out = this;
					AddRef();
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = --references;
				if (!left) {
					{
						std::lock_guard guard(lock);
						tracked.erase(resource);
					}
					delete this;
				}
				return left;
			}

		private:
			std::atomic<ULONG> references = 1;
			ID3D11DeviceChild* resource;
		};

		std::uint32_t BitsPerPixel(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G32B32A32_TYPELESS:
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
			case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_SINT:
				return 128;
			case DXGI_FORMAT_R32G32B32_TYPELESS:
			case DXGI_FORMAT_R32G32B32_FLOAT:
			case DXGI_FORMAT_R32G32B32_UINT:
			case DXGI_FORMAT_R32G32B32_SINT:
				return 96;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R16G16B16A16_UINT:
			case DXGI_FORMAT_R16G16B16A16_SNORM:
			case DXGI_FORMAT_R16G16B16A16_SINT:
			case DXGI_FORMAT_R32G32_TYPELESS:
			case DXGI_FORMAT_R32G32_FLOAT:
			case DXGI_FORMAT_R32G32_UINT:
			case DXGI_FORMAT_R32G32_SINT:
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
			case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
				return 64;
			case DXGI_FORMAT_R8G8_TYPELESS:
			case DXGI_FORMAT_R8G8_UNORM:
			case DXGI_FORMAT_R8G8_UINT:
			case DXGI_FORMAT_R8G8_SNORM:
			case DXGI_FORMAT_R8G8_SINT:
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_R16_UINT:
			case DXGI_FORMAT_R16_SNORM:
			case DXGI_FORMAT_R16_SINT:
			case DXGI_FORMAT_B5G6R5_UNORM:
			case DXGI_FORMAT_B5G5R5A1_UNORM:
			case DXGI_FORMAT_B4G4R4A4_UNORM:
				return 16;
			case DXGI_FORMAT_R8_TYPELESS:
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_R8_UINT:
			case DXGI_FORMAT_R8_SNORM:
			case DXGI_FORMAT_R8_SINT:
			case DXGI_FORMAT_A8_UNORM:
			case DXGI_FORMAT_BC2_TYPELESS:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_TYPELESS:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_TYPELESS:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_TYPELESS:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_TYPELESS:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return 8;
			case DXGI_FORMAT_BC1_TYPELESS:
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_TYPELESS:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				return 4;
			default:
				return 32;
			}
		}

		bool IsBlock(DXGI_FORMAT a_format)
		{
			return (a_format >= DXGI_FORMAT_BC1_TYPELESS && a_format <= DXGI_FORMAT_BC5_SNORM) || (a_format >= DXGI_FORMAT_BC6H_TYPELESS && a_format <= DXGI_FORMAT_BC7_UNORM_SRGB);
		}

		std::uint64_t TextureBytes(DXGI_FORMAT a_format, UINT a_width, UINT a_height, UINT a_depth, UINT a_mips, UINT a_layers, UINT a_samples)
		{
			if (!a_mips) {
				a_mips = 1;
				for (UINT size = std::max({ a_width, a_height, a_depth }); size > 1; size >>= 1)
					++a_mips;
			}
			const auto bits = BitsPerPixel(a_format);
			const bool block = IsBlock(a_format);
			std::uint64_t total = 0;
			for (UINT mip = 0; mip < a_mips; ++mip) {
				std::uint64_t width = std::max(a_width >> mip, 1u), height = std::max(a_height >> mip, 1u);
				if (block) {
					width = (width + 3) & ~std::uint64_t(3);
					height = (height + 3) & ~std::uint64_t(3);
				}
				total += width * height * std::max(a_depth >> mip, 1u) * bits / 8;
			}
			return total * a_layers * a_samples;
		}

		std::string ModuleOf(void* a_address)
		{
			HMODULE module = nullptr;
			char path[MAX_PATH]{};
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<const char*>(a_address), &module))
				GetModuleFileNameA(module, path, MAX_PATH);
			return std::filesystem::path(path).filename().string();
		}

		void Add(ID3D11DeviceChild* a_resource, Tracked a_row, void* a_caller)
		{
			a_row.module = ModuleOf(a_caller);
			{
				std::lock_guard guard(lock);
				tracked[a_resource] = std::move(a_row);
			}
			auto* notifier = new Notifier(a_resource);
			a_resource->SetPrivateDataInterface(kNotifierGuid, notifier);
			notifier->Release();
		}

		using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
		using CreateTexture1DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
		using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
		using CreateTexture3DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);
		CreateBufferFn realBuffer = nullptr;
		CreateTexture1DFn realTexture1D = nullptr;
		CreateTexture2DFn realTexture2D = nullptr;
		CreateTexture3DFn realTexture3D = nullptr;

		HRESULT STDMETHODCALLTYPE HookBuffer(ID3D11Device* a_device, const D3D11_BUFFER_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Buffer** a_out)
		{
			const HRESULT result = realBuffer(a_device, a_desc, a_data, a_out);
			if (SUCCEEDED(result) && a_out && *a_out)
				Add(*a_out, { "Buffer", a_desc->ByteWidth, {}, std::format("usage {} bind {:x}", static_cast<int>(a_desc->Usage), a_desc->BindFlags), {}, a_desc->BindFlags }, _ReturnAddress());
			return result;
		}
		HRESULT STDMETHODCALLTYPE HookTexture1D(ID3D11Device* a_device, const D3D11_TEXTURE1D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture1D** a_out)
		{
			const HRESULT result = realTexture1D(a_device, a_desc, a_data, a_out);
			if (SUCCEEDED(result) && a_out && *a_out)
				Add(*a_out, { "Texture", TextureBytes(a_desc->Format, a_desc->Width, 1, 1, a_desc->MipLevels, a_desc->ArraySize, 1),
								{}, std::format("{}x1x1 mips {} layers {} DXGI {} usage {}", a_desc->Width, a_desc->MipLevels, a_desc->ArraySize, static_cast<int>(a_desc->Format), static_cast<int>(a_desc->Usage)), {}, a_desc->BindFlags },
					_ReturnAddress());
			return result;
		}
		HRESULT STDMETHODCALLTYPE HookTexture2D(ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out)
		{
			const HRESULT result = realTexture2D(a_device, a_desc, a_data, a_out);
			if (SUCCEEDED(result) && a_out && *a_out)
				Add(*a_out, { "Texture", TextureBytes(a_desc->Format, a_desc->Width, a_desc->Height, 1, a_desc->MipLevels, a_desc->ArraySize, a_desc->SampleDesc.Count),
								{}, std::format("{}x{}x1 mips {} layers {} DXGI {} usage {}", a_desc->Width, a_desc->Height, a_desc->MipLevels, a_desc->ArraySize, static_cast<int>(a_desc->Format), static_cast<int>(a_desc->Usage)), {}, a_desc->BindFlags },
					_ReturnAddress());
			return result;
		}
		HRESULT STDMETHODCALLTYPE HookTexture3D(ID3D11Device* a_device, const D3D11_TEXTURE3D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture3D** a_out)
		{
			const HRESULT result = realTexture3D(a_device, a_desc, a_data, a_out);
			if (SUCCEEDED(result) && a_out && *a_out)
				Add(*a_out, { "Texture", TextureBytes(a_desc->Format, a_desc->Width, a_desc->Height, a_desc->Depth, a_desc->MipLevels, 1, 1),
								{}, std::format("{}x{}x{} mips {} layers 1 DXGI {} usage {}", a_desc->Width, a_desc->Height, a_desc->Depth, a_desc->MipLevels, static_cast<int>(a_desc->Format), static_cast<int>(a_desc->Usage)), {}, a_desc->BindFlags },
					_ReturnAddress());
			return result;
		}

		/** The branch's owner names, as far as a descriptor and a call site tell them. */
		std::string OwnerOf(const Tracked& a_row)
		{
			if (!a_row.name.empty()) {
				const auto colons = a_row.name.find("::");
				return std::format("CS/{}/{}", colons == std::string::npos ? "named" : a_row.name.substr(0, colons), a_row.name);
			}
			const bool game = a_row.module == "SkyrimSE.exe";
			const char* prefix = game ? "Game" : (a_row.module == "CommunityShaders.dll" ? "CS" : "Other");
			if (a_row.bind & D3D11_BIND_DEPTH_STENCIL)
				return std::format("{}/DepthStencil", prefix);
			if (a_row.bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))
				return std::format("{}/RenderTarget", prefix);
			if (a_row.type == std::string_view("Buffer"))
				return std::format("{}/{}", prefix, (a_row.bind & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) ? "Geometry" : "Buffer");
			return std::format("{}/Textures", prefix);
		}

		void WriteJson()
		{
			const auto directory = logger::log_directory().value_or(std::filesystem::path{});
			const auto reading = MemProbe::Read();
			nlohmann::json json;
			json["dxvkSerial"] = 0;
			json["heaps"].push_back({ { "index", 0 }, { "deviceLocal", true }, { "size", reading.localBudget }, { "usage", reading.localUsage }, { "budget", reading.localBudget } });
			json["heaps"].push_back({ { "index", 1 }, { "deviceLocal", false }, { "size", 0 }, { "usage", reading.nonLocalUsage }, { "budget", 0 } });
			json["rows"] = nlohmann::json::array();
			{
				std::lock_guard guard(lock);
				for (const auto& [resource, row] : tracked)
					json["rows"].push_back({ { "name", row.name }, { "owner", OwnerOf(row) }, { "type", row.type }, { "source", "D3D11" }, { "bytes", row.bytes },
						{ "flags", 0 }, { "deviceLocal", true }, { "heap", 0 }, { "detail", row.detail }, { "callsite", row.module }, { "uid", 0 } });
			}
			const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
			std::ofstream file(directory / std::format("CommunityShaders-VRAM-{}.json", stamp), std::ios::binary);
			file << json.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
		}

		bool LoadingScreen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		struct Command
		{
			std::uint32_t frame;
			std::string text;
		};
		struct Span
		{
			std::uint32_t start = 0, end = 0;
			float amount = 0.0f;
		};
		std::vector<Span> ParseSpans(const std::string& a_value)
		{
			std::vector<Span> spans;
			std::string_view text = a_value;
			while (!text.empty()) {
				const auto end = text.find(';');
				Span span;
				if (std::sscanf(std::string(text.substr(0, end)).c_str(), "%u:%u:%f", &span.start, &span.end, &span.amount) == 3)
					spans.push_back(span);
				text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			}
			return spans;
		}

		void RunCommand(const std::string& a_command, std::uint32_t a_frame)
		{
			logger::info("[AB] test command at frame {}: {}", a_frame, a_command);
			if (a_command.rfind("dclf:mark=", 0) == 0) {
				MemProbe::Mark(a_command.substr(10), a_frame);
				return;
			}
			if (a_command == "dclf:vram-json") {
				WriteJson();
				return;
			}
			if (a_command.rfind("dclf:", 0) == 0)
				return;
			if (auto* tasks = SKSE::GetTaskInterface())
				tasks->AddTask([a_command] {
					const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
					if (auto* script = factory ? static_cast<RE::Script*>(factory->Create()) : nullptr) {
						script->SetCommand(a_command);
						script->CompileAndRun(nullptr);
						delete script;
					}
				});
		}
	}

	void ApplyEnvironmentFile()
	{
		auto path = logger::log_directory();
		if (!path)
			return;
		std::ifstream file(*path / "CommunityShaders.env");
		std::string line;
		while (file && std::getline(file, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const auto equals = line.find('=');
			if (equals != std::string::npos && line[0] != '#')
				SetEnvironmentVariableA(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str());
		}
	}

	void NoteName(ID3D11DeviceChild* a_resource, const char* a_name)
	{
		std::lock_guard guard(lock);
		if (const auto found = tracked.find(a_resource); found != tracked.end())
			found->second.name = a_name;
	}

	void Track(ID3D11Device* a_device)
	{
		if (Switch("CS_AB_PROBE") != "1" || !a_device)
			return;
		auto** vtable = *reinterpret_cast<void***>(a_device);
		realBuffer = reinterpret_cast<CreateBufferFn>(vtable[3]);
		realTexture1D = reinterpret_cast<CreateTexture1DFn>(vtable[4]);
		realTexture2D = reinterpret_cast<CreateTexture2DFn>(vtable[5]);
		realTexture3D = reinterpret_cast<CreateTexture3DFn>(vtable[6]);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&realBuffer), reinterpret_cast<PVOID>(HookBuffer));
		DetourAttach(reinterpret_cast<PVOID*>(&realTexture1D), reinterpret_cast<PVOID>(HookTexture1D));
		DetourAttach(reinterpret_cast<PVOID*>(&realTexture2D), reinterpret_cast<PVOID>(HookTexture2D));
		DetourAttach(reinterpret_cast<PVOID*>(&realTexture3D), reinterpret_cast<PVOID>(HookTexture3D));
		logger::info("[AB] resource tracking: {}", DetourTransactionCommit());
	}

	void OnPresent()
	{
		MemProbe::Start(logger::log_directory().value_or(std::filesystem::path{}));
		static const auto commands = [] {
			std::vector<Command> list;
			if (!Switch("CS_DCLF_TEST_MOVE").empty())
				list.push_back({ 1, "tgm" });
			std::string value = Switch("CS_DCLF_TEST_COMMANDS");
			std::string_view text = value;
			while (!text.empty()) {
				const auto end = text.find(';');
				const auto item = text.substr(0, end);
				const auto colon = item.find(':');
				if (colon != std::string_view::npos)
					list.push_back({ static_cast<std::uint32_t>(std::strtoul(std::string(item.substr(0, colon)).c_str(), nullptr, 10)), std::string(item.substr(colon + 1)) });
				text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			}
			return list;
		}();
		static const auto moves = ParseSpans(Switch("CS_DCLF_TEST_MOVE"));
		static const auto turns = ParseSpans(Switch("CS_DCLF_TEST_TURN"));
		static const auto exitFrame = static_cast<std::uint32_t>(std::strtoul(Switch("CS_DCLF_TEST_EXIT").c_str(), nullptr, 10));
		static std::uint32_t frame = 0;
		static std::size_t next = 0;
		if (LoadingScreen())
			return;
		++frame;
		if (exitFrame && frame >= exitFrame) {
			logger::info("[AB] test exit at frame {}", frame);
			spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& a_logger) { a_logger->flush(); });
			::TerminateProcess(::GetCurrentProcess(), 0);
		}
		while (next < commands.size() && frame >= commands[next].frame)
			RunCommand(commands[next++].text, frame);
		for (const auto& move : moves)
			if (frame >= move.start && frame < move.end)
				if (auto* tasks = SKSE::GetTaskInterface())
					tasks->AddTask([units = move.amount] {
						if (auto* player = RE::PlayerCharacter::GetSingleton()) {
							auto position = player->GetPosition();
							const float heading = player->GetAngleZ();
							position.x += std::sin(heading) * units;
							position.y += std::cos(heading) * units;
							player->SetPosition(position, true);
						}
					});
		for (const auto& turn : turns)
			if (frame >= turn.start && frame < turn.end)
				if (auto* tasks = SKSE::GetTaskInterface())
					tasks->AddTask([radians = turn.amount * 0.017453292f] {
						if (auto* player = RE::PlayerCharacter::GetSingleton())
							player->SetHeading(player->GetAngleZ() + radians);
					});
	}
}
