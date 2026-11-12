// DxilStages.h: DCLF's stages on D3D12, read and prepared through the dxcompiler.dll in the process.
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DxilStages.h"

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/FrameSource.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"

#include <ORGModuleServices/ShaderCompiler.h>

#include <d3d12shader.h>
#include <dxcapi.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace DCLF::DxilStages
{
	namespace
	{
		// CLSID_DxcCompiler and CLSID_DxcUtils, spelled out so no GUID definition is needed (as Utils/DxcShaderStore.cpp).
		constexpr GUID kClsidDxcCompiler = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
		constexpr GUID kClsidDxcUtils = { 0x6245d6af, 0x66e0, 0x48fd, { 0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c } };

		// The dxcompiler.dll the render graph's compiler loaded: its DxcCreateInstance, or null.
		DxcCreateInstanceProc CreateInstance()
		{
			const HMODULE dxc = ::GetModuleHandleW(L"dxcompiler.dll");
			return dxc ? reinterpret_cast<DxcCreateInstanceProc>(::GetProcAddress(dxc, "DxcCreateInstance")) : nullptr;
		}

		// One utils and one compiler per thread: DXC's objects are not shared between threads.
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

		winrt::com_ptr<ID3D12ShaderReflection> Reflector(std::span<const std::byte> a_dxil)
		{
			auto* utils = Utils();
			if (!utils || !IsDxil(a_dxil))
				return nullptr;
			const DxcBuffer buffer{ a_dxil.data(), a_dxil.size(), 0 };
			winrt::com_ptr<ID3D12ShaderReflection> reflector;
			if (FAILED(utils->CreateReflection(&buffer, __uuidof(ID3D12ShaderReflection), reflector.put_void())))
				return nullptr;
			return reflector;
		}
	}

	bool IsDxil(std::span<const std::byte> a_binary)
	{
		return a_binary.size() >= 32 && std::memcmp(a_binary.data(), "DXBC", 4) == 0;
	}

	bool Reflect(std::span<const std::byte> a_dxil, SpirvReflection& a_out)
	{
		a_out.inputs.clear();
		a_out.bindings.clear();
		a_out.blockMembers.clear();
		a_out.outputLocations = 0;
		const auto reflector = Reflector(a_dxil);
		D3D12_SHADER_DESC desc{};
		if (!reflector || FAILED(reflector->GetDesc(&desc)))
			return false;
		for (UINT i = 0; i < desc.InputParameters; ++i) {
			D3D12_SIGNATURE_PARAMETER_DESC input{};
			if (SUCCEEDED(reflector->GetInputParameterDesc(i, &input)) && input.SemanticName)
				a_out.inputs.push_back({ input.SemanticName, input.SemanticIndex, input.Register });
		}
		for (UINT i = 0; i < desc.OutputParameters; ++i) {
			D3D12_SIGNATURE_PARAMETER_DESC output{};
			if (SUCCEEDED(reflector->GetOutputParameterDesc(i, &output)) && output.SystemValueType == D3D_NAME_TARGET && output.SemanticIndex < 32)
				a_out.outputLocations |= 1u << output.SemanticIndex;
		}
		using Kind = SpirvReflection::BindingKind;
		for (UINT i = 0; i < desc.BoundResources; ++i) {
			D3D12_SHADER_INPUT_BIND_DESC bind{};
			if (FAILED(reflector->GetResourceBindingDesc(i, &bind)))
				continue;
			switch (bind.Type) {
			case D3D_SIT_CBUFFER:
				// The pass's push block (DCLFFramePush, b191) is push data, as the draw's (b190) is kept for the checks.
				if (bind.BindPoint == kFramePushBinding)
					continue;
				a_out.bindings.push_back({ 0, kBindingShiftB + bind.BindPoint, Kind::ConstantBuffer });
				break;
			case D3D_SIT_TEXTURE:
			case D3D_SIT_TBUFFER:
				a_out.bindings.push_back({ 0, kBindingShiftT + bind.BindPoint, Kind::Texture });
				break;
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
				a_out.bindings.push_back({ 0, kBindingShiftT + bind.BindPoint, Kind::StorageBuffer });
				break;
			case D3D_SIT_SAMPLER:
				a_out.bindings.push_back({ 0, kBindingShiftS + bind.BindPoint, Kind::Sampler });
				break;
			default:
				a_out.bindings.push_back({ 0, kBindingShiftU + bind.BindPoint, Kind::Other });
				break;
			}
		}
		for (UINT b = 0; b < desc.ConstantBuffers; ++b) {
			auto* buffer = reflector->GetConstantBufferByIndex(b);
			D3D12_SHADER_BUFFER_DESC bufferDesc{};
			if (!buffer || FAILED(buffer->GetDesc(&bufferDesc)) || bufferDesc.Type != D3D_CT_CBUFFER || !bufferDesc.Name)
				continue;
			for (UINT v = 0; v < bufferDesc.Variables; ++v) {
				auto* variable = buffer->GetVariableByIndex(v);
				D3D12_SHADER_VARIABLE_DESC variableDesc{};
				if (variable && SUCCEEDED(variable->GetDesc(&variableDesc)) && variableDesc.Name)
					a_out.blockMembers.push_back({ bufferDesc.Name, variableDesc.Name, variableDesc.StartOffset });
			}
		}
		return true;
	}

	FragmentFacts Fragment(std::span<const std::byte> a_dxil)
	{
		FragmentFacts facts;
		const auto reflector = Reflector(a_dxil);
		D3D12_SHADER_DESC desc{};
		auto* compiler = Compiler();
		if (!reflector || !compiler || FAILED(reflector->GetDesc(&desc)))
			return facts;
		for (UINT i = 0; i < desc.OutputParameters; ++i) {
			D3D12_SIGNATURE_PARAMETER_DESC output{};
			if (FAILED(reflector->GetOutputParameterDesc(i, &output)))
				continue;
			facts.exportsDepth = facts.exportsDepth || output.SystemValueType == D3D_NAME_DEPTH || output.SystemValueType == D3D_NAME_DEPTH_GREATER_EQUAL ||
			                     output.SystemValueType == D3D_NAME_DEPTH_LESS_EQUAL;
			facts.exportsSampleMask = facts.exportsSampleMask || output.SystemValueType == D3D_NAME_COVERAGE;
		}
		// Discards, texture and typed buffer stores and atomics are DXIL operations: found by name in the disassembly. Raw buffer stores
		// (DeviceStore) are not, as the SPIR-V scan does not count its stores through addresses.
		const DxcBuffer buffer{ a_dxil.data(), a_dxil.size(), 0 };
		winrt::com_ptr<IDxcResult> result;
		winrt::com_ptr<IDxcBlobUtf8> text;
		if (FAILED(compiler->Disassemble(&buffer, __uuidof(IDxcResult), result.put_void())) || !result ||
			FAILED(result->GetOutput(DXC_OUT_DISASSEMBLY, __uuidof(IDxcBlobUtf8), text.put_void(), nullptr)) || !text)
			return facts;
		const std::string_view disassembly(text->GetStringPointer(), text->GetStringLength());
		bool writes = false;
		for (const std::string_view op : { "@dx.op.discard", "@dx.op.textureStore", "@dx.op.bufferStore", "@dx.op.atomicBinOp", "@dx.op.atomicCompareExchange" })
			writes = writes || disassembly.find(op) != std::string_view::npos;
		facts.defersDepth = writes || facts.exportsDepth || facts.exportsSampleMask;
		return facts;
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
		// The frame record's reads first, then the build's source.
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
		auto rewritten = FrameSource::Rewrite(std::string_view(text->GetStringPointer(), text->GetStringLength()), options);
		if (!rewritten.error.empty()) {
			a_error = "the D3D12 build's frame rewrite failed: " + rewritten.error;
			return false;
		}
		a_storage = std::move(rewritten.source);
		// Every include and define has done its work: the compile reads this text alone. Its #line directives name the files.
		a_request.source = std::as_bytes(std::span(a_storage.data(), a_storage.size()));
		return true;
	}
}
