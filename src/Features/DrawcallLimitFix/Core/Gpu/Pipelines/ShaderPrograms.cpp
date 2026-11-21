// The stage cache, the compile requests and the pipeline lane's program entries.

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "RenderGraph/Fatal.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DxilStages.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PulledSource.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include <ORGModuleServices/PipelineRegistry.h>
#include "Utils/SharedShaderStore.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/ShaderTarget.h"
#include "ShaderCache.h"
#include "RenderGraph/Async/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"

#include <ORGModuleServices/ShaderCompiler.h>
#include "TextureStreaming/Streamer.h"

namespace DCLF
{
	namespace
	{
		/** @brief The kind of program entry waiting for a stage. */
		enum class StageUser : std::uint8_t
		{
			Lighting,
			Shadow,
			Forward,
			// A draw family's user is DrawFamily or DrawFamilyForward plus its index.
			DrawFamily,
			DrawFamilyForward = DrawFamily + kDrawFamilyLanes,
			Family = DrawFamilyForward + kDrawFamilyLanes  // a family's is Family + its index
		};

		StageUser FamilyUser(Api::ProgramFamily a_family) { return static_cast<StageUser>(static_cast<std::uint8_t>(StageUser::Family) + static_cast<std::uint8_t>(a_family)); }
		std::size_t FamilyIndexOf(StageUser a_user) { return static_cast<std::size_t>(a_user) - static_cast<std::size_t>(StageUser::Family); }
		StageUser DrawFamilyUser(Api::DrawFamily a_family, bool a_forward)
		{
			return static_cast<StageUser>(static_cast<std::uint8_t>(a_forward ? StageUser::DrawFamilyForward : StageUser::DrawFamily) + static_cast<std::uint8_t>(a_family));
		}
		bool IsDrawFamilyUser(StageUser a_user) { return a_user >= StageUser::DrawFamily && a_user < StageUser::Family; }
		bool IsDrawFamilyForward(StageUser a_user) { return a_user >= StageUser::DrawFamilyForward && a_user < StageUser::Family; }
		std::size_t DrawFamilyIndexOf(StageUser a_user)
		{
			return static_cast<std::size_t>(a_user) - static_cast<std::size_t>(IsDrawFamilyForward(a_user) ? StageUser::DrawFamilyForward : StageUser::DrawFamily);
		}
	}

	namespace
	{
		constexpr const char* kSourcePath = "Data/Shaders/Lighting.hlsl";
		constexpr const char* kUtilitySourcePath = "Data/Shaders/Utility.hlsl";
		constexpr const char* kShaderDirectory = RenderGraphRuntime::kShaderDirectory;

		std::wstring Widen(const std::string& a_value)
		{
			return std::wstring(a_value.begin(), a_value.end());
		}

		// Built with debug info by default (CS_DCLF_SHADER_DEBUG=0 turns it off) so crash dumps name the shader, still optimized.
		// There is deliberately no -Od form. Unoptimized code reads per-frame constant buffers the epochs do not supply, so DCLF would draw nothing.
		bool ShaderDebug()
		{
#if defined(CS_RELEASE_PROFILE)
			return SwitchValue(Switch::ShaderDebug) == "1";
#else
			return SwitchValue(Switch::ShaderDebug) != "0";
#endif
		}

		// Copy the shader tree out of MO2's virtual file system so a debugger outside it can find the sources.
		// Keeps the Data/Shaders layout and only writes files newer than their copy.
		void SnapshotSources(const std::vector<std::filesystem::path>& a_files)
		{
			auto root = std::filesystem::path(SwitchValue(Switch::ShaderSourceDir));
			if (root.empty()) {
				root = SwitchesDirectory();
				if (root.empty())
					return;
				root /= L"CommunityShaders-ShaderSource";
			}
			std::size_t copied = 0, current = 0;
			std::string firstError;
			for (const auto& file : a_files) {
				std::error_code ec;
				const auto destination = root / file.lexically_normal();
				// copy_options::update_existing fails every file through the VFS, so compare size and time by hand.
				std::error_code sizeError, timeError;
				const auto size = std::filesystem::file_size(destination, sizeError);
				const auto written = std::filesystem::last_write_time(destination, timeError);
				if (!sizeError && !timeError && size == std::filesystem::file_size(file, ec) && !ec && written >= std::filesystem::last_write_time(file, ec) && !ec) {
					++current;
					continue;
				}
				ec.clear();
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (!ec && std::filesystem::copy_file(file, destination, std::filesystem::copy_options::overwrite_existing, ec))
					++copied;
				else if (firstError.empty())
					firstError = fmt::format("; first failure {}: {}", file.string(), ec.message());
			}
			logger::info("[DCLF] Shader debug info on; {} of {} shader files copied to {} ({} already current){}", copied, a_files.size(), root.string(), current, firstError);
		}
	}

	namespace
	{
		/** @brief One stage's compile. Kept for the process, as the programs are. */
		struct Stage
		{
			std::uint64_t key = 0;
			std::shared_ptr<const org::services::ShaderArtifact> artifact;  // written once, before done
			std::atomic<bool> done{ false };
			// A pulled vertex stage's attributes, written before the stage is shared. 0 otherwise.
			std::uint32_t vertexAttributes = 0;
			std::uint64_t sourcesGeneration = 0;
		};
		using StagePtr = std::shared_ptr<Stage>;

		/** @brief Completed stage keys, pushed from any thread and drained by the pipeline lane. */
		RenderGraph::Frame::EventQueue<std::uint64_t>& StageCompletions()
		{
			// Never destroyed, because a compile may complete while the process tears down.
			static auto* queue = new RenderGraph::Frame::EventQueue<std::uint64_t>;
			return *queue;
		}

		struct StageWaiter
		{
			StageUser user = StageUser::Lighting;
			std::uint64_t id = 0;
		};
	}

	template <class P, std::size_t N>
	struct ShaderPrograms::ProgramEntry
	{
		std::array<StagePtr, N> stages;
		std::uint8_t remaining = 0;  // stages not completed when requested
		std::shared_ptr<P> program;  // shared with the builds made from it
		bool failed = false;
		std::uint8_t onDemand = 0;  // stages no precompile had asked for
		std::chrono::steady_clock::time_point requested;
	};

	std::string ShaderPrograms::OnDemandStages(std::uint8_t a_stages)
	{
		std::string text;
		for (const auto& [bit, name] : { std::pair{ kOnDemandVertex, "VS" }, std::pair{ kOnDemandPixel, "PS" }, std::pair{ kOnDemandDepthPixel, "depth PS" },
				 std::pair{ kOnDemandPulledVertex, "pulled VS" }, std::pair{ kOnDemandPulledDepthPixel, "pulled depth PS" }, std::pair{ kOnDemandPulledPixel, "pulled PS" },
				 std::pair{ kOnDemandPulledDepthVertex, "pulled depth VS" } })
			if (a_stages & bit)
				text += fmt::format("{}{}", text.empty() ? "" : ", ", name);
		return text;
	}

	ShaderPrograms& ShaderPrograms::Get()
	{
		static ShaderPrograms programs;
		return programs;
	}

	bool ShaderPrograms::Enabled() const
	{
		return !sourcesMissing && RenderGraphRuntime::Get().ShaderCompiler() != nullptr;
	}

	std::uint64_t ShaderPrograms::CurrentSourcesGeneration()
	{
		const auto* registry = RenderGraphRuntime::PipelineRegistry();
		return registry ? registry->SourcesGeneration() : 0;
	}


	namespace
	{
		/**
		 * @brief CS_DCLF_STAGE_DUMP=<dir> writes stages.jsonl so tools/shader-compare can compile the same stages offline.
		 * Any thread. The file is truncated at a launch's first line.
		 */
		void DumpStageLine(const std::string& a_line)
		{
			static const std::string directory = SwitchValue(Switch::StageDump);
			if (directory.empty())
				return;
			static std::mutex mutex;
			static std::ofstream out;
			static bool opened = false;
			std::lock_guard lock(mutex);
			if (!opened) {
				opened = true;
				std::error_code ec;
				std::filesystem::create_directories(directory, ec);
				out.open(std::filesystem::path(directory) / "stages.jsonl", std::ios::binary | std::ios::trunc);
				logger::info("[DCLF] stage dump to {}{}", directory, out ? "" : ": the file did not open");
			}
			if (out) {
				out << a_line << '\n';
				out.flush();
			}
		}

