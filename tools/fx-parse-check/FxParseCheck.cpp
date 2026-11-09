// fx-parse-check: runs the ORG FX runtime's parser (src/Features/Effects11/ORG) over ENB-style presets and reports what it found.
//
// For each .fx file it does what Effects11 does before compiling (docs/development/org-fx-runtime.md, section 1.2): stringify
// macros, D3DPreprocess with includes from the preset folder (a missing include reads as empty), stringify expansion and #line
// stripping. ShaderPatches.json and ENB Extender's #pragma uidefine are not applied. Then OrgFx::Parse, and a report per file.
//
// It also checks the parser's $Globals layout against FXC: it compiles the first pixel entry with ps_5_0 and compares every
// global's offset and size with the shader's reflection. A mismatch counts as an error.
//
// With --dxc it also lowers every pass (FxLower.h) and compiles both wrappers with that dxc.exe, with the flags the runtime uses
// (BasicRHI's SPIR-V ABI, -HV 2018). Sources go to --out (default: a folder in %TEMP%), so a failure can be read.
//
// Usage: fx-parse-check [-v] [--dxc <dxc.exe>] [--out <folder>] <preset folder or .fx file>...
//   A folder is scanned for *.fx (not recursively). -v lists every global, technique and pass.
//   Exit code 0 when every readable file parsed (and, with --dxc, lowered and compiled) without errors, 1 otherwise.
//
// Build (x64 Native Tools prompt, from the repository root):
//   cl /nologo /std:c++latest /EHsc /W4 /WX /permissive- /utf-8 /Isrc tools\fx-parse-check\FxParseCheck.cpp
//      src\Features\Effects11\ORG\FxParser.cpp src\Features\Effects11\ORG\FxLower.cpp src\Features\Effects11\ORG\FxSourceText.cpp
//      d3dcompiler.lib dxguid.lib
//      /Fe:build\fx-parse-check.exe /Fo:build\

