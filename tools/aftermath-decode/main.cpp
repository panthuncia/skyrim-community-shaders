// aftermath-decode: decodes a Nsight Aftermath GPU crash dump (.nv-gpudmp) written by Community Shaders
// (src/Aftermath.cpp) into a readable summary and the SDK's full JSON.
//
//   aftermath-decode <dump.nv-gpudmp> [--debug-info <dir>] [--spirv <dir>] [--json <out.json>]
//
// The summary names the device status, the page fault (the faulting GPU virtual address, its access and
// client, and the resources the fault touched), the shaders active at the fault, and the last event
// markers per context (DXVK's checkpoints and ORG's, which Aftermath.cpp resolves to their labels as the
// dump is written). The shader debug info Aftermath.cpp writes next to a dump
// (gpu-crash-<stamp>-shader-<identifier>.nvdbg) is looked up by its identifier in the dump's folder, or
// in --debug-info. The JSON (by default <dump>.json) holds everything the SDK can decode.
//
// --spirv names ORGModuleServices' shader cache (Data/ShaderCache/v3, in MO2's overwrite folder): the
// SPIR-V of every shader DCLF compiled. It is opened read-only through the store, so a running game keeps
// it. The decoder hands the SDK the binary whose Aftermath hash an active shader carries, so the JSON maps
// the faulting warps to SPIR-V, and to HLSL lines when the shader was built with CS_DCLF_SHADER_DEBUG=1.
// Hashing the cache is slow, so the hashes are kept in <dir>/aftermath-hashes.txt, keyed by compile key:
// an artifact's key names its contents, so a key's hash never changes.
//
// The dump carries the fault address, resources and warp PCs only when DXVK created the device with
// the NVIDIA diagnostics config: DXVK_DEBUG=hang plus DXVK_AFTERMATH_RESOURCE_TRACKING=1,
// DXVK_AFTERMATH_SHADER_DEBUG_INFO=1 and DXVK_AFTERMATH_SHADER_ERROR_REPORTING=1
// (extern/dxvk/src/dxvk/dxvk_adapter.cpp).

// The SDK declares its SPIR-V types only beside Vulkan's header; they need nothing from it.
#define VULKAN_H_ 1
#include <GFSDK_Aftermath.h>
#include <GFSDK_Aftermath_Defines.h>
#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

#include <ORGModuleServices/ShaderStore.h>