		std::string JsonString(std::string_view a_text)
		{
			std::string out = "\"";
			for (const char c : a_text) {
				if (c == '"' || c == '\\')
					out += '\\';
				out += c;
			}
			return out + "\"";
		}

		std::string JsonDefines(std::span<const org::services::ShaderDefine> a_defines)
		{
			std::string out = "[";
			for (std::size_t i = 0; i < a_defines.size(); ++i)
				out += fmt::format("{}[{},{}]", i ? "," : "", JsonString(Util::WStringToString(a_defines[i].name)), JsonString(Util::WStringToString(a_defines[i].value)));
			return out + "]";
		}
	}

	namespace
	{
		std::vector<std::byte> ReadShaderFile(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary);
			const std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			std::vector<std::byte> bytes(chars.size());
			if (!chars.empty())
				std::memcpy(bytes.data(), chars.data(), chars.size());
			return bytes;
		}

		// The rewrite's game inputs: the material textures, the shadow mask and the rows' samplers.
		PulledSource::Options PulledOptions(const std::unordered_set<std::string>* a_eager)
		{
			PulledSource::Options options;
			for (std::uint32_t r = 0; r < options.materialTexture.size(); ++r)
				options.materialTexture[r] = (r < kPixelTextureSlots && r != kShadowMaskSlot) || Extensions::FeatureTextureSlotOf(r) >= 0;
			options.shadowMaskSlot = kShadowMaskSlot;
			options.samplerSlots = 16;
			options.eager = a_eager;
			return options;
		}

