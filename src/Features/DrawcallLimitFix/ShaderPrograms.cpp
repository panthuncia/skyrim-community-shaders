#include "ShaderPrograms.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>

#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "Switches.h"

#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include <ORGModuleServices/ShaderCompiler.h>
#	define DCLF_HAS_SHADER_COMPILER 1
#endif

namespace DCLF
{
	namespace
	{
		constexpr const char* kSourcePath = "Data/Shaders/Lighting.hlsl";
		constexpr const char* kUtilitySourcePath = "Data/Shaders/Utility.hlsl";
		constexpr const char* kShaderDirectory = RenderGraphRuntime::kShaderDirectory;
		constexpr std::size_t kMaxLoggedFailures = 8;

		std::wstring Widen(const std::string& a_value)
		{
			return std::wstring(a_value.begin(), a_value.end());
		}

		// CS_DCLF_SHADER_DEBUG=1 builds the SPIR-V with source-level debug info (for Nsight and
		// RenderDoc; the source is embedded, and SnapshotSources' copy is there for editing), still optimized. Part of the
		// compile key, so the debug builds are cached beside the release ones. There is deliberately no
		// -Od form: unoptimized code reads per-frame constant buffers (VS b6, PS b7) that the epochs do
		// not supply, so every candidate is skipped and DCLF draws nothing.
		bool ShaderDebug()
		{
			static const bool enabled = SwitchEnabled("CS_DCLF_SHADER_DEBUG");
			return enabled;
		}

		// The shader tree as the game sees it through MO2's virtual file system, copied to a real
		// directory a debugger outside the VFS can use as a source search path. The debug info names
		// files relative to the game directory (Data/Shaders/...), so the copy keeps that layout.
		void SnapshotSources(const std::vector<std::filesystem::path>& a_files)
		{
			auto root = std::filesystem::path(SwitchValue("CS_DCLF_SHADER_SOURCE_DIR"));
			if (root.empty()) {
				root = SwitchesDirectory();
				if (root.empty())
					return;
				root /= L"CommunityShaders-ShaderSource";
			}
			std::size_t copied = 0;
			for (const auto& file : a_files) {
				std::error_code ec;
				const auto destination = root / file.lexically_normal();
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (!ec && std::filesystem::copy_file(file, destination, std::filesystem::copy_options::overwrite_existing, ec))
					++copied;
			}
			logger::info("[DCLF] Shader debug info on; {} of {} shader files copied to {}", copied, a_files.size(), root.string());
		}
	}

	bool BindlessObjects()
	{
		// Default ON, disabled with =0. The permutations are compiled against this answer, so the two
		// forms cannot be compared within one run; =0 is the control, and it is the form every
		// pre-Step-D measurement in the docs was taken with.
		static const bool enabled = SwitchValue("CS_DCLF_BINDLESS") != "0";
		return enabled;
	}

	bool BindlessDraws()
	{
		// Default ON. This is what switches on Step D's binding-record deduplication: without it every
		// candidate assembles its own record, which measured ~1538 records for 1538 draws against 105
		// with it, and ~1.6 ms a frame across the two epochs.
		//
		// Validated over the live test matrix: interiors with rooms and portals, exteriors, six cell
		// transitions, a worldspace change, night lighting, a dungeon with alpha-tested foliage, and
		// save and load - 0 parity mismatches, 0 rejected buffers, correct images - plus the map menu
		// and the first-person camera checked by hand.
		static const bool enabled = BindlessObjects() && SwitchValue("CS_DCLF_BINDLESS_DRAW") != "0";
		return enabled;
	}

	struct ShaderPrograms::ShadowEntry
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		std::shared_future<org::services::ShaderArtifact> vertex;
		std::shared_future<org::services::ShaderArtifact> pixel;
#endif
		std::unique_ptr<ShadowProgram> program;
		bool failed = false;
	};

	struct ShaderPrograms::Entry
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		std::shared_future<org::services::ShaderArtifact> vertex;
		std::shared_future<org::services::ShaderArtifact> pixel;
		std::shared_future<org::services::ShaderArtifact> depthPixel;