#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	std::vector<char> ReadFile(const fs::path& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		return in ? std::vector<char>(std::istreambuf_iterator<char>(in), {}) : std::vector<char>{};
	}

	struct Lookup
	{
		std::vector<fs::path> directories;
		std::vector<char> held;  // the SDK copies what it is given before the callback returns; kept alive regardless
		unsigned found = 0, missing = 0;
		std::unique_ptr<org::services::ShaderStore> store;
		struct Held
		{
			org::services::ShaderStoreHash key;
			std::size_t size = 0;
		};
		std::unordered_map<std::uint64_t, Held> spirv;  // Aftermath shader hash -> the artifact in the ORG cache
		std::vector<std::byte> heldSpirv;
		unsigned spirvFound = 0, spirvMissing = 0;
	};

	// The store's format number for SPIR-V (extern/ORGModuleServices, docs/development/shader-cache.md).
	constexpr std::uint8_t kFormatSpirv = 1;

	// Indexes a cache directory by Aftermath shader hash, reusing the hashes of artifacts seen in an earlier run.
	void IndexSpirv(Lookup& a_lookup, const fs::path& a_directory)
	{
		const auto indexPath = a_directory / "aftermath-hashes.txt";
		std::unordered_map<std::string, std::uint64_t> known;
		{
			std::ifstream in(indexPath);
			std::string key;
			std::uint64_t hash = 0;
			while (in >> key >> std::hex >> hash >> std::dec)
				known[key] = hash;
		}
		org::services::ShaderStoreOptions options;
		options.sourceRoot = a_directory;
		options.cacheDirectory = a_directory;
		options.readOnly = true;
		options.saveInterval = std::chrono::milliseconds(0);
		a_lookup.store = std::make_unique<org::services::ShaderStore>(std::move(options));
		std::ostringstream index;
		unsigned hashed = 0, reused = 0;
		a_lookup.store->ForEachArtifact([&](const org::services::ShaderStoreHash& a_key, std::uint8_t a_format, std::span<const std::byte> a_binary) {
			if (a_format != kFormatSpirv)
				return;
			const auto name = a_key.ToHex();
			std::uint64_t hash = 0;
			if (const auto it = known.find(name); it != known.end()) {
				hash = it->second;
				++reused;
			} else {
				const GFSDK_Aftermath_SpirvCode code{ a_binary.data(), static_cast<std::uint32_t>(a_binary.size()) };
				GFSDK_Aftermath_ShaderBinaryHash shaderHash{};
				if (!GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GetShaderHashSpirv(GFSDK_Aftermath_Version_API, &code, &shaderHash)))
					return;
				hash = shaderHash.hash;
				++hashed;
			}
			// The hash ignores debug info, so a shader's debug and release builds share one: keep the larger, the debug build.
			if (auto& held = a_lookup.spirv[hash]; held.size < a_binary.size())
				held = { a_key, a_binary.size() };
			index << name << ' ' << std::hex << hash << std::dec << '\n';
		});
		std::ofstream(indexPath) << index.str();
		std::printf("spirv: %zu shaders indexed in %s (%u hashed, %u reused)\n", a_lookup.spirv.size(), a_directory.string().c_str(), hashed, reused);
	}

	// The SPIR-V of an active shader, by the hash the dump names.
	void GFSDK_AFTERMATH_CALL ShaderBinaryLookup(const GFSDK_Aftermath_ShaderBinaryHash* a_hash, PFN_GFSDK_Aftermath_SetData a_set, void* a_user)
	{
		auto& lookup = *static_cast<Lookup*>(a_user);
		if (const auto it = lookup.spirv.find(a_hash->hash); it != lookup.spirv.end()) {
			if (lookup.store->ReadArtifact(it->second.key, kFormatSpirv, lookup.heldSpirv)) {
				a_set(lookup.heldSpirv.data(), static_cast<uint32_t>(lookup.heldSpirv.size()));
				++lookup.spirvFound;
				std::printf("spirv: shader 0x%016llx is artifact %s\n", static_cast<unsigned long long>(a_hash->hash), it->second.key.ToHex().c_str());
				return;
			}
		}
		++lookup.spirvMissing;
	}

	// The shader debug info whose identifier the decoder asks for: a .nvdbg file whose name carries it.
	void GFSDK_AFTERMATH_CALL ShaderDebugInfoLookup(const GFSDK_Aftermath_ShaderDebugInfoIdentifier* a_identifier, PFN_GFSDK_Aftermath_SetData a_set, void* a_user)
	{
		auto& lookup = *static_cast<Lookup*>(a_user);
		char suffix[64];
		std::snprintf(suffix, sizeof(suffix), "-shader-%016llx%016llx.nvdbg", static_cast<unsigned long long>(a_identifier->id[0]),
			static_cast<unsigned long long>(a_identifier->id[1]));
		for (const auto& directory : lookup.directories) {
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(directory, ec)) {
				const auto name = entry.path().filename().string();
				if (name.size() >= std::strlen(suffix) && name.ends_with(suffix)) {
					lookup.held = ReadFile(entry.path());
					if (!lookup.held.empty()) {
						a_set(lookup.held.data(), static_cast<uint32_t>(lookup.held.size()));
						++lookup.found;
						return;
					}
				}
			}
		}
		++lookup.missing;
	}

	const char* StatusName(GFSDK_Aftermath_Device_Status a_status)
	{
		switch (a_status) {
		case GFSDK_Aftermath_Device_Status_Active:
			return "active";
		case GFSDK_Aftermath_Device_Status_Timeout:
			return "timeout (TDR)";
		case GFSDK_Aftermath_Device_Status_OutOfMemory:
			return "out of memory";
		case GFSDK_Aftermath_Device_Status_PageFault:
			return "page fault";
		case GFSDK_Aftermath_Device_Status_Stopped:
			return "stopped";
		case GFSDK_Aftermath_Device_Status_Reset:
			return "reset";
		case GFSDK_Aftermath_Device_Status_DmaFault:
			return "DMA fault";
		default:
			return "unknown";
		}
	}

	// A marker's text when it is one (a resolved label), else its bytes as hex.
	std::string MarkerText(const GFSDK_Aftermath_GpuCrashDump_EventMarkerInfo& a_marker)
	{
		const auto* data = static_cast<const char*>(a_marker.markerData);
		if (!data || !a_marker.markerDataSize)
			return "(no data)";
		std::string text(data, a_marker.markerDataSize);
		while (!text.empty() && text.back() == '\0')
			text.pop_back();
		bool printable = !text.empty();
		for (const unsigned char c : text)
			printable &= std::isprint(c) || c == '\t';
		if (printable)
			return text;
		std::string hex;
		char byte[4];
		for (std::uint32_t i = 0; i < a_marker.markerDataSize && i < 32; ++i) {
			std::snprintf(byte, sizeof(byte), "%02x", static_cast<unsigned char>(data[i]));
			hex += byte;
		}
		return "0x" + hex;
	}

	bool Ok(GFSDK_Aftermath_Result a_result, const char* a_what)
	{
		if (GFSDK_Aftermath_SUCCEED(a_result))
			return true;
		std::fprintf(stderr, "%s failed (0x%08x)\n", a_what, static_cast<unsigned>(a_result));
		return false;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: aftermath-decode <dump.nv-gpudmp> [--debug-info <dir>] [--spirv <dir>] [--json <out.json>]\n");
		return 2;
	}
	const fs::path dumpPath = argv[1];
	Lookup lookup;
	lookup.directories.push_back(dumpPath.has_parent_path() ? dumpPath.parent_path() : fs::current_path());
	fs::path jsonPath = dumpPath;
	jsonPath += ".json";
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string option = argv[i];
		if (option == "--debug-info")
			lookup.directories.insert(lookup.directories.begin(), argv[i + 1]);
		else if (option == "--spirv")
			IndexSpirv(lookup, argv[i + 1]);
		else if (option == "--json")
			jsonPath = argv[i + 1];
	}

	const auto dump = ReadFile(dumpPath);
	if (dump.empty()) {
		std::fprintf(stderr, "cannot read %s\n", dumpPath.string().c_str());
		return 1;
	}
	GFSDK_Aftermath_GpuCrashDump_Decoder decoder{};
	if (!Ok(GFSDK_Aftermath_GpuCrashDump_CreateDecoder(GFSDK_Aftermath_Version_API, dump.data(), static_cast<uint32_t>(dump.size()), &decoder), "CreateDecoder"))
		return 1;

	GFSDK_Aftermath_GpuCrashDump_BaseInfo base{};
	if (Ok(GFSDK_Aftermath_GpuCrashDump_GetBaseInfo(decoder, &base), "GetBaseInfo"))
		std::printf("dump: %s, %s, pid %u\n", base.applicationName, base.creationDate, base.pid);
	GFSDK_Aftermath_GpuCrashDump_DeviceInfo device{};
	if (Ok(GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo(decoder, &device), "GetDeviceInfo"))
		std::printf("device: %s%s%s\n", StatusName(device.status), device.adapterReset ? ", adapter reset" : "", device.engineReset ? ", engine reset" : "");

	GFSDK_Aftermath_GpuCrashDump_PageFaultInfo fault{};
	if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetPageFaultInfo(decoder, &fault))) {
		std::printf("page fault: VA 0x%016llx, fault type %d, access %d, engine %d, client %d, %u resources\n", static_cast<unsigned long long>(fault.faultingGpuVA),
			static_cast<int>(fault.faultType), static_cast<int>(fault.accessType), static_cast<int>(fault.engine), static_cast<int>(fault.client), fault.resourceInfoCount);
		if (fault.resourceInfoCount) {
			std::vector<GFSDK_Aftermath_GpuCrashDump_ResourceInfo> resources(fault.resourceInfoCount);
			if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetPageFaultResourceInfo(decoder, fault.resourceInfoCount, resources.data())))
				for (const auto& resource : resources)
					std::printf("  resource: VA 0x%016llx, %llu bytes, %ux%u, format %u, %s%s\n", static_cast<unsigned long long>(resource.gpuVa),
						static_cast<unsigned long long>(resource.size), resource.width, resource.height, static_cast<unsigned>(resource.format),
						resource.bIsBufferHeap ? "buffer heap" : (resource.bIsStaticTextureHeap ? "texture heap" : (resource.bIsRenderTargetOrDepthStencilViewHeap ? "RT/DS heap" : "resource")),
						resource.bWasDestroyed ? ", DESTROYED" : "");
		}
	} else {
		std::printf("page fault: none recorded\n");
	}

	uint32_t shaderCount = 0;
	if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount(decoder, &shaderCount)) && shaderCount) {
		std::vector<GFSDK_Aftermath_GpuCrashDump_ShaderInfo> shaders(shaderCount);
		if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo(decoder, shaderCount, shaders.data())))
			for (const auto& shader : shaders)
				std::printf("active shader: hash 0x%016llx, type %d, debug info uid 0x%016llx%s\n", static_cast<unsigned long long>(shader.shaderHash),
					static_cast<int>(shader.shaderType), static_cast<unsigned long long>(shader.shaderDebugInfoUid), shader.isInternal ? " (internal)" : "");
	} else {
		std::printf("active shaders: none\n");
	}

	uint32_t markerCount = 0;
	if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfoCount(decoder, &markerCount)) && markerCount) {
		std::vector<GFSDK_Aftermath_GpuCrashDump_EventMarkerInfo> markers(markerCount);
		if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfo(decoder, markerCount, markers.data())))
			for (const auto& marker : markers)
				std::printf("marker: context 0x%llx (type %d, status %d): %s\n", static_cast<unsigned long long>(marker.contextId), static_cast<int>(marker.contextType),
					static_cast<int>(marker.contextStatus), MarkerText(marker).c_str());
	} else {
		std::printf("markers: none\n");
	}

	uint32_t jsonSize = 0;
	if (Ok(GFSDK_Aftermath_GpuCrashDump_GenerateJSON(decoder, GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO, GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE,
			   ShaderDebugInfoLookup, lookup.spirv.empty() ? nullptr : ShaderBinaryLookup, nullptr, &lookup, &jsonSize), "GenerateJSON") && jsonSize) {
		std::string json(jsonSize, '\0');
		if (Ok(GFSDK_Aftermath_GpuCrashDump_GetJSON(decoder, jsonSize, json.data()), "GetJSON")) {
			while (!json.empty() && json.back() == '\0')
				json.pop_back();
			std::ofstream(jsonPath, std::ios::binary) << json;
			std::printf("json: %s (%zu bytes; shader debug info %u found, %u missing; SPIR-V %u found, %u missing)\n", jsonPath.string().c_str(), json.size(),
				lookup.found, lookup.missing, lookup.spirvFound, lookup.spirvMissing);
		}
	}
	GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
	return 0;
}