		// Empty and logged when the rewrite fails.
		std::vector<std::byte> PulledLightingSource(std::span<const std::byte> a_source, const PulledSource::Options& a_options)
		{
			const auto result = PulledSource::Rewrite(std::string_view(reinterpret_cast<const char*>(a_source.data()), a_source.size()), a_options);
			if (!result.error.empty()) {
				logger::error("[DCLF] Pulled Lighting source: {}", result.error);
				return {};
			}
			logger::info("[DCLF] Pulled Lighting source: {} texture, {} sampler and {} block member declarations read from the draw's rows ({} where used)",
				result.textures, result.samplers, result.members, result.lazy);
			std::vector<std::byte> bytes(result.source.size());
			std::memcpy(bytes.data(), result.source.data(), result.source.size());
			return bytes;
		}
	}

	/** @brief The shader files of one sources generation. Includes are not listed, since the compiler records what each stage opens. Immutable. */
	struct ShaderPrograms::Sources
	{
		explicit Sources(std::uint64_t a_generation);

		std::uint64_t generation = 0;
		bool missing = false;  // Lighting.hlsl could not be read, so no SPIR-V builds of this generation
		std::vector<std::byte> source;
		std::vector<std::byte> pulledSource;  // empty when it could not be made
		std::vector<std::byte> utilitySource;
		std::array<std::vector<std::byte>, kDrawFamilyLanes> drawFamilySources;
		std::array<std::vector<std::byte>, Extensions::kProgramFamilies> familySources, familyPulledSources;

		/** @brief Where a stage compiles from, and its name for logs. */
		struct Resolved
		{
			std::span<const std::byte> source;
			const char* name = nullptr;
			bool pulled = false;
			bool family = false;
		};
		/** @brief a_stage's source in this generation. False when that source is missing. */
		bool Resolve(const RE::BSShader& a_shader, const ShaderStageSpec& a_stage, Resolved& a_out) const;
	};

	ShaderPrograms::Sources::Sources(std::uint64_t a_generation) :
		generation(a_generation)
	{
		source = ReadShaderFile(kSourcePath);
		if (source.empty()) {
			logger::error("[DCLF] {} is missing; no SPIR-V builds", kSourcePath);
			missing = true;
			return;
		}
		const auto& dependencies = RenderGraphRuntime::ShaderSourceFiles();
		const auto eager = PulledSource::EagerRowNames(dependencies);
		const auto pulledOptions = PulledOptions(&eager);
		if (!SwitchValue(Switch::StageDump).empty()) {
			std::string textures;
			for (std::size_t r = 0; r < pulledOptions.materialTexture.size(); ++r)
				if (pulledOptions.materialTexture[r])
					textures += fmt::format("{}{}", textures.empty() ? "" : ",", r);
			DumpStageLine(fmt::format(R"({{"kind":"options","generation":{},"materialTextures":[{}],"shadowMaskSlot":{},"samplerSlots":{},"lazyRows":true}})", a_generation,
				textures, pulledOptions.shadowMaskSlot, pulledOptions.samplerSlots));
		}
		pulledSource = PulledLightingSource(source, pulledOptions);
		utilitySource = ReadShaderFile(kUtilitySourcePath);
		if (utilitySource.empty())
			logger::warn("[DCLF] {} is missing; the shadow views stay native", kUtilitySourcePath);
		// Draw families' sources first, then the program families'.
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f) {
			const auto& family = Extensions::DrawFamilies()[f];
			if (!family.taken)
				continue;
			drawFamilySources[f] = ReadShaderFile(family.sourcePath);
			if (drawFamilySources[f].empty())
				logger::warn("[DCLF] {} is missing; {}", family.sourcePath, family.nativeNote);
		}
		for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
			const auto& family = Extensions::ProgramFamilies()[f];
			if (!family.taken)
				continue;
			familySources[f] = ReadShaderFile(family.sourcePath);
			if (!familySources[f].empty())
				familyPulledSources[f] = PulledLightingSource(familySources[f], pulledOptions);
			else
				logger::warn("[DCLF] {} is missing; {}", family.sourcePath, family.nativeNote);
		}
		logger::info("[DCLF] SPIR-V builds of {}: {} shader files seen (sources generation {})", kSourcePath, dependencies.size(), generation);
		if (ShaderDebug())
			SnapshotSources(dependencies);
	}

	namespace
	{
		// The logs' name of a source.
		const char* SourceLogName(const char* a_sourceName)
		{
			if (a_sourceName == kSourcePath)
				return "Lighting";
			if (a_sourceName == kUtilitySourcePath)
				return "Utility";
			for (const auto& family : Extensions::ProgramFamilies())
				if (family.taken && family.sourcePath.c_str() == a_sourceName)
					return family.name.c_str();
			for (const auto& family : Extensions::DrawFamilies())
				if (family.taken && family.sourcePath.c_str() == a_sourceName)
					return family.name.c_str();
			return a_sourceName;
		}

		std::uint64_t StageKeyBitOf(Api::ProgramFamily a_family)
		{
			return a_family == Api::ProgramFamily::Count ? 0ull : ProgramFamilySlotOf(a_family).stageKeyBit;
		}
		std::uint64_t DrawStageKeyBitOf(Api::DrawFamily a_family)
		{
			return a_family == Api::DrawFamily::Count ? 0ull : DrawFamilyLaneSlotOf(a_family).stageKeyBit;
		}

		// The attributes a pulled vertex stage reads, from its defines. Kept by hand to match DCLFPulledInput in Lighting.hlsl and
		// Effect.hlsl, so change both together. Lighting.hlsl defines VC itself under TREE_ANIM or LANDSCAPE.
		std::uint32_t VertexAttributesOf(std::span<const org::services::ShaderDefine> a_defines, bool a_lighting)
		{
			constexpr std::uint32_t kPosition = 1u << 0, kTexcoord0 = 1u << 1, kNormal = 1u << 3, kBinormal = 1u << 4, kColor = 1u << 5,
									kSkinning = 1u << 6, kLandData = 1u << 7, kEyeData = 1u << 8;
			auto has = [&](std::wstring_view a_name) { return std::ranges::any_of(a_defines, [&](const org::services::ShaderDefine& a_define) { return a_define.name == a_name; }); };
			std::uint32_t attributes = 0;
			if (a_lighting) {
				attributes = kPosition | kTexcoord0;
				if (!has(L"MODELSPACENORMALS"))
					attributes |= kNormal | kBinormal;
				if (has(L"VC") || has(L"TREE_ANIM") || has(L"LANDSCAPE"))
					attributes |= kColor | (has(L"LANDSCAPE") ? kLandData : 0u);
				if (has(L"SKINNED"))
					attributes |= kSkinning;
				if (has(L"EYE"))
					attributes |= kEyeData;
				return attributes;
			}
			// A particle system's quads read no stream.
			if (has(L"PARTICLES"))
				return 0;
			attributes = kPosition;
			if (has(L"TEXCOORD"))
				attributes |= kTexcoord0;
			if (has(L"NORMALS") || has(L"MOTIONVECTORS_NORMALS"))
				attributes |= kNormal;
			if (has(L"BINORMAL_TANGENT"))
				attributes |= kBinormal;
			if (has(L"VC"))
				attributes |= kColor;
			if (has(L"SKINNED"))
				attributes |= kSkinning;
			return attributes;
		}

		/** @brief A stage's compile request. The lane and the registry's rebuilds share it, so one request means one compiler key. */
		org::services::ShaderCompileRequest MakeStageRequest(std::span<const std::byte> a_source, const RE::BSShader& a_lighting, bool a_pixel, std::uint32_t a_descriptor, bool a_depthOnly, bool a_pulled, bool a_family,
			const char* a_sourceName, std::uint32_t& a_vertexAttributes)
		{
			a_vertexAttributes = 0;
			org::services::ShaderCompileRequest request{};
			DCLF_ZONE("CS.DCLF.Shaders.RequestStage");
			request.sourceName = a_sourceName;
			request.source = a_source;
			request.entryPoint = L"main";
			request.target = a_pixel ? L"ps_6_6" : L"vs_6_6";
			// SPIR-V on Vulkan, DXIL on D3D12.
			::RenderGraph::ShaderTarget::Apply(request);
			request.warningsAsErrors = false;
			request.debugInfo = ShaderDebug();
			request.languageVersion = L"2018";  // CS's shaders are written for FXC's semantics
			request.includeDirectories = { kShaderDirectory };
			for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(a_lighting, a_pixel ? SIE::ShaderClass::Pixel : SIE::ShaderClass::Vertex, a_descriptor))
				request.defines.push_back({ Widen(name), Widen(value) });
			const std::size_t communityShadersDefines = request.defines.size();
			request.defines.push_back({ L"DCLF_ORG", L"1" });
			// The Z-prepass pixel build compiles away everything past the alpha test, so it reads none of the per-frame pixel bindings.
			if (a_depthOnly)
				request.defines.push_back({ L"DCLF_DEPTH_ONLY", L"1" });
			// Per-object values come from a GPU table indexed by the draw's object index, so the binding record is not per-object.
			request.defines.push_back({ L"DCLF_BINDLESS", L"1" });
			request.defines.push_back({ L"DCLF_BINDLESS_DRAW", L"1" });
			// The point light table's size without Light Limit Fix. Must match the CPU's.
			request.defines.push_back({ L"DCLF_POINT_LIGHT_TABLE_LIGHTS", std::to_wstring(Draws::kPointLightTableLights) });
			// Plain indirect draws fetch their own indices, vertices and per-draw words.
			if (a_pulled)
				request.defines.push_back({ L"DCLF_PULLED", L"1" });
			if (a_pulled && (a_sourceName == kSourcePath || a_family))
				request.defines.push_back({ L"DCLF_PULLED_ROWS", L"1" });
			// On D3D12, a pulled build also reads the pass's bindings itself, rewritten by DxilStages before it compiles.
			// Other builds keep their registers.
			const bool utility = a_sourceName == kUtilitySourcePath;
			const bool drawFamily = Extensions::DrawFamilyOfShader(static_cast<std::uint8_t>(a_lighting.shaderType.get())) != Api::DrawFamily::Count;
			if (a_pulled && (a_sourceName == kSourcePath || a_family || utility || drawFamily) && !::RenderGraph::ShaderTarget::IsSpirv()) {
				request.defines.push_back({ DxilStages::kFrameDefine, L"1" });
				if (utility)
					request.defines.push_back({ DxilStages::kShadowFrameDefine, L"1" });
			}
			// D3D12 on AMD indexes device refs as uniform. The driver wraps every non-uniform load in a loop (colour pass 17 ms against 9),
			// and its waves never mix draws anyway. Refs that differ within one draw stay non-uniform.
			if (!::RenderGraph::ShaderTarget::IsSpirv() && RenderGraphRuntime::VendorId() == 0x1002)
				request.defines.push_back({ L"DEVICE_REF_ASSUME_UNIFORM", L"1" });
			// Device-generated vertex stages pull their attributes from the draw's push data. No pipeline has input assembly, which an
			// execution set would otherwise have to share.
			if (!a_pixel && !a_pulled && (a_sourceName == kSourcePath || a_family)) {
				request.defines.push_back({ L"DCLF_PULLED_VERTICES", L"1" });
				a_vertexAttributes = VertexAttributesOf(request.defines, a_sourceName == kSourcePath);
			}
			// A define, so a run without streaming keeps the shaders and pipelines it had.
			if (a_pixel && !a_depthOnly && a_sourceName == kSourcePath && TextureStreaming::Enabled() && !TextureStreaming::FullMode())
				request.defines.push_back({ L"DCLF_TEX_FEEDBACK", L"1" });
			// Log the first few define sets, to reproduce builds with the DXC command line.
			static std::atomic<std::uint32_t> logged = 0;
			if (logged.fetch_add(1) < 4) {
				std::string text;
				for (const auto& define : request.defines)
					text += fmt::format(" -D {}{}{}", Util::WStringToString(define.name), define.value.empty() ? "" : "=", Util::WStringToString(define.value));
				logger::info("[DCLF] {} build of {} {}{} {:08X} defines:{}", ::RenderGraph::ShaderTarget::FormatName(), SourceLogName(a_sourceName), a_pixel ? (a_depthOnly ? "PS (depth)" : "PS") : "VS",
					a_pulled ? " (pulled)" : "", a_descriptor, text);
			}
			const auto shift = [&](const wchar_t* a_flag, std::uint32_t a_value) {
				request.arguments.insert(request.arguments.end(), { a_flag, std::to_wstring(a_value), L"0" });
			};
			// SPIR-V only. On D3D12 a stage keeps its HLSL registers.
			if (::RenderGraph::ShaderTarget::IsSpirv()) {
				shift(L"-fvk-b-shift", kBindingShiftB);
				shift(L"-fvk-t-shift", kBindingShiftT);
				shift(L"-fvk-s-shift", kBindingShiftS);
				shift(L"-fvk-u-shift", kBindingShiftU);
			}
			if (!SwitchValue(Switch::StageDump).empty()) {
				std::string arguments = "[";
				for (std::size_t i = 0; i < request.arguments.size(); ++i)
					arguments += (i ? "," : "") + JsonString(Util::WStringToString(request.arguments[i]));
				const std::span<const org::services::ShaderDefine> defines(request.defines);
				DumpStageLine(fmt::format(R"({{"kind":"stage","source":{},"pixel":{},"depth":{},"pulled":{},"family":{},"descriptor":"{:08X}","target":{},"cs":{},"dclf":{},"arguments":{}]}})",
					JsonString(a_sourceName), a_pixel, a_depthOnly, a_pulled, a_family, a_descriptor, JsonString(Util::WStringToString(request.target)),
					JsonDefines(defines.first(communityShadersDefines)), JsonDefines(defines.subspan(communityShadersDefines)), arguments));
			}
			return request;
		}

		/** @brief Compiles a_request. A D3D12 pulled build is rewritten first on the build executor, which costs about a small compile. */
		void CompileStage(org::services::ShaderCompiler& a_compiler, org::services::ShaderCompileRequest a_request, org::services::ShaderCompiler::CompletionCallback a_done)
		{
			if (!DxilStages::NeedsFrameSource(a_request)) {
				(void)a_compiler.CompileAsync(std::move(a_request), std::move(a_done));
				return;
			}
			// Copied, because the task may outlive the caller's sources.
			auto source = std::make_shared<std::string>(reinterpret_cast<const char*>(a_request.source.data()), a_request.source.size());
			a_request.source = std::as_bytes(std::span(source->data(), source->size()));
			auto task = [request = std::move(a_request), source, done = std::move(a_done)]() mutable {
				(void)source;  // held until the rewrite replaces it
				std::string storage, error;
				auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
				if (!compiler)
					error = "no runtime shader compiler";
				else if (DxilStages::PrepareFrameSource(request, storage, error)) {
					// The compiler copies the rewritten source before this returns.
					(void)compiler->CompileAsync(std::move(request), std::move(done));
					return;
				}
				org::services::ShaderArtifact failed{};
				failed.format = org::services::ShaderBinaryFormat::Dxil;
				failed.diagnostics = std::move(error);
				done(failed);
			};
			if (!BuildSubmitter()(std::function<void()>(std::move(task))))
				logger::error("[DCLF] the build executor refused a D3D12 stage's preparation");
		}

		void RequestStage(org::services::ShaderCompiler& a_compiler, const StagePtr& a_stage, org::services::ShaderCompileRequest a_request)
		{
			// The future is not kept. The stage holds the artifact.
			CompileStage(a_compiler, std::move(a_request), [a_stage](const org::services::ShaderArtifact& a_artifact) {
				a_stage->artifact = std::make_shared<const org::services::ShaderArtifact>(a_artifact);
				a_stage->done.store(true, std::memory_order_release);
				a_stage->done.notify_all();
				// A stage two kinds share is waited on by both, and UpdateLane tells each.
				StageCompletions().Push(a_stage->key);
				WakePipelineLane();
			});
		}

		/** @brief A stage's key in the stage cache, and whether it is a pulled build. */
		std::uint64_t StageKeyOf(const RE::BSShader& a_shader, const ShaderStageSpec& a_stage, bool& a_pulled)
		{
			const bool utility = a_shader.shaderType.get() == RE::BSShader::Type::Utility;
			const auto drawFamily = Extensions::DrawFamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
			const auto family = Extensions::FamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
			a_pulled = family != Api::ProgramFamily::Count ? a_stage.pulled :
			                                                 (a_stage.pulled || utility || (drawFamily != Api::DrawFamily::Count && DrawFamilyLaneSlotOf(drawFamily).pulled));
			return a_stage.descriptor | (std::uint64_t(a_stage.pixel) << 32) | (std::uint64_t(a_stage.depth) << 33) | (std::uint64_t(utility) << 34) |
			       (std::uint64_t(a_pulled) << 35) | DrawStageKeyBitOf(drawFamily) | StageKeyBitOf(family);
		}

		/** @brief Mixes a stage binary into a program's fingerprint. */
		std::uint64_t MixBinary(std::uint64_t a_seed, const std::vector<std::byte>& a_binary)
		{
			return ankerl::unordered_dense::detail::wyhash::mix(a_seed ^ ankerl::unordered_dense::detail::wyhash::hash(a_binary.data(), a_binary.size()),
				0x9E3779B97F4A7C15ull);
		}

		// Each kind's stage list, shared by the lane's Find* and the registry's Rebuild*, so both make the same requests and fingerprints.
		std::array<ShaderStageSpec, 7> LightingStages(const PipelineKey& a_key)
		{
			const std::uint32_t v = a_key.vertexDescriptor, p = a_key.pixelDescriptor;
			// vertex, pixel, depth pixel, pulled vertex, pulled depth pixel, pulled pixel, pulled depth vertex
			return { ShaderStageSpec{ false, v }, ShaderStageSpec{ true, p }, ShaderStageSpec{ true, p, true }, ShaderStageSpec{ false, v, false, true },
				ShaderStageSpec{ true, p, true, true }, ShaderStageSpec{ true, p, false, true }, ShaderStageSpec{ false, v, true, true } };
		}
		std::array<ShaderStageSpec, 6> FamilyStages(const PipelineKey& a_key)
		{
			// Program's order with the depth slots repeating the pixel stages. A stage listed twice is waited for twice, harmlessly.
			const std::uint32_t v = a_key.vertexDescriptor, p = a_key.pixelDescriptor;
			return { ShaderStageSpec{ false, v }, ShaderStageSpec{ true, p }, ShaderStageSpec{ true, p }, ShaderStageSpec{ false, v, false, true },
				ShaderStageSpec{ true, p, false, true }, ShaderStageSpec{ true, p, false, true } };
		}
		std::array<ShaderStageSpec, 2> ShadowStages(std::uint32_t a_technique)
		{
			// Utility's descriptor is the technique itself, so both stages take it.
			return { ShaderStageSpec{ false, a_technique }, ShaderStageSpec{ true, a_technique } };
		}
		std::array<ShaderStageSpec, 6> DrawFamilyStages(Api::DrawFamily a_family)
		{
			// Colour, depth, then the second colour pair. With one colour program the first repeats and KeepColourPrograms empties it.
			const auto& program = Extensions::DrawFamilyOf(a_family).contribution.program;
			const auto& descriptors = program.descriptors;
			const std::uint32_t colour2 = program.colours == 2 ? descriptors.colour2 : descriptors.colour;
			return { ShaderStageSpec{ false, descriptors.colour }, ShaderStageSpec{ true, descriptors.colour }, ShaderStageSpec{ false, descriptors.depth },
				ShaderStageSpec{ true, descriptors.depth }, ShaderStageSpec{ false, colour2 }, ShaderStageSpec{ true, colour2 } };
		}
		/** @brief Keeps the second colour pair only with two colour programs. */
		void KeepColourPrograms(Api::DrawFamily a_family, ShaderPrograms::DrawProgram& a_program)
		{
			if (Extensions::DrawFamilyOf(a_family).contribution.program.colours == 2)
				return;
			a_program.vertex2 = {};
			a_program.pixel2 = {};
		}
		std::array<ShaderStageSpec, 2> ForwardStages(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor)
		{
			// The pulled vertex stage is the Z-prepass's. The pixel stage is the colour one.
			return { ShaderStageSpec{ false, a_vertexDescriptor, false, true }, ShaderStageSpec{ true, a_pixelDescriptor, false, true } };
		}
		std::array<ShaderStageSpec, 2> DrawFamilyForwardStages(Api::DrawFamily a_family)
		{
			const std::uint32_t forward = Extensions::DrawFamilyOf(a_family).contribution.program.descriptors.forward;
			return { ShaderStageSpec{ false, forward }, ShaderStageSpec{ true, forward } };
		}
	}

	std::string ShaderPrograms::SourcePathOf(const RE::BSShader& a_shader)
	{
		if (a_shader.shaderType.get() == RE::BSShader::Type::Utility)
			return kUtilitySourcePath;
		if (const auto drawFamily = Extensions::DrawFamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get())); drawFamily != Api::DrawFamily::Count)
			return Extensions::DrawFamilyOf(drawFamily).sourcePath;
		if (const auto family = Extensions::FamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get())); family != Api::ProgramFamily::Count)
			return Extensions::ProgramFamilyOf(family).sourcePath;
		return kSourcePath;
	}

	bool ShaderPrograms::Sources::Resolve(const RE::BSShader& a_shader, const ShaderStageSpec& a_stage, Resolved& a_out) const
	{
		const bool utility = a_shader.shaderType.get() == RE::BSShader::Type::Utility;
		const auto drawFamily = Extensions::DrawFamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
		const bool drawBuild = drawFamily != Api::DrawFamily::Count;
		const auto d = static_cast<std::size_t>(drawFamily);
		const auto family = Extensions::FamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
		const bool familyBuild = family != Api::ProgramFamily::Count;
		const auto f = static_cast<std::size_t>(family);
		bool pulled = false;
		(void)StageKeyOf(a_shader, a_stage, pulled);
		if (missing || (utility && utilitySource.empty()) || (drawBuild && drawFamilySources[d].empty()) ||
			(familyBuild && (pulled ? familyPulledSources[f] : familySources[f]).empty()) || (pulled && !utility && !drawBuild && !familyBuild && pulledSource.empty()))
			return false;
		a_out.source = utility ? utilitySource : drawBuild ? drawFamilySources[d] : familyBuild ? (pulled ? familyPulledSources[f] : familySources[f]) :
		               pulled  ? pulledSource :
		                         source;
		a_out.name = utility ? kUtilitySourcePath : drawBuild ? Extensions::DrawFamilyOf(drawFamily).sourcePath.c_str() :
		             familyBuild                  ? Extensions::ProgramFamilyOf(family).sourcePath.c_str() :
		                                            kSourcePath;
		a_out.pulled = pulled;
		a_out.family = familyBuild;
		return true;
	}

	struct ShaderPrograms::StageCache
	{
		std::mutex mutex;
		std::mutex requestMutex;
		bool started = false;
		struct Pending { const RE::BSShader* shader; bool pixel; std::uint32_t descriptor; };
		std::vector<Pending> pending;
		ankerl::unordered_dense::map<std::uint64_t, StagePtr> stages;
		// The compiler we pointed at DCLF's build executor. A runtime restart replaces it.
		org::services::ShaderCompiler* configured = nullptr;
		// The lane's waiters by stage key.
		ankerl::unordered_dense::map<std::uint64_t, std::vector<StageWaiter>> waiting;
		std::vector<StageWaiter> ready;

		// a_created is set when this call made the request. Null when the stage's source is missing.
		// A stage belongs to one sources generation, so an earlier one is not found.
		StagePtr Request(ShaderPrograms& owner, const RE::BSShader& shader, bool pixel, std::uint32_t descriptor, bool depth = false, bool* a_created = nullptr,
			bool pulled = false)
		{
			const ShaderStageSpec spec{ pixel, descriptor, depth, pulled };
			bool effectivePulled = false;
			const std::uint64_t key = StageKeyOf(shader, spec, effectivePulled);
			const std::uint64_t generation = CurrentSourcesGeneration();
			{
				std::lock_guard lock(mutex);
				if (const auto found = stages.find(key); found != stages.end() && found->second->sourcesGeneration == generation)
					return found->second;
			}
			// Cache hits must not wait behind another permutation's filesystem key construction.
			std::lock_guard requestLock(requestMutex);
			const auto current = owner.SourcesLocked();
			{
				std::lock_guard lock(mutex);
				if (const auto found = stages.find(key); found != stages.end() && found->second->sourcesGeneration == current->generation)
					return found->second;
			}
			auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
			Sources::Resolved resolved;
			if (!compiler || !current->Resolve(shader, spec, resolved))
				return {};
			if (configured != compiler) {
				// Every DCLF request takes requestMutex, so none is submitted before this.
				compiler->Configure(BuildSubmitter());
				configured = compiler;
			}
			auto stage = std::make_shared<Stage>();
			stage->key = key;
			stage->sourcesGeneration = current->generation;
			RequestStage(*compiler, stage,
				MakeStageRequest(resolved.source, shader, pixel, descriptor, depth, resolved.pulled, resolved.family, resolved.name,
					stage->vertexAttributes));
			{
				std::lock_guard lock(mutex);
				stages.insert_or_assign(key, stage);
			}
			if (a_created)
				*a_created = true;
			return stage;
		}

		template <std::size_t N>
		std::array<StagePtr, N> RequestAll(ShaderPrograms& owner, const RE::BSShader& shader, const std::array<ShaderStageSpec, N>& a_stages, bool* a_created)
		{
			std::array<StagePtr, N> out;
			for (std::size_t i = 0; i < N; ++i)
				out[i] = Request(owner, shader, a_stages[i].pixel, a_stages[i].descriptor, a_stages[i].depth, a_created ? &a_created[i] : nullptr, a_stages[i].pulled);
			return out;
		}

		/**
		 * @brief The pipeline lane. Registers a new entry's stages. False when a stage could not be requested, failing the entry.
		 * A stage's done is set before its key is pushed, so a stage read as not done still has a completion to drain.
		 */
		template <std::size_t N>
		bool Await(StageUser a_user, std::uint64_t a_id, const std::array<StagePtr, N>& a_stages, std::uint8_t& a_remaining)
		{
			for (const auto& stage : a_stages)
				if (!stage)
					return false;
			for (const auto& stage : a_stages) {
				if (stage->done.load(std::memory_order_acquire))
					continue;
				waiting[stage->key].push_back({ a_user, a_id });
				++a_remaining;
			}
			if (!a_remaining)
				ready.push_back({ a_user, a_id });
			return true;
		}
	};

	ShaderPrograms::ShaderPrograms() : stages(std::make_unique<StageCache>()) {}
	ShaderPrograms::~ShaderPrograms() = default;

	std::shared_ptr<const ShaderPrograms::Sources> ShaderPrograms::SourcesLocked()
	{
		const std::uint64_t generation = CurrentSourcesGeneration();
		if (!sources || sources->generation != generation) {
			sources = std::make_shared<const Sources>(generation);
			sourcesMissing.store(sources->missing, std::memory_order_relaxed);
			// Old stages stay with the entries and builds that hold them, but no request finds them again.
			std::lock_guard lock(stages->mutex);
			stages->stages.clear();
		}
		return sources;
	}

	std::shared_ptr<const ShaderPrograms::Sources> ShaderPrograms::CurrentSources()
	{
		std::lock_guard lock(stages->requestMutex);
		return SourcesLocked();
	}

	void ShaderPrograms::Precompile(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
	{
		const auto type = a_shader.shaderType.get();
		if (type != RE::BSShader::Type::Lighting && type != RE::BSShader::Type::Utility)
			return;
		{
			std::lock_guard lock(stages->mutex);
			if (!stages->started) {
				stages->pending.push_back({ &a_shader, a_pixel, a_descriptor });
				return;
			}
		}
		DCLF_ZONE("CS.DCLF.Shaders.Precompile");

		auto stage = stages->Request(*this, a_shader, a_pixel, a_descriptor);
		const bool lighting = type == RE::BSShader::Type::Lighting;
		auto depth = a_pixel && lighting ? stages->Request(*this, a_shader, true, a_descriptor, true) : StagePtr{};
		auto pulled = lighting ? stages->Request(*this, a_shader, a_pixel, a_descriptor, a_pixel, nullptr, true) : StagePtr{};
		auto pulledColour = lighting && a_pixel ? stages->Request(*this, a_shader, true, a_descriptor, false, nullptr, true) : StagePtr{};
		auto pulledDepthVertex = lighting && !a_pixel ? stages->Request(*this, a_shader, false, a_descriptor, true, nullptr, true) : StagePtr{};
		// The shadow views' merged pipelines draw with a technique's canonical form.
		const std::uint32_t canonical = !lighting ? CanonicalShadowTechnique(a_descriptor) : a_descriptor;
		auto merged = canonical != a_descriptor ? stages->Request(*this, a_shader, a_pixel, canonical) : StagePtr{};
		// Hold CS's bounded compilation worker until this is done. The compiles run on DCLF's pool, which never waits on these.
		for (const auto* requested : { &stage, &depth, &pulled, &pulledColour, &pulledDepthVertex, &merged }) {
			if (!*requested) continue;
			(*requested)->done.wait(false, std::memory_order_acquire);
			const auto& artifact = *(*requested)->artifact;
			if (!artifact) {
				// A failed build is a shader bug: DCLF would quietly leave the draws to the engine.
				RenderGraph::Fatal("DCLF shaders", fmt::format("ORG precompile {} {} {:08X} failed: {}", type == RE::BSShader::Type::Lighting ? "Lighting" : "Utility",
						a_pixel ? "PS" : "VS", a_descriptor, artifact.diagnostics.substr(0, 1500)));
			}
		}
	}

	void ShaderPrograms::StartPrecompile()
	{
		std::vector<StageCache::Pending> pending;
		{
			std::lock_guard lock(stages->mutex);
			if (stages->started) return;
			stages->started = true;
			pending.swap(stages->pending);
		}
		auto remaining = std::make_shared<std::atomic<std::size_t>>(pending.size());
		for (const auto request : pending)
			SIE::ShaderCache::Instance().compilationPool.detach_task([this, request, remaining] {
				try { Precompile(*request.shader, request.pixel, request.descriptor); }
				catch (const std::exception& e) { logger::warn("[DCLF] ORG shader precompile failed: {}", e.what()); }

				// The last task saves the index and reports the batch's cost.
				if (remaining->fetch_sub(1) == 1)
					Util::FinishShaderStoreBatch("DCLF precompile");
				SIE::ShaderCache::Instance().NotifyPoolProgress();
			});
	}

	bool ShaderPrograms::FollowSources()
	{
		const std::uint64_t generation = CurrentSourcesGeneration();
		if (generation == entriesGeneration)
			return false;
		entriesGeneration = generation;
		// A stage still compiling completes into its own object. No entry waits for it.
		const std::size_t dropped = entries.size() + shadowEntries.size() + forwardEntries.size();
		stages->waiting.clear();
		stages->ready.clear();
		entries.clear();
		shadowEntries.clear();
		forwardEntries.clear();
		for (auto& family : familyEntries)
			family.clear();
		for (auto& entry : drawFamilyEntries)
			entry.reset();
		for (auto& entry : drawFamilyForwardEntries)
			entry.reset();
		loggedFailures = 0;
		logger::info("[DCLF] shader programs: sources generation {}; {} program entries of the files before dropped, made again as they are asked for", generation, dropped);
		return true;
	}

	template <class E, class MakeStages>
	E& ShaderPrograms::MakeEntry(std::unique_ptr<E>& a_slot, [[maybe_unused]] std::uint8_t a_user, [[maybe_unused]] std::uint64_t a_id, std::atomic<std::uint32_t>* a_requested,
		[[maybe_unused]] std::atomic<std::uint32_t>& a_failed, [[maybe_unused]] MakeStages&& a_makeStages)
	{
		a_slot = std::make_unique<E>();
		auto& entry = *a_slot;
		std::array<bool, std::tuple_size_v<decltype(entry.stages)>> created{};
		entry.stages = a_makeStages(created.data());
		if (!stages->Await(static_cast<StageUser>(a_user), a_id, entry.stages, entry.remaining)) {
			entry.failed = true;
			a_failed.fetch_add(1, std::memory_order_relaxed);
		}
		// After a reload every stage is requested here, so skip the on-demand warning.
		for (std::size_t i = 0; i < created.size() && !entriesGeneration; ++i)
			entry.onDemand |= created[i] ? static_cast<std::uint8_t>(1u << i) : std::uint8_t(0);
		entry.requested = std::chrono::steady_clock::now();
		if (a_requested)
			a_requested->fetch_add(1, std::memory_order_relaxed);
		return entry;
	}

	namespace
	{
		template <class E>
		auto Found(const E* a_entry, bool* a_failed) -> std::shared_ptr<const typename decltype(E::program)::element_type>
		{
			if (a_failed)
				*a_failed = a_entry && a_entry->failed;
			return a_entry ? a_entry->program : nullptr;
		}
	}

	std::shared_ptr<const ShaderPrograms::Program> ShaderPrograms::Find(const PipelineKey& a_key, RE::BSShader& a_lighting, std::uint8_t* a_onDemand, bool* a_failed)
	{
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled())
			return Found<Entry>(nullptr, a_failed);
		const std::uint64_t id = LightingProgramId(a_key);
		auto& slot = entries[id];
		if (!slot) {
			DumpStageLine(fmt::format(R"({{"kind":"lighting","vertex":"{:08X}","pixel":"{:08X}"}})", a_key.vertexDescriptor, a_key.pixelDescriptor));
			MakeEntry(slot, static_cast<std::uint8_t>(StageUser::Lighting), id, &laneStats.requested, laneStats.failed,
				[&](bool* a_created) { return stages->RequestAll(*this, a_lighting, LightingStages(a_key), a_created); });
			if (a_onDemand)
				*a_onDemand = slot->onDemand;
		}
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::Program> ShaderPrograms::FindFamily(Api::ProgramFamily a_family, const PipelineKey& a_key, const RE::BSShader& a_shader, bool* a_failed)
	{
		if (!Enabled())
			return Found<FamilyEntry>(nullptr, a_failed);
		const std::uint64_t id = FamilyProgramId(a_family, a_key);
		auto& slot = familyEntries[static_cast<std::size_t>(a_family)][id];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(FamilyUser(a_family)), id, &laneStats.requested, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_shader, FamilyStages(a_key), nullptr); });
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::ShadowProgram> ShaderPrograms::FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility, bool a_allowRequest, bool* a_requested,
		std::uint8_t* a_onDemand, bool* a_failed)
	{
		if (a_requested)
			*a_requested = false;
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled() || (!a_allowRequest && !shadowEntries.contains(a_technique)))
			return Found<ShadowEntry>(nullptr, a_failed);
		auto& slot = shadowEntries[a_technique];
		if (!slot) {
			MakeEntry(slot, static_cast<std::uint8_t>(StageUser::Shadow), a_technique, &laneStats.shadowRequested, laneStats.shadowFailed,
				[&](bool* a_created) { return stages->RequestAll(*this, a_utility, ShadowStages(a_technique), a_created); });
			if (a_requested)
				*a_requested = true;
			if (a_onDemand)
				*a_onDemand = slot->onDemand;
		}
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::DrawProgram> ShaderPrograms::FindDrawFamily(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed)
	{
		if (!Enabled())
			return Found<DrawFamilyEntry>(nullptr, a_failed);
		auto& slot = drawFamilyEntries[static_cast<std::size_t>(a_family)];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(DrawFamilyUser(a_family, false)), 0, nullptr, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_shader, DrawFamilyStages(a_family), nullptr); });
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::ForwardProgram> ShaderPrograms::FindForward(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor, RE::BSShader& a_lighting,
		bool* a_failed)
	{
		if (!Enabled())
			return Found<ForwardEntry>(nullptr, a_failed);
		const std::uint64_t id = ForwardProgramId(a_vertexDescriptor, a_pixelDescriptor);
		auto& slot = forwardEntries[id];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(StageUser::Forward), id, nullptr, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_lighting, ForwardStages(a_vertexDescriptor, a_pixelDescriptor), nullptr); });
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::ForwardProgram> ShaderPrograms::FindDrawFamilyForward(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed)
	{
		if (!Enabled())
			return Found<ForwardEntry>(nullptr, a_failed);
		auto& slot = drawFamilyForwardEntries[static_cast<std::size_t>(a_family)];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(DrawFamilyUser(a_family, true)), 0, nullptr, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_shader, DrawFamilyForwardStages(a_family), nullptr); });
		return Found(slot.get(), a_failed);
	}

	namespace
	{
		const org::services::ShaderArtifact& ArtifactOf(const StagePtr& a_stage) { return *a_stage->artifact; }

		// Report how an on-demand stage turned out: compiled, or found in the disk cache.
		void OnDemandOutcome(const char* a_what, std::uint8_t a_stages, std::chrono::steady_clock::time_point a_requested,
			std::initializer_list<std::pair<std::uint8_t, const org::services::ShaderArtifact*>> a_artifacts)
		{
			if (!a_stages)
				return;
			std::uint8_t compiled = 0, cached = 0, failed = 0;
			for (const auto& [bit, artifact] : a_artifacts) {
				if (!(a_stages & bit))
					continue;
				(!*artifact ? failed : artifact->fromCache ? cached : compiled) |= bit;
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_requested).count();
			logger::warn("[DCLF] on-demand SPIR-V {} done after {:.0f} ms: compiled now [{}], from the disk cache [{}], failed [{}]", a_what, ms,
				ShaderPrograms::OnDemandStages(compiled), ShaderPrograms::OnDemandStages(cached), ShaderPrograms::OnDemandStages(failed));
		}

		/**
		 * @brief Fans completed stages out to the entries waiting for them. a_arrive returns -1 if not done yet, 0 if counted, 1 if last.
		 * A stage from an earlier sources generation may complete after the cache moved on.
		 */
		template <class Arrive>
		void FanOut(ankerl::unordered_dense::map<std::uint64_t, std::vector<StageWaiter>>& a_waiting, std::vector<StageWaiter>& a_ready, Arrive&& a_arrive)
		{
			StageCompletions().Drain([&](std::uint64_t&& a_key) {
				const auto found = a_waiting.find(a_key);
				if (found == a_waiting.end())
					return;  // no entry waits
				std::erase_if(found->second, [&](const StageWaiter& a_waiter) {
					const int arrived = a_arrive(a_waiter, a_key);
					if (arrived == 1)
						a_ready.push_back(a_waiter);
					return arrived >= 0;
				});
				if (found->second.empty())
					a_waiting.erase(found);
			});
		}

		/** @brief A program's fingerprint and the oldest sources generation among its stages. */
		template <std::size_t N>
		std::pair<std::uint64_t, std::uint64_t> FingerprintOf(const std::array<StagePtr, N>& a_stages)
		{
			std::uint64_t fingerprint = 0xcbf29ce484222325ull, generation = ~0ull;
			for (const auto& stage : a_stages) {
				fingerprint = MixBinary(fingerprint, stage->artifact->binary);
				generation = std::min(generation, stage->sourcesGeneration);
			}
			return { fingerprint, generation };
		}
	}

	template <class E, class LogFailure>
	void ShaderPrograms::FinishEntry(E& a_entry, std::atomic<std::uint32_t>* a_ready, std::atomic<std::uint32_t>& a_failed, LogFailure&& a_logFailure)
	{
		std::uint32_t cached = 0;
		for (std::size_t i = 0; i < a_entry.stages.size(); ++i) {
			const auto& artifact = ArtifactOf(a_entry.stages[i]);
			if (!artifact) {
				a_entry.failed = true;
				a_failed.fetch_add(1, std::memory_order_relaxed);
				a_logFailure(i, artifact);
				return;
			}
			cached += artifact.fromCache ? 1u : 0u;
		}
		[&]<std::size_t... I>(std::index_sequence<I...>) {
			a_entry.program = std::make_shared<std::remove_reference_t<decltype(*a_entry.program)>>(
				std::remove_reference_t<decltype(*a_entry.program)>{ ArtifactOf(a_entry.stages[I]).binary... });
		}(std::make_index_sequence<std::tuple_size_v<decltype(a_entry.stages)>>{});
		std::tie(a_entry.program->fingerprint, a_entry.program->sourcesGeneration) = FingerprintOf(a_entry.stages);
		if (a_ready)
			a_ready->fetch_add(1, std::memory_order_relaxed);
		laneStats.fromCache.fetch_add(cached, std::memory_order_relaxed);
	}

	void ShaderPrograms::FinishDrawFamily(std::uint8_t a_user, Finished& a_finished)
	{
		const auto user = static_cast<StageUser>(a_user);
		const auto f = DrawFamilyIndexOf(user);
		const auto& family = Extensions::DrawFamilies()[f];
		if (IsDrawFamilyForward(user)) {
			FinishEntry(*drawFamilyForwardEntries[f], nullptr, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
				// A failed build is a shader bug: DCLF would quietly leave the draws to the engine.
				RenderGraph::Fatal("DCLF shaders", fmt::format("SPIR-V build of the forward {} {} failed:\n{}", family.name, a_stage ? "PS" : "VS", a_artifact.diagnostics.substr(0, 1500)));
			});
			a_finished.drawFamilyForward[f] = true;
			return;
		}
		auto& entry = *drawFamilyEntries[f];
		FinishEntry(entry, nullptr, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
			static constexpr const char* kStages[] = { "VS", "PS", "VS (depth)", "PS (depth)", "VS (colour 2)", "PS (colour 2)" };
			// A failed build is a shader bug: DCLF would quietly leave the draws to the engine.
			RenderGraph::Fatal("DCLF shaders", fmt::format("SPIR-V build of {} {} failed; {}:\n{}", family.name, kStages[a_stage], family.nativeNote, a_artifact.diagnostics.substr(0, 1500)));
		});
		if (entry.program) {
			KeepColourPrograms(static_cast<Api::DrawFamily>(f), *entry.program);
			logger::info("[DCLF] {} programs ready ({}, pulled)", family.label, family.name);
		}
		a_finished.drawFamily[f] = true;
	}

	void ShaderPrograms::UpdateLane(Finished& a_finished)
	{
		const auto visit = [&](const StageWaiter& a_waiter, auto&& a_visit) {
			switch (a_waiter.user) {
			case StageUser::Shadow: return a_visit(*shadowEntries.find(static_cast<std::uint32_t>(a_waiter.id))->second);
			case StageUser::Forward: return a_visit(*forwardEntries.find(a_waiter.id)->second);
			case StageUser::Lighting: return a_visit(*entries.find(a_waiter.id)->second);
			default: break;
			}
			if (IsDrawFamilyUser(a_waiter.user))
				return IsDrawFamilyForward(a_waiter.user) ? a_visit(*drawFamilyForwardEntries[DrawFamilyIndexOf(a_waiter.user)]) :
				                                            a_visit(*drawFamilyEntries[DrawFamilyIndexOf(a_waiter.user)]);
			return a_visit(*familyEntries[FamilyIndexOf(a_waiter.user)].find(a_waiter.id)->second);
		};
		FanOut(stages->waiting, stages->ready, [&](const StageWaiter& a_waiter, std::uint64_t a_key) {
			return visit(a_waiter, [&](auto& a_entry) -> int {
				const bool done = std::ranges::any_of(a_entry.stages, [&](const StagePtr& a_stage) {
					return a_stage->key == a_key && a_stage->done.load(std::memory_order_acquire);
				});
				if (!done)
					return -1;
				return --a_entry.remaining == 0 ? 1 : 0;
			});
		});
		for (const auto& waiter : std::exchange(stages->ready, {})) {
			const std::uint64_t id = waiter.id;
			const auto vertex = static_cast<std::uint32_t>(id >> 32), pixel = static_cast<std::uint32_t>(id);
			switch (waiter.user) {
			case StageUser::Shadow:
				{
					const auto technique = static_cast<std::uint32_t>(id);
					auto& entry = *shadowEntries.find(technique)->second;
					OnDemandOutcome(fmt::format("Utility technique {:08X}", technique).c_str(), entry.onDemand, entry.requested,
						{ { kOnDemandVertex, &ArtifactOf(entry.stages[0]) }, { kOnDemandPixel, &ArtifactOf(entry.stages[1]) } });
					FinishEntry(entry, &laneStats.shadowReady, laneStats.shadowFailed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
						// A failed build is a shader bug: DCLF would quietly leave the draws to the engine.
						RenderGraph::Fatal("DCLF shaders", fmt::format("SPIR-V build of Utility {} {:08X} failed:\n{}", a_stage ? "PS" : "VS", technique, a_artifact.diagnostics.substr(0, 1500)));
					});
					a_finished.shadow.push_back(technique);
				}
				continue;
			case StageUser::Forward:
				FinishEntry(*forwardEntries.find(id)->second, nullptr, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
					// A failed build is a shader bug: DCLF would quietly leave the draws to the engine.
					RenderGraph::Fatal("DCLF shaders", fmt::format("SPIR-V build of the forward {} {} failed:\n{}", fmt::format("Lighting VS {:08X} PS {:08X}", vertex, pixel), a_stage ? "PS" : "VS",
							a_artifact.diagnostics.substr(0, 1500)));
				});
				a_finished.forward.push_back(id);
				continue;
			case StageUser::Lighting:
				break;
			default:
				if (IsDrawFamilyUser(waiter.user)) {
					FinishDrawFamily(static_cast<std::uint8_t>(waiter.user), a_finished);
					continue;
				}
				{
					// The id's vertex part, without the family's program-id bit.
					const auto f = FamilyIndexOf(waiter.user);
					const auto& family = Extensions::ProgramFamilies()[f];
					const auto familyVertex = static_cast<std::uint32_t>((id & ~kProgramFamilySlots[f].programIdBit) >> 32);
					auto& entry = *familyEntries[f].find(id)->second;
					FinishEntry(entry, &laneStats.ready, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
						// A failed build is a shader bug: DCLF would quietly leave the draws to the engine.
						RenderGraph::Fatal("DCLF shaders", fmt::format("SPIR-V build of {} {}{} {:08X} failed; its {}:\n{}", family.name, a_stage == 0 || a_stage == 3 ? "VS" : "PS",
								a_stage >= 3 ? " (pulled)" : "", a_stage == 0 || a_stage == 3 ? familyVertex : pixel,
								family.nativeNote, a_artifact.diagnostics.substr(0, 1500)));
					});
					if (entry.program)
						entry.program->vertexAttributes = entry.stages[0]->vertexAttributes;
					a_finished.families[f].push_back(id);
				}
				continue;
			}
			auto& entry = *entries.find(id)->second;
			a_finished.lighting.push_back(id);
			OnDemandOutcome(fmt::format("Lighting VS {:08X} PS {:08X}", vertex, pixel).c_str(), entry.onDemand, entry.requested,
				{ { kOnDemandVertex, &ArtifactOf(entry.stages[0]) }, { kOnDemandPixel, &ArtifactOf(entry.stages[1]) }, { kOnDemandDepthPixel, &ArtifactOf(entry.stages[2]) },
					{ kOnDemandPulledVertex, &ArtifactOf(entry.stages[3]) }, { kOnDemandPulledDepthPixel, &ArtifactOf(entry.stages[4]) },
					{ kOnDemandPulledPixel, &ArtifactOf(entry.stages[5]) }, { kOnDemandPulledDepthVertex, &ArtifactOf(entry.stages[6]) } });
			FinishEntry(entry, &laneStats.ready, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
				static constexpr const char* kStages[] = { "VS", "PS", "PS (depth)", "VS (pulled)", "PS (pulled depth)", "PS (pulled)", "VS (pulled depth)" };
				// A failed build is a shader bug: DCLF would quietly leave the draws to the engine.
				RenderGraph::Fatal("DCLF shaders", fmt::format("SPIR-V build of Lighting {} {:08X} failed:\n{}", kStages[a_stage], a_stage == 0 || a_stage == 3 || a_stage == 6 ? vertex : pixel,
						a_artifact.diagnostics.substr(0, 1500)));
			});
			// DrawPipelines checks the key's vertex layout has these attributes.
			if (entry.program)
				entry.program->vertexAttributes = entry.stages[0]->vertexAttributes;
		}
	}

	template <class P, std::size_t N>
	void ShaderPrograms::CompileProgram(const RE::BSShader& a_shader, const std::array<ShaderStageSpec, N>& a_stages, Rebuilt<P> a_done)
	{
		auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
		const auto current = CurrentSources();
		if (!compiler || current->missing)
			return a_done(nullptr, compiler ? fmt::format("{} could not be read", kSourcePath) : std::string("no runtime shader compiler"));
		// Resolve every source first, so a missing one fails before anything compiles.
		std::array<Sources::Resolved, N> resolved;
		for (std::size_t i = 0; i < N; ++i)
			if (!current->Resolve(a_shader, a_stages[i], resolved[i]))
				return a_done(nullptr, fmt::format("the source of {} is missing", SourcePathOf(a_shader)));
		// Each completion writes its own stage. The last one makes the program.
		struct Compiles
		{
			std::array<std::shared_ptr<const org::services::ShaderArtifact>, N> artifacts;
			std::array<std::uint32_t, N> attributes{};
			std::atomic<std::uint32_t> remaining{ static_cast<std::uint32_t>(N) };
			std::uint64_t generation = 0;
			Rebuilt<P> done;
		};
		auto compiles = std::make_shared<Compiles>();
		compiles->generation = current->generation;
		compiles->done = std::move(a_done);
		std::array<org::services::ShaderCompileRequest, N> requests;
		for (std::size_t i = 0; i < N; ++i) {
			const auto& stage = a_stages[i];
			requests[i] = MakeStageRequest(resolved[i].source, a_shader, stage.pixel, stage.descriptor, stage.depth,
				resolved[i].pulled, resolved[i].family, resolved[i].name, compiles->attributes[i]);
		}
		for (std::size_t i = 0; i < N; ++i) {
			// A memory-cache hit completes on this thread.
			CompileStage(*compiler, std::move(requests[i]), [compiles, i](const org::services::ShaderArtifact& a_artifact) {
				compiles->artifacts[i] = std::make_shared<const org::services::ShaderArtifact>(a_artifact);
				if (compiles->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1)
					return;
				for (std::size_t s = 0; s < N; ++s)
					if (!*compiles->artifacts[s])
						return compiles->done(nullptr, fmt::format("its stage {} failed to compile:\n{}", s, compiles->artifacts[s]->diagnostics.substr(0, 1500)));
				auto program = [&]<std::size_t... I>(std::index_sequence<I...>) {
					return std::make_shared<P>(P{ compiles->artifacts[I]->binary... });
				}(std::make_index_sequence<N>{});
				std::uint64_t fingerprint = 0xcbf29ce484222325ull;
				for (const auto& artifact : compiles->artifacts)
					fingerprint = MixBinary(fingerprint, artifact->binary);
				program->fingerprint = fingerprint;
				program->sourcesGeneration = compiles->generation;
				if constexpr (std::is_same_v<P, Program>)
					program->vertexAttributes = compiles->attributes[0];
				compiles->done(std::move(program), {});
			});
		}
	}

	void ShaderPrograms::RebuildLighting(const PipelineKey& a_key, const RE::BSShader& a_lighting, Rebuilt<Program> a_done)
	{
		CompileProgram<Program>(a_lighting, LightingStages(a_key), std::move(a_done));
	}

	void ShaderPrograms::RebuildFamily(Api::ProgramFamily, const PipelineKey& a_key, const RE::BSShader& a_shader, Rebuilt<Program> a_done)
	{
		CompileProgram<Program>(a_shader, FamilyStages(a_key), std::move(a_done));
	}

	void ShaderPrograms::RebuildShadow(std::uint32_t a_technique, const RE::BSShader& a_utility, Rebuilt<ShadowProgram> a_done)
	{
		CompileProgram<ShadowProgram>(a_utility, ShadowStages(a_technique), std::move(a_done));
	}

	void ShaderPrograms::RebuildDrawFamily(Api::DrawFamily a_family, const RE::BSShader& a_shader, Rebuilt<DrawProgram> a_done)
	{
		CompileProgram<DrawProgram>(a_shader, DrawFamilyStages(a_family), [a_family, done = std::move(a_done)](std::shared_ptr<const DrawProgram> a_program, std::string a_error) {
			// Made for this rebuild alone, so nothing shares it yet.
			if (a_program)
				KeepColourPrograms(a_family, *std::const_pointer_cast<DrawProgram>(a_program));
			done(std::move(a_program), std::move(a_error));
		});
	}

	void ShaderPrograms::RebuildForward(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor, const RE::BSShader& a_lighting, Rebuilt<ForwardProgram> a_done)
	{
		CompileProgram<ForwardProgram>(a_lighting, ForwardStages(a_vertexDescriptor, a_pixelDescriptor), std::move(a_done));
	}

	void ShaderPrograms::RebuildDrawFamilyForward(Api::DrawFamily a_family, const RE::BSShader& a_shader, Rebuilt<ForwardProgram> a_done)
	{
		CompileProgram<ForwardProgram>(a_shader, DrawFamilyForwardStages(a_family), std::move(a_done));
	}
}