#endif
		std::unique_ptr<Program> program;
		bool failed = false;
	};

	ShaderPrograms::ShaderPrograms() = default;
	ShaderPrograms::~ShaderPrograms() = default;

	ShaderPrograms& ShaderPrograms::Get()
	{
		static ShaderPrograms programs;
		return programs;
	}

	bool ShaderPrograms::Enabled() const
	{
		return !sourcesMissing && RenderGraphRuntime::Get().ShaderCompiler() != nullptr;
	}

	bool ShaderPrograms::LoadSources()
	{
		if (sourcesLoaded)
			return true;
		std::ifstream file(kSourcePath, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (bytes.empty()) {
			logger::error("[DCLF] {} is missing; no SPIR-V builds", kSourcePath);
			sourcesMissing = true;
			return false;
		}
		source.resize(bytes.size());
		std::memcpy(source.data(), bytes.data(), bytes.size());
		// Every shader file is a potential include; the compiler re-hashes one only when it changes.
		dependencies = RenderGraphRuntime::ShaderSourceFiles();
		{
			std::ifstream utility(kUtilitySourcePath, std::ios::binary);
			std::vector<char> utilityBytes((std::istreambuf_iterator<char>(utility)), std::istreambuf_iterator<char>());
			utilitySource.resize(utilityBytes.size());
			if (!utilityBytes.empty())
				std::memcpy(utilitySource.data(), utilityBytes.data(), utilityBytes.size());
			else
				logger::warn("[DCLF] {} is missing; the shadow views stay native", kUtilitySourcePath);
		}
		sourcesLoaded = true;
		logger::info("[DCLF] SPIR-V builds of {}: {} shader files tracked as dependencies", kSourcePath, dependencies.size());
		if (ShaderDebug())
			SnapshotSources(dependencies);
		return true;
	}

#if defined(DCLF_HAS_SHADER_COMPILER)
	namespace
	{
		std::shared_future<org::services::ShaderArtifact> RequestStage(std::span<const std::byte> a_source, const std::vector<std::filesystem::path>& a_dependencies,
			RE::BSShader& a_lighting, bool a_pixel, std::uint32_t a_descriptor, bool a_depthOnly = false, const char* a_sourceName = kSourcePath)
		{
			org::services::ShaderCompileRequest request{};
			request.sourceName = a_sourceName;
			request.source = a_source;
			request.entryPoint = L"main";
			request.target = a_pixel ? L"ps_6_6" : L"vs_6_6";
			request.format = org::services::ShaderBinaryFormat::Spirv;
			request.warningsAsErrors = false;
			request.debugInfo = ShaderDebug();
			request.languageVersion = L"2018";  // CS's shaders are written for FXC's semantics
			request.includeDirectories = { kShaderDirectory };
			request.dependencyFiles = a_dependencies;
			for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(a_lighting, a_pixel ? SIE::ShaderClass::Pixel : SIE::ShaderClass::Vertex, a_descriptor))
				request.defines.push_back({ Widen(name), Widen(value) });
			request.defines.push_back({ L"DCLF_ORG", L"1" });
			// The Z-prepass build of the pixel stage: everything past the alpha test is compiled away, so it
			// reads only what the discard needs and none of the per-frame pixel bindings.
			if (a_depthOnly)
				request.defines.push_back({ L"DCLF_DEPTH_ONLY", L"1" });
			// The five per-object PerGeometry variables come from a GPU-resident table indexed by the draw's
			// own object index instead of from its constant buffer. CS_DCLF_BINDLESS=0 builds the constant
			// buffer form instead, which is the control the two are compared against.
			if (BindlessObjects())
				request.defines.push_back({ L"DCLF_BINDLESS", L"1" });
			// The three registers that still made the binding record per-object read from the record too.
			if (BindlessDraws())
				request.defines.push_back({ L"DCLF_BINDLESS_DRAW", L"1" });
			// The first few define sets, to reproduce builds with the DXC command line.
			static std::atomic<std::uint32_t> logged = 0;
			if (logged.fetch_add(1) < 4) {
				std::string text;
				for (const auto& define : request.defines)
					text += fmt::format(" -D {}{}{}", Util::WStringToString(define.name), define.value.empty() ? "" : "=", Util::WStringToString(define.value));
				logger::info("[DCLF] SPIR-V build of {} {} {:08X} defines:{}", a_sourceName == kSourcePath ? "Lighting" : "Utility",
					a_pixel ? (a_depthOnly ? "PS (depth)" : "PS") : "VS", a_descriptor, text);
			}
			const auto shift = [&](const wchar_t* a_flag, std::uint32_t a_value) {
				request.arguments.insert(request.arguments.end(), { a_flag, std::to_wstring(a_value), L"0" });
			};
			shift(L"-fvk-b-shift", kBindingShiftB);
			shift(L"-fvk-t-shift", kBindingShiftT);
			shift(L"-fvk-s-shift", kBindingShiftS);
			shift(L"-fvk-u-shift", kBindingShiftU);
			return RenderGraphRuntime::Get().ShaderCompiler()->CompileAsync(std::move(request));
		}
	}
