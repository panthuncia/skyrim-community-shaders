// DCLF's D3D12 pulled builds, prepared through the dxcompiler.dll in the process.
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DxilStages.h"

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/FrameSource.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"

#include <ORGModuleServices/ShaderCompiler.h>

#include <dxcapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <vector>

namespace DCLF::DxilStages
{
	namespace
	{
		// Spelled out so no GUID definition is needed.
		constexpr GUID kClsidDxcCompiler = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
		constexpr GUID kClsidDxcUtils = { 0x6245d6af, 0x66e0, 0x48fd, { 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c } };

		// The loaded dxcompiler.dll's DxcCreateInstance, or null.
		DxcCreateInstanceProc CreateInstance()
		{
			const HMODULE dxc = ::GetModuleHandleW(L"dxcompiler.dll");
			return dxc ? reinterpret_cast<DxcCreateInstanceProc>(::GetProcAddress(dxc, "DxcCreateInstance")) : nullptr;
		}

		// One utils and one compiler per thread, because DXC's objects are not shared between threads.
		IDxcUtils* Utils()
		{
			thread_local winrt::com_ptr<IDxcUtils> utils;
			if (!utils)
				if (const auto create = CreateInstance())
					(void)create(kClsidDxcUtils, __uuidof(IDxcUtils), utils.put_void());
			return utils.get();
		}

		IDxcCompiler3* Compiler()
		{
			thread_local winrt::com_ptr<IDxcCompiler3> compiler;
			if (!compiler)
				if (const auto create = CreateInstance())
					(void)create(kClsidDxcCompiler, __uuidof(IDxcCompiler3), compiler.put_void());
			return compiler.get();
		}
	}

	bool NeedsFrameSource(const org::services::ShaderCompileRequest& a_request)
	{
		return std::ranges::any_of(a_request.defines, [](const org::services::ShaderDefine& a_define) { return a_define.name == kFrameDefine; });
	}

	bool PrepareFrameSource(org::services::ShaderCompileRequest& a_request, std::string& a_storage, std::string& a_error)
	{
		auto* utils = Utils();
		auto* compiler = Compiler();
		if (!utils || !compiler) {
			a_error = "no dxcompiler.dll in the process to preprocess the D3D12 build";
			return false;
		}
		std::string source = "#include \"Common/DCLFFrameRecord.hlsli\"\n";
		source.append(reinterpret_cast<const char*>(a_request.source.data()), a_request.source.size());
		std::vector<std::wstring> arguments = { L"-P", L"-E", a_request.entryPoint, L"-T", a_request.target, L"-HV",
			a_request.languageVersion.empty() ? std::wstring(L"2021") : a_request.languageVersion };
		for (const auto& define : a_request.defines) {
			arguments.emplace_back(L"-D");
			arguments.push_back(define.value.empty() ? define.name : define.name + L"=" + define.value);
		}
		for (const auto& directory : a_request.includeDirectories) {
			arguments.emplace_back(L"-I");
			arguments.push_back(directory.wstring());
		}
		arguments.push_back(std::wstring(a_request.sourceName.begin(), a_request.sourceName.end()));
		std::vector<const wchar_t*> raw;
		raw.reserve(arguments.size());
		for (const auto& argument : arguments)
			raw.push_back(argument.c_str());
		winrt::com_ptr<IDxcIncludeHandler> includes;
		if (FAILED(utils->CreateDefaultIncludeHandler(includes.put()))) {
			a_error = "DXC has no default include handler";
			return false;
		}
		const DxcBuffer buffer{ source.data(), source.size(), DXC_CP_UTF8 };
		winrt::com_ptr<IDxcResult> result;
		if (FAILED(compiler->Compile(&buffer, raw.data(), static_cast<UINT32>(raw.size()), includes.get(), __uuidof(IDxcResult), result.put_void())) || !result) {
			a_error = "DXC could not run the D3D12 build's preprocess";
			return false;
		}
		HRESULT status = S_OK;
		result->GetStatus(&status);
		winrt::com_ptr<IDxcBlobUtf8> text;
		if (FAILED(status) || FAILED(result->GetOutput(DXC_OUT_HLSL, __uuidof(IDxcBlobUtf8), text.put_void(), nullptr)) || !text || !text->GetStringLength()) {
			winrt::com_ptr<IDxcBlobUtf8> errors;
			(void)result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), errors.put_void(), nullptr);
			a_error = "the D3D12 build's preprocess failed: " + (errors ? std::string(errors->GetStringPointer(), errors->GetStringLength()) : std::string("no diagnostics"));
			return false;
		}
		FrameSource::Options options;
		options.pixel = a_request.target.starts_with(L"ps");
		options.vertexBlocks = kFramePushVS;
		options.pixelBlocks = kFramePushPS;
		options.keepFirst = kDrawPushBinding;
		options.keepLast = kFramePushBinding;
		options.shadow = std::ranges::any_of(a_request.defines, [](const org::services::ShaderDefine& a_define) { return a_define.name == kShadowFrameDefine; });
		auto rewritten = FrameSource::Rewrite(std::string_view(text->GetStringPointer(), text->GetStringLength()), options);
		if (!rewritten.error.empty()) {
			a_error = "the D3D12 build's frame rewrite failed: " + rewritten.error;
			return false;
		}
		a_storage = std::move(rewritten.source);
		// CS_D3D12_SOURCE_DUMP=<dir> dumps the first rewritten sources to inspect offline.
		static const std::string dumpDirectory = [] {
			char value[MAX_PATH]{};
			const DWORD length = ::GetEnvironmentVariableA("CS_D3D12_SOURCE_DUMP", value, sizeof(value));
			return length && length < sizeof(value) ? std::string(value) : std::string{};
		}();
		if (!dumpDirectory.empty()) {
			static std::atomic<std::uint32_t> dumped = 0;
			const auto index = dumped.fetch_add(1);
			if (index < 24) {
				std::error_code error;
				std::filesystem::create_directories(dumpDirectory, error);
				std::ofstream(std::filesystem::path(dumpDirectory) / std::format("{:02}_{}.hlsl", index, options.pixel ? "ps" : "vs"), std::ios::binary) << a_storage;
			}
		}
		// Includes and defines are resolved, so the compile reads this text alone. Its #line directives name the files.
		a_request.source = std::as_bytes(std::span(a_storage.data(), a_storage.size()));
		return true;
	}
}
