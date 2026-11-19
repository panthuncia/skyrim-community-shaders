#include "Vram/Debug.h"

#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "Utils/SwitchFile.h"
#include "Vram/DxvkMemory.h"

#include <DbgHelp.h>
#include <RE/N/NiSourceTexture.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace Vram::Debug
{
	namespace
	{
		std::string Lower(std::string a_text)
		{
			for (auto& c : a_text)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return a_text;
		}

		// ----- Call sites (DbgHelp is single-threaded: everything under one lock) -----

		std::mutex g_symbolMutex;
		bool g_symbolsReady = false;
		std::unordered_map<const void*, std::pair<std::string, std::string>> g_frameCache;  // address -> module, text

		const std::pair<std::string, std::string>& DescribeFrame(const void* a_address)
		{
			if (const auto it = g_frameCache.find(a_address); it != g_frameCache.end())
				return it->second;

			std::string module = "(unknown)";
			std::uintptr_t base = 0;
			HMODULE handle = nullptr;
			if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCWSTR>(a_address), &handle)) {
				wchar_t path[MAX_PATH]{};
				if (::GetModuleFileNameW(handle, path, MAX_PATH))
					module = Lower(std::filesystem::path(path).filename().string());
				base = reinterpret_cast<std::uintptr_t>(handle);
			}

			if (!g_symbolsReady) {
				::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
				g_symbolsReady = ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE) != FALSE;
			}

			std::string text;
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 255;
			DWORD64 displacement = 0;
			const auto address = reinterpret_cast<DWORD64>(a_address);
			if (g_symbolsReady && ::SymFromAddr(::GetCurrentProcess(), address, &displacement, symbol)) {
				text = fmt::format("{}!{}+0x{:X}", module, symbol->Name, displacement);
				IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
				DWORD lineDisplacement = 0;
				if (::SymGetLineFromAddr64(::GetCurrentProcess(), address, &lineDisplacement, &line))
					text += fmt::format(" ({}:{})", std::filesystem::path(line.FileName).filename().string(), line.LineNumber);
			} else {
				text = fmt::format("{}+0x{:X}", module, address - base);
			}
			return g_frameCache.emplace(a_address, std::make_pair(std::move(module), std::move(text))).first->second;
		}

		bool InDxvk(const std::string& a_module)
		{
			return a_module == "dxvk_d3d11.dll" || a_module == "dxvk_dxgi.dll";
		}

		/** @brief A frame inside BasicRHI, ORG or the standard library: an allocation's mechanics, not who asked for it. */
		bool InLibraries(const std::string& a_text)
		{
			return a_text.find("!rhi::") != std::string::npos || a_text.find("!org::") != std::string::npos ||
			       a_text.find("!std::") != std::string::npos;
		}

		// The first frame outside DXVK (or, for the device memory ledger, outside BasicRHI, ORG and std) names the creator; the two
		// after it give the context.
		CallSite Describe(const void* const* a_frames, std::size_t a_count, bool a_skipDxvk, bool a_skipLibraries = false)
		{
			std::lock_guard lock(g_symbolMutex);
			CallSite site;
			int described = 0;
			// The ledger keeps eight frames: without the PDB the library frames cannot be skipped by name, and eight reach the caller.
			const int depth = a_skipLibraries ? 8 : 3;
			for (std::size_t i = 0; i < a_count && described < depth; ++i) {
				const auto& [module, text] = DescribeFrame(a_frames[i]);
				if (a_skipDxvk && described == 0 && InDxvk(module))
					continue;
				if (a_skipLibraries && described == 0 && InLibraries(text))
					continue;
				if (described == 0)
					site.module = module;
				site.text += (described ? " < " : "") + text;
				++described;
			}
			return site;
		}

		// ----- Game texture loads -----

		struct PendingTexture
		{
			RE::NiPointer<RE::NiTexture> texture;
			std::string path;
			std::chrono::steady_clock::time_point queued;
		};
		std::mutex g_textureMutex;
		std::vector<PendingTexture> g_pendingTextures;

		const char* TextureOwner(std::string_view a_path)
		{
			// "textures\landscape\dirt02.dds" (or "data\textures\...") -> "Game/Textures/landscape"
			std::string path = Lower(std::string(a_path));
			std::ranges::replace(path, '/', '\\');
			for (std::string_view prefix : { "data\\", "textures\\" })
				if (path.starts_with(prefix))
					path.erase(0, prefix.size());
			const auto slash = path.find('\\');
			const std::string folder = slash == std::string::npos ? "(root)" : path.substr(0, slash);

			static std::mutex mutex;
			static std::unordered_map<std::string, std::string> owners;
			std::lock_guard lock(mutex);
			auto& owner = owners[folder];
			if (owner.empty())
				owner = "Game/Textures/" + folder;
			return owner.c_str();
		}

		// Whether the texture's D3D11 resource exists yet; labels it if so.
		bool TryTag(RE::NiTexture* a_texture, std::string_view a_path)
		{
			auto* source = netimmerse_cast<RE::NiSourceTexture*>(a_texture);
			if (!source)
				return true;  // not a file texture: nothing to wait for
			if (!source->rendererTexture || !source->rendererTexture->texture)
				return false;
			// DXVK's exports take its own objects, never an engine texture's tripwire handle.
			TagGameTexture(RenderGraph::Engine::Tripwire::Unwrap(source->rendererTexture->texture), a_path);
			return true;
		}

		struct BSShaderManager_GetTexture
		{
			static void thunk(const char* a_path, bool a_demand, RE::NiPointer<RE::NiTexture>& a_textureOut, bool a_isHeightMap)
			{
				func(a_path, a_demand, a_textureOut, a_isHeightMap);
				if (!a_path || !a_textureOut)
					return;
				// A queued load finishes on a loader thread later: hold the texture until it has its D3D11 resource.
				if (!DxvkAvailable())
					return;
				if (!TryTag(a_textureOut.get(), a_path)) {
					std::lock_guard lock(g_textureMutex);
					g_pendingTextures.push_back({ a_textureOut, a_path, std::chrono::steady_clock::now() });
				}
				ProcessPendingTextures();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Enabled()
	{
		static const bool enabled = Util::SwitchFile::Enabled("CS_VRAM_DEBUG");
		return enabled;
	}

	void ConfigureDxvk(HMODULE a_dxvkD3D11)
	{
		if (!a_dxvkD3D11)
			return;
		const auto configure = reinterpret_cast<PFN_dxvkConfigureMemoryDebug>(reinterpret_cast<void*>(::GetProcAddress(a_dxvkD3D11, "dxvkConfigureMemoryDebug")));
		if (!configure) {
			logger::info("[VRAM] dxvk_d3d11.dll has no dxvkConfigureMemoryDebug; resources without an owner are not traced to their creators");
			return;
		}
		configure(DXVK_MEMORY_DEBUG_CREATION_STACKS);
		logger::info("[VRAM] DXVK records every resource's creation stack");
	}

	const char* GameTextureOwner(std::string_view a_path)
	{
		return TextureOwner(a_path);
	}

	void TagGameTexture(ID3D11Resource* a_resource, std::string_view a_path)
	{
		SetOwner(a_resource, TextureOwner(a_path), true);
		UINT size = 0;
		if (a_resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, nullptr) != S_OK || size == 0)
			a_resource->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(a_path.size()), a_path.data());
	}

	void InstallGameHooks()
	{
		stl::detour_thunk<BSShaderManager_GetTexture>(REL::RelocationID(98986, 105640));
	}

	void ProcessPendingTextures()
	{
		std::vector<PendingTexture> ready;
		{
			std::lock_guard lock(g_textureMutex);
			const auto now = std::chrono::steady_clock::now();
			std::erase_if(g_pendingTextures, [&](PendingTexture& a_pending) {
				// Gives up on a load that never finishes rather than keeping its texture alive.
				if (TryTag(a_pending.texture.get(), a_pending.path) || now - a_pending.queued > std::chrono::seconds(60)) {
					ready.push_back(std::move(a_pending));
					return true;
				}
				return false;
			});
		}
		// The last reference may free the texture: released outside the lock.
		ready.clear();
	}

	CallSite DxvkCreationSite(std::uint64_t a_cookie)
	{
		static const auto getStack = [] {
			HMODULE d3d11 = ::GetModuleHandleW(L"dxvk_d3d11.dll");
			return d3d11 ? reinterpret_cast<PFN_dxvkGetResourceCreationStack>(reinterpret_cast<void*>(::GetProcAddress(d3d11, "dxvkGetResourceCreationStack"))) :
			               nullptr;
		}();
		if (!getStack || !globals::d3d::device)
			return {};
		void* frames[48];
		std::uint32_t count = 0;
		if (getStack(globals::d3d::device, a_cookie, frames, 48, &count) != S_OK)
			return {};
		return Describe(frames, std::min<std::uint32_t>(count, 48), true);
	}


}