#endif

	const ShaderPrograms::Program* ShaderPrograms::Find(const PipelineKey& a_key, RE::BSShader& a_lighting)
	{
		if (!Enabled() || !LoadSources())
			return nullptr;
		const std::uint64_t id = (static_cast<std::uint64_t>(a_key.vertexDescriptor) << 32) | a_key.pixelDescriptor;
		auto [it, inserted] = entries.try_emplace(id);
		if (inserted) {
			it->second = std::make_unique<Entry>();
#if defined(DCLF_HAS_SHADER_COMPILER)
			it->second->vertex = RequestStage(source, dependencies, a_lighting, false, a_key.vertexDescriptor);
			it->second->pixel = RequestStage(source, dependencies, a_lighting, true, a_key.pixelDescriptor);
			it->second->depthPixel = RequestStage(source, dependencies, a_lighting, true, a_key.pixelDescriptor, true);
#else
			(void)a_lighting;
			it->second->failed = true;
#endif
			++stats.requested;
		}
		return it->second->program.get();
	}

	const ShaderPrograms::ShadowProgram* ShaderPrograms::FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility)
	{
		if (!Enabled() || !LoadSources() || utilitySource.empty())
			return nullptr;
		auto [it, inserted] = shadowEntries.try_emplace(a_technique);
		if (inserted) {
			it->second = std::make_unique<ShadowEntry>();
#if defined(DCLF_HAS_SHADER_COMPILER)
			// Both stages take the same technique: Utility's descriptor is the technique itself, not a
			// pair of vertex and pixel descriptors as the Lighting shader's is.
			it->second->vertex = RequestStage(utilitySource, dependencies, a_utility, false, a_technique, false, kUtilitySourcePath);
			it->second->pixel = RequestStage(utilitySource, dependencies, a_utility, true, a_technique, false, kUtilitySourcePath);
#else
			(void)a_utility;
			it->second->failed = true;
#endif
			++stats.shadowRequested;
		}
		return it->second->program.get();
	}

	void ShaderPrograms::Update()
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		const auto ready = [](const auto& a_future) {
			return a_future.valid() && a_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		};
		for (auto& [id, entryPointer] : entries) {
			auto& entry = *entryPointer;
			if (entry.program || entry.failed || !ready(entry.vertex) || !ready(entry.pixel) || !ready(entry.depthPixel))
				continue;
			const auto& vertex = entry.vertex.get();
			const auto& pixel = entry.pixel.get();
			const auto& depthPixel = entry.depthPixel.get();
			if (!vertex || !pixel || !depthPixel) {
				entry.failed = true;
				++stats.failed;
				if (loggedFailures++ < kMaxLoggedFailures) {
					const auto& failed = !vertex ? vertex : pixel;
					std::string diagnostics = failed.diagnostics.substr(0, 1500);
					logger::warn("[DCLF] SPIR-V build of Lighting {} {:08X} failed:\n{}", !vertex ? "VS" : "PS",
						static_cast<std::uint32_t>(!vertex ? (id >> 32) : id), diagnostics);
				}
				continue;
			}
			entry.program = std::make_unique<Program>(Program{ vertex.binary, pixel.binary, depthPixel.binary });
			++stats.ready;
			stats.fromCache += (vertex.fromCache ? 1 : 0) + (pixel.fromCache ? 1 : 0) + (depthPixel.fromCache ? 1 : 0);
		}
		for (auto& [technique, entryPointer] : shadowEntries) {
			auto& entry = *entryPointer;
			if (entry.program || entry.failed || !ready(entry.vertex) || !ready(entry.pixel))
				continue;
			const auto& vertex = entry.vertex.get();
			const auto& pixel = entry.pixel.get();
			if (!vertex || !pixel) {
				entry.failed = true;
				++stats.shadowFailed;
				if (loggedFailures++ < kMaxLoggedFailures) {
					const auto& failed = !vertex ? vertex : pixel;
					logger::warn("[DCLF] SPIR-V build of Utility {} {:08X} failed:\n{}", !vertex ? "VS" : "PS", technique, failed.diagnostics.substr(0, 1500));
				}
				continue;
			}
			entry.program = std::make_unique<ShadowProgram>(ShadowProgram{ vertex.binary, pixel.binary });
			++stats.shadowReady;
			stats.fromCache += (vertex.fromCache ? 1 : 0) + (pixel.fromCache ? 1 : 0);
		}
#endif
	}
}