#include <Windows.h>

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include "Features/Effects11/ORG/FxLower.h"
#include "Features/Effects11/ORG/FxParser.h"
#include "Features/Effects11/ORG/FxSourceText.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
	std::string ReadFile(const std::filesystem::path& a_path, bool& a_ok)
	{
		std::ifstream file(a_path, std::ios::binary);
		a_ok = file.is_open();
		return a_ok ? std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()) : std::string{};
	}

	/** @brief Includes resolved inside the preset folder, as Effects11's PresetInclude does. Stringify defines are collected. */
	class Include final : public ID3DInclude
	{
	public:
		explicit Include(std::filesystem::path a_base) :
			base(std::move(a_base)) {}

		HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR a_name, LPCVOID, LPCVOID* a_data, UINT* a_bytes) override
		{
			std::string_view name(a_name);
			while (!name.empty() && (name.front() == '/' || name.front() == '\\'))
				name.remove_prefix(1);
			bool ok = false;
			auto text = ReadFile(base / name, ok);
			if (!ok) {
				missing.push_back(std::string(name));
				text = "\n";
			} else {
				opened.push_back(std::string(name));
				OrgFx::SourceText::StripStringifyDefines(text, stringifyMacros);
			}
			auto* buffer = new char[text.size()];
			std::copy(text.begin(), text.end(), buffer);
			*a_data = buffer;
			*a_bytes = static_cast<UINT>(text.size());
			return S_OK;
		}

		HRESULT __stdcall Close(LPCVOID a_data) override
		{
			delete[] static_cast<const char*>(a_data);
			return S_OK;
		}

		std::filesystem::path base;
		std::vector<std::string> stringifyMacros;
		std::vector<std::string> opened;
		std::vector<std::string> missing;
	};

	const char* SeverityName(OrgFx::Severity a_severity)
	{
		switch (a_severity) {
		case OrgFx::Severity::Info:
			return "info";
		case OrgFx::Severity::Warning:
			return "warning";
		default:
			return "error";
		}
	}

	std::string Join(const std::vector<double>& a_values)
	{
		std::string out;
		for (std::size_t i = 0; i < a_values.size(); ++i)
			out += std::format("{}{:g}", i ? ", " : "", a_values[i]);
		return out;
	}

	const char* ClassName(OrgFx::TypeClass a_cls)
	{
		using enum OrgFx::TypeClass;
		switch (a_cls) {
		case Void:
			return "void";
		case Numeric:
			return "numeric";
		case Struct:
			return "struct";
		case String:
			return "string";
		case Texture:
			return "texture";
		case RWResource:
			return "uav";
		case Sampler:
			return "sampler";
		case Shader:
			return "shader";
		case StateObject:
			return "state";
		default:
			return "unknown";
		}
	}

	/**
	 * @brief Compiles pixel entries with ps_5_0 until one has $Globals (FXC drops it from a shader that reads none), and
	 * compares it with the parser's layout.
	 * @return The number of mismatches; prints why the check was skipped when it could not run.
	 */
	std::size_t CheckLayout(const OrgFx::EffectDesc& a_fx, const std::string& a_text, const std::string& a_name)
	{
		std::vector<std::string> entries;
		for (const auto& t : a_fx.techniques)
			for (const auto& p : t.passes) {
				const auto& b = p.shaders[static_cast<std::size_t>(OrgFx::Stage::Pixel)];
				if (b.function >= 0 && b.arguments.empty() && std::find(entries.begin(), entries.end(), b.entry) == entries.end())
					entries.push_back(b.entry);
			}
		if (entries.empty()) {
			std::printf("   layout: no pixel entry without uniform arguments to compile; not checked\n");
			return 0;
		}
		ID3DBlob* code = nullptr;
		ID3D11ShaderReflection* reflection = nullptr;
		ID3D11ShaderReflectionConstantBuffer* globals = nullptr;
		D3D11_SHADER_BUFFER_DESC bufferDesc{};
		std::string entry;
		for (const auto& candidate : entries) {
			ID3DBlob* errors = nullptr;
			const HRESULT hr = D3DCompile(a_text.data(), a_text.size(), a_name.c_str(), nullptr, nullptr, candidate.c_str(), "ps_5_0", 0, 0, &code, &errors);
			if (FAILED(hr) || !code) {
				std::string message = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "(no message)";
				message = message.substr(0, message.find('\n'));
				std::printf("   layout: ps_5_0 compile of '%s' failed: %s\n", candidate.c_str(), message.c_str());
				if (errors)
					errors->Release();
				code = nullptr;
				continue;
			}
			if (errors)
				errors->Release();
			if (SUCCEEDED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&reflection)))) {
				globals = reflection->GetConstantBufferByName("$Globals");
				if (globals && SUCCEEDED(globals->GetDesc(&bufferDesc))) {
					entry = candidate;
					break;
				}
				reflection->Release();
				reflection = nullptr;
			}
			code->Release();
			code = nullptr;
		}
		if (entry.empty()) {
			std::printf("   layout: no compiled pixel entry reads $Globals; not checked\n");
			return 0;
		}
		std::size_t mismatches = 0, compared = 0, defaultsCompared = 0;
		{
			if (bufferDesc.Size != a_fx.globalsSize) {
				std::printf("   layout: $Globals is %u bytes in FXC, %u in the parser\n", bufferDesc.Size, a_fx.globalsSize);
				++mismatches;
			}
			const auto defaults = OrgFx::BuildDefaultValues(a_fx);
			for (UINT i = 0; i < bufferDesc.Variables; ++i) {
				D3D11_SHADER_VARIABLE_DESC varDesc{};
				if (FAILED(globals->GetVariableByIndex(i)->GetDesc(&varDesc)))
					continue;
				const auto* v = a_fx.FindVariable(varDesc.Name);
				++compared;
				if (!v || v->bufferOffset != static_cast<std::int32_t>(varDesc.StartOffset) || v->bufferSize != varDesc.Size) {
					std::printf("   layout: '%s' is @%u+%u in FXC, @%d+%u in the parser\n", varDesc.Name, varDesc.StartOffset, varDesc.Size, v ? v->bufferOffset : -1, v ? v->bufferSize : 0u);
					++mismatches;
					continue;
				}
				if (!varDesc.DefaultValue || varDesc.StartOffset + varDesc.Size > defaults.size())
					continue;
				++defaultsCompared;
				if (std::memcmp(defaults.data() + varDesc.StartOffset, varDesc.DefaultValue, varDesc.Size) != 0) {
					auto words = [&](const void* a_bytes) {
						std::string out;
						for (UINT w = 0; w < varDesc.Size / 4; ++w) {
							std::uint32_t word = 0;
							std::memcpy(&word, static_cast<const std::uint8_t*>(a_bytes) + w * 4, 4);
							out += std::format(" {:08X}", word);
						}
						return out;
					};
					std::printf("   defaults: '%s' ('%s')\n     FXC:   %s\n     parser:%s\n", varDesc.Name, v->initializerText.c_str(), words(varDesc.DefaultValue).c_str(),
						words(defaults.data() + varDesc.StartOffset).c_str());
					++mismatches;
				}
			}
		}
		std::printf("   layout: %zu $Globals variable(s) and %zu default(s) compared with FXC (ps_5_0 '%s'), %zu mismatch(es)\n", compared, defaultsCompared, entry.c_str(), mismatches);
		reflection->Release();
		code->Release();
		return mismatches;
	}

	struct Totals
	{
		int files = 0;
		int encrypted = 0;
		int clean = 0;
		int failed = 0;
		std::size_t techniques = 0;
		std::size_t passes = 0;
		std::size_t resolvedStages = 0;
		std::size_t stages = 0;
		std::size_t uiVariables = 0;
		std::size_t errors = 0;
		std::size_t warnings = 0;
		std::size_t layoutMismatches = 0;
		std::size_t lowered = 0;
		std::size_t compiled = 0;
		std::size_t compileFailures = 0;
	};

	struct Options
	{
		bool verbose = false;
		std::filesystem::path dxc;
		std::filesystem::path out;
	};

	/** @brief Runs a command line; its exit code, with its output in a_output. */
	int Run(const std::string& a_command, std::string& a_output)
	{
		a_output.clear();
		FILE* pipe = _popen(("\"" + a_command + " 2>&1\"").c_str(), "r");
		if (!pipe)
			return -1;
		char buffer[512];
		while (std::fgets(buffer, sizeof(buffer), pipe))
			a_output += buffer;
		return _pclose(pipe);
	}

	/** @brief Lowers every pass and compiles its wrappers with DXC. @return Whether every pass compiled. */
	bool LowerAndCompile(const OrgFx::EffectDesc& a_fx, const std::string& a_text, const std::filesystem::path& a_file, const Options& a_options, Totals& a_totals)
	{
		const auto effect = OrgFx::LowerEffect(a_text, a_fx);
		for (const auto& d : effect.diagnostics)
			std::printf("   lower: %s line %u: %s\n", SeverityName(d.severity), d.line, d.message.c_str());
		if (!effect.ok)
			return false;
		const auto folder = a_options.out / a_file.parent_path().filename() / a_file.stem();
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		std::map<std::size_t, bool> done;  // by source hash: passes that share a source compile once
		std::size_t passes = 0, failures = 0;
		for (std::uint32_t t = 0; t < a_fx.techniques.size(); ++t)
			for (std::uint32_t p = 0; p < a_fx.techniques[t].passes.size(); ++p) {
				++passes;
				const auto& technique = a_fx.techniques[t];
				const auto pass = OrgFx::LowerPass(effect, a_fx, t, p);
				for (const auto& d : pass.diagnostics)
					if (a_options.verbose || d.severity != OrgFx::Severity::Info)
						std::printf("   lower %s/%u: %s line %u: %s\n", technique.name.c_str(), p, SeverityName(d.severity), d.line, d.message.c_str());
				if (!pass.ok) {
					++failures;
					continue;
				}
				++a_totals.lowered;
				const auto hash = std::hash<std::string>{}(pass.source);
				if (auto it = done.find(hash); it != done.end()) {
					failures += it->second ? 0 : 1;
					continue;
				}
				const auto source = folder / std::format("{}_{}.hlsl", technique.name.empty() ? std::format("technique{}", t) : technique.name, p);
				std::ofstream(source, std::ios::binary) << pass.source;
				bool ok = true;
				for (const auto& [target, entry] : { std::pair{ "vs_6_6", OrgFx::kVertexEntry }, std::pair{ "ps_6_6", OrgFx::kPixelEntry } }) {
					const auto spv = source.string() + "." + target + ".spv";
					const auto command = std::format(
						"\"{}\" -spirv -fvk-use-dx-layout -fvk-use-dx-position-w -fspv-target-env=vulkan1.3 -fvk-bind-resource-heap 1000000 0 "
						"-fvk-bind-sampler-heap 1000001 0 -fvk-bind-counter-heap 1000002 0 -fspv-use-unknown-image-format -HV 2018 "
						"-DBASICRHI_SHADER_API_VULKAN=1 -DCS_ORG_BINDLESS=1 -T {} -E {} -Fo \"{}\" \"{}\"",
						a_options.dxc.string(), target, entry, spv, source.string());
					std::string output;
					if (Run(command, output) != 0) {
						ok = false;
						std::string first;
						for (std::size_t at = 0; at < output.size();) {
							const auto end = output.find('\n', at);
							const auto line = output.substr(at, end == std::string::npos ? std::string::npos : end - at);
							if (line.find("error") != std::string::npos) {
								first = line;
								break;
							}
							if (end == std::string::npos)
								break;
							at = end + 1;
						}
						std::printf("   dxc %s %s/%u failed: %s\n     source: %s\n", target, technique.name.c_str(), p, first.empty() ? output.substr(0, 300).c_str() : first.c_str(),
							source.string().c_str());
					} else
						++a_totals.compiled;
				}
				done[hash] = ok;
				failures += ok ? 0 : 1;
				a_totals.compileFailures += ok ? 0 : 1;
			}
		std::printf("   lowered and compiled: %zu of %zu pass(es), %zu unique source(s), %zu resource slot(s), %u value byte(s) per pass\n", passes - failures, passes, done.size(),
			effect.resources.size(), effect.RegionSize());
		return failures == 0;
	}

	void Report(const std::filesystem::path& a_file, const Options& a_options, Totals& a_totals)
	{
		const bool a_verbose = a_options.verbose;
		++a_totals.files;
		bool ok = false;
		auto source = ReadFile(a_file, ok);
		std::printf("== %s\n", a_file.string().c_str());
		if (!ok) {
			std::printf("   could not read the file\n");
			++a_totals.failed;
			return;
		}
		if (OrgFx::SourceText::IsEncrypted(source)) {
			std::printf("   encrypted (KIEFX): no source, stays on FX11\n");
			++a_totals.encrypted;
			return;
		}
		if (source.find("uidefine") != std::string::npos)
			std::printf("   note: #pragma uidefine is not applied by this tool\n");

		std::vector<std::string> macros;
		OrgFx::SourceText::StripStringifyDefines(source, macros);
		Include include(a_file.parent_path());
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		const auto name = a_file.string();
		const HRESULT hr = D3DPreprocess(source.data(), source.size(), name.c_str(), nullptr, &include, &blob, &errors);
		if (FAILED(hr) || !blob) {
			std::printf("   D3DPreprocess failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
			if (errors)
				errors->Release();
			++a_totals.failed;
			return;
		}
		std::string text(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
		blob->Release();
		if (errors)
			errors->Release();
		while (!text.empty() && text.back() == '\0')
			text.pop_back();
		macros.insert(macros.end(), include.stringifyMacros.begin(), include.stringifyMacros.end());
		OrgFx::SourceText::ExpandStringifyMacros(text, macros);
		OrgFx::SourceText::StripLineDirectives(text);
		for (const auto& m : include.missing)
			std::printf("   note: include '%s' not found (read as empty)\n", m.c_str());

		const auto fx = OrgFx::Parse(text);
		const auto layoutMismatches = CheckLayout(fx, text, name);
		a_totals.layoutMismatches += layoutMismatches;

		std::size_t passes = 0, stages = 0, resolved = 0;
		for (const auto& t : fx.techniques)
			for (const auto& p : t.passes) {
				++passes;
				for (const auto& b : p.shaders)
					if (b.set && !b.null) {
						++stages;
						resolved += b.function >= 0 ? 1 : 0;
					}
			}
		std::size_t numeric = 0, ui = 0, textures = 0, files = 0, strings = 0, samplers = 0, externs = 0;
		for (const auto& v : fx.variables) {
			const bool hasUi = !OrgFx::AnnotationText(v.annotations, "UIName").empty();
			if (v.bufferOffset >= 0)
				++numeric;
			if (hasUi && (v.type.cls == OrgFx::TypeClass::Numeric || v.type.cls == OrgFx::TypeClass::String))
				++ui;
			if (v.type.cls == OrgFx::TypeClass::Texture) {
				++textures;
				files += OrgFx::FindAnnotation(v.annotations, "ResourceName") ? 1 : 0;
			}
			strings += v.type.cls == OrgFx::TypeClass::String ? 1 : 0;
			samplers += v.type.cls == OrgFx::TypeClass::Sampler ? 1 : 0;
			externs += OrgFx::FindAnnotation(v.annotations, "ExternBinding") ? 1 : 0;
		}
		for (const auto& s : fx.states)
			samplers += (s.kind == OrgFx::StateKind::Sampler || s.kind == OrgFx::StateKind::SamplerComparison) ? 1 : 0;

		const auto errorCount = fx.Count(OrgFx::Severity::Error);
		const auto warningCount = fx.Count(OrgFx::Severity::Warning);
		std::printf("   %zu bytes preprocessed, %zu include(s); %zu error(s), %zu warning(s)\n", text.size(), include.opened.size(), errorCount, warningCount);
		std::printf("   techniques %zu in %zu chain(s), passes %zu, shader stages %zu (%zu resolved)\n", fx.techniques.size(), fx.chains.size(), passes, stages, resolved);
		std::printf("   functions %zu, structs %zu, $Globals %u bytes (%zu values), UI variables %zu, strings %zu, textures %zu (%zu from files), samplers %zu, extern bindings %zu, other states %zu\n",
			fx.functions.size(), fx.structs.size(), fx.globalsSize, numeric, ui, strings, textures, files, samplers, externs,
			static_cast<std::size_t>(std::count_if(fx.states.begin(), fx.states.end(), [](const OrgFx::StateObject& s) {
				return s.kind != OrgFx::StateKind::Sampler && s.kind != OrgFx::StateKind::SamplerComparison;
			})));
		for (const auto& c : fx.chains) {
			std::string list;
			for (auto i : c.techniques) {
				const auto& t = fx.techniques[i];
				const auto target = OrgFx::AnnotationText(t.annotations, "RenderTarget");
				list += std::format(" {}{}", t.name, target.empty() ? "" : "->" + target);
			}
			const auto* first = c.techniques.empty() ? nullptr : &fx.techniques[c.techniques.front()];
			const auto uiName = first ? OrgFx::AnnotationText(first->annotations, "UIName") : std::string{};
			std::printf("   chain %s%s:%s\n", c.key.c_str(), uiName.empty() ? "" : std::format(" \"{}\"", uiName).c_str(), list.c_str());
		}
		for (const auto& d : fx.diagnostics)
			if (a_verbose || d.severity != OrgFx::Severity::Info)
				std::printf("   %s line %u: %s\n", SeverityName(d.severity), d.line, d.message.c_str());

		if (a_verbose) {
			for (const auto& t : fx.techniques)
				for (const auto& p : t.passes) {
					std::printf("   pass %s/%s:", t.name.c_str(), p.name.c_str());
					for (std::size_t s = 0; s < p.shaders.size(); ++s) {
						const auto& b = p.shaders[s];
						if (!b.set)
							continue;
						if (b.null) {
							std::printf(" %s=NULL", OrgFx::StageName(static_cast<OrgFx::Stage>(s)));
							continue;
						}
						std::string args;
						for (const auto& a : b.arguments)
							args += std::format("{}{}", args.empty() ? "" : ", ", a.text);
						std::printf(" %s=%s %s(%s)%s", OrgFx::StageName(static_cast<OrgFx::Stage>(s)), b.profile.c_str(), b.entry.c_str(), args.c_str(), b.function >= 0 ? "" : " [unresolved]");
					}
					std::printf("\n");
				}
			for (const auto& v : fx.variables) {
				std::printf("   var %s %s%s %s", ClassName(v.type.cls), v.type.name.c_str(), v.arraySize ? std::format("[{}]", v.arraySize).c_str() : "", v.name.c_str());
				if (v.bufferOffset >= 0)
					std::printf(" @%d+%u", v.bufferOffset, v.bufferSize);
				if (v.initializerEvaluated)
					std::printf(" = {%s}", Join(v.initializer).c_str());
				else if (!v.initializerText.empty())
					std::printf(" = '%s'", v.initializerText.c_str());
				for (const auto& a : v.annotations)
					std::printf(" <%s=%s>", a.name.c_str(), a.text.c_str());
				std::printf("\n");
			}
			for (const auto& s : fx.states) {
				const auto& d = s.sampler;
				std::printf("   state %s kind=%d filter=%d/%d/%d aniso=%d address=%d/%d/%d keys=%zu\n", s.name.c_str(), static_cast<int>(s.kind), static_cast<int>(d.minFilter),
					static_cast<int>(d.magFilter), static_cast<int>(d.mipFilter), d.anisotropic ? 1 : 0, static_cast<int>(d.addressU), static_cast<int>(d.addressV),
					static_cast<int>(d.addressW), s.assignments.size());
			}
		}

		const bool compiled = a_options.dxc.empty() || errorCount != 0 || LowerAndCompile(fx, text, a_file, a_options, a_totals);

		a_totals.techniques += fx.techniques.size();
		a_totals.passes += passes;
		a_totals.stages += stages;
		a_totals.resolvedStages += resolved;
		a_totals.uiVariables += ui;
		a_totals.errors += errorCount;
		a_totals.warnings += warningCount;
		if (errorCount == 0 && layoutMismatches == 0 && compiled)
			++a_totals.clean;
		else
			++a_totals.failed;
	}
}

int main(int a_argc, char** a_argv)
{
	Options options;
	std::vector<std::filesystem::path> files;
	for (int i = 1; i < a_argc; ++i) {
		const std::string arg = a_argv[i];
		if (arg == "-v") {
			options.verbose = true;
			continue;
		}
		if ((arg == "--dxc" || arg == "--out") && i + 1 < a_argc) {
			(arg == "--dxc" ? options.dxc : options.out) = a_argv[++i];
			continue;
		}
		const std::filesystem::path path(arg);
		std::error_code ec;
		if (std::filesystem::is_directory(path, ec)) {
			std::vector<std::filesystem::path> found;
			for (const auto& entry : std::filesystem::directory_iterator(path, ec))
				if (entry.is_regular_file() && entry.path().extension() == ".fx")
					found.push_back(entry.path());
			std::sort(found.begin(), found.end());
			files.insert(files.end(), found.begin(), found.end());
		} else
			files.push_back(path);
	}
	if (files.empty()) {
		std::printf("usage: fx-parse-check [-v] [--dxc <dxc.exe>] [--out <folder>] <preset folder or .fx file>...\n");
		return 2;
	}
	if (!options.dxc.empty() && options.out.empty())
		options.out = std::filesystem::temp_directory_path() / "fx-parse-check";
	Totals totals;
	for (const auto& file : files)
		Report(file, options, totals);
	std::printf("\n%d file(s): %d parsed without errors, %d with errors or unreadable, %d encrypted. %zu techniques, %zu passes, %zu/%zu shader stages resolved, %zu UI variables, %zu errors, %zu warnings, %zu layout mismatches.\n",
		totals.files, totals.clean, totals.failed, totals.encrypted, totals.techniques, totals.passes, totals.resolvedStages, totals.stages, totals.uiVariables, totals.errors, totals.warnings,
		totals.layoutMismatches);
	if (!options.dxc.empty())
		std::printf("Lowering: %zu pass(es) lowered, %zu stage(s) compiled by DXC, %zu unique source(s) failed. Sources in %s\n", totals.lowered, totals.compiled, totals.compileFailures,
			options.out.string().c_str());
	return totals.failed == 0 ? 0 : 1;
}
