#include "D3D11InteropBridge.h"

#include "PCH.h"
#include <rhi_helpers.h>
#include <rhi_interop_dx12.h>

namespace
{
	DXGI_FORMAT ToDXGI(rhi::Format format) noexcept
	{
		if (format == rhi::Format::Unknown) return DXGI_FORMAT_UNKNOWN;
		for (uint32_t value = 1; value <= static_cast<uint32_t>(DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE); ++value) {
			const auto candidate = static_cast<DXGI_FORMAT>(value);
			if (rhi::helpers::ToRHI(candidate) == format) return candidate;
		}
		return DXGI_FORMAT_UNKNOWN;
	}

	std::shared_ptr<void> RetainNative(ID3D12Resource* native)
	{
		if (!native) return {};
		native->AddRef();
		return { native, [](void* value) { static_cast<ID3D12Resource*>(value)->Release(); } };
	}

	bool OpenSharedTexture(rhi::Device device, ID3D11Texture2D* texture, std::shared_ptr<void>& output) noexcept
	{
		auto* nativeDevice = rhi::dx12::get_device(device);
		if (!texture || !nativeDevice) return false;
		try {
			D3D11_TEXTURE2D_DESC description{};
			texture->GetDesc(&description);
			if (description.Usage != D3D11_USAGE_DEFAULT || description.SampleDesc.Count != 1 ||
				(description.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) == 0) return false;
			winrt::com_ptr<IDXGIResource1> dxgiResource;
			if (FAILED(texture->QueryInterface(dxgiResource.put()))) return false;
			HANDLE handle{};
			if (FAILED(dxgiResource->CreateSharedHandle(nullptr,
				DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) return false;
			winrt::com_ptr<ID3D12Resource> native;
			const auto result = nativeDevice->OpenSharedHandle(handle, IID_PPV_ARGS(native.put()));
			CloseHandle(handle);
			if (FAILED(result)) return false;
			output = RetainNative(native.get());
			return static_cast<bool>(output);
		} catch (...) { return false; }
	}
}

D3D11InteropBridge::D3D11InteropBridge(ID3D11Device5* d3d11, rhi::Device graphDevice) : device(graphDevice)
{
	device11.copy_from(d3d11);
}

bool D3D11InteropBridge::OpenTimeline(rhi::Timeline timeline, ID3D11Fence** output) const noexcept
{
	if (!output || !device11) return false;
	*output = nullptr;
	auto* nativeDevice = rhi::dx12::get_device(device);
	auto* nativeTimeline = rhi::dx12::get_timeline(timeline);
	if (!nativeDevice || !nativeTimeline) return false;
	HANDLE handle{};
	if (FAILED(nativeDevice->CreateSharedHandle(nativeTimeline, nullptr, GENERIC_ALL, nullptr, &handle))) return false;
	const auto result = device11->OpenSharedFence(handle, IID_PPV_ARGS(output));
	CloseHandle(handle);
	return SUCCEEDED(result);
}

bool D3D11InteropBridge::ProbeGraphOwnedSharing() const noexcept
{
	auto* nativeDevice = rhi::dx12::get_device(device);
	if (!nativeDevice) return false;
	D3D12_FEATURE_DATA_D3D12_OPTIONS4 options{};
	if (SUCCEEDED(nativeDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options, sizeof(options))))
		logger::info("[D3D11Interop] Shared-resource compatibility tier={}",
			static_cast<unsigned>(options.SharedResourceCompatibilityTier));
	SharedTexture probe;
	return CreateGraphOwnedSharedTexture(1, 1, rhi::Format::R16G16B16A16_Float,
		rhi::RF_AllowRenderTarget | rhi::RF_AllowUnorderedAccess, true, probe);
}

bool D3D11InteropBridge::CreateGraphOwnedSharedTexture(
	uint32_t width,
	uint32_t height,
	rhi::Format format,
	rhi::ResourceFlags flags,
	bool createD3D11SRV,
	SharedTexture& output) const noexcept
{
	D3D11_TEXTURE2D_DESC description{};
	description.Width = width;
	description.Height = height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = ToDXGI(format);
	description.SampleDesc.Count = 1;
	return CreateGraphOwnedSharedTexture(description, flags, createD3D11SRV, output);
}

bool D3D11InteropBridge::CreateGraphOwnedSharedTexture(
	const D3D11_TEXTURE2D_DESC& requested,
	rhi::ResourceFlags flags,
	bool createD3D11SRV,
	SharedTexture& output) const noexcept
{
	output = {};
	auto* nativeDevice = rhi::dx12::get_device(device);
	if (!device11 || !nativeDevice || !requested.Width || !requested.Height ||
		!requested.ArraySize || requested.Format == DXGI_FORMAT_UNKNOWN ||
		requested.SampleDesc.Count != 1)
		return false;
	try {
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		heap.CreationNodeMask = 1;
		heap.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC description{};
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.Width = requested.Width;
		description.Height = requested.Height;
		description.DepthOrArraySize = static_cast<UINT16>(requested.ArraySize);
		description.MipLevels = static_cast<UINT16>(requested.MipLevels);
		description.Format = requested.Format;
		description.SampleDesc = requested.SampleDesc;
		description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		description.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags) | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		winrt::com_ptr<ID3D12Resource> nativeResource;
		const HRESULT createResult = nativeDevice->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_SHARED, &description, D3D12_RESOURCE_STATE_COMMON,
			nullptr, IID_PPV_ARGS(nativeResource.put()));
		if (FAILED(createResult)) {
			logger::error("[DX12Interop] D3D12 CreateCommittedResource(shared) failed: HRESULT=0x{:08X}, format={}, flags=0x{:X}",
				static_cast<unsigned>(createResult), static_cast<unsigned>(requested.Format), static_cast<unsigned>(description.Flags));
			return false;
		}
		HANDLE handle{};
		const HRESULT shareResult = nativeDevice->CreateSharedHandle(
			nativeResource.get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(shareResult)) {
			logger::error("[DX12Interop] D3D12 CreateSharedHandle(Texture2D) failed: HRESULT=0x{:08X}", static_cast<unsigned>(shareResult));
			output = {};
			return false;
		}
		const HRESULT openResult = device11->OpenSharedResource1(handle, IID_PPV_ARGS(output.d3d11.put()));
		CloseHandle(handle);
		if (FAILED(openResult)) {
			logger::error("[DX12Interop] D3D11 OpenSharedResource1(D3D12 Texture2D) failed: HRESULT=0x{:08X}, format={}, flags=0x{:X}",
				static_cast<unsigned>(openResult), static_cast<unsigned>(requested.Format), static_cast<unsigned>(description.Flags));
			output = {};
			return false;
		}
		if (createD3D11SRV && FAILED(device11->CreateShaderResourceView(output.d3d11.get(), nullptr, output.srv11.put()))) {
			logger::error("[DX12Interop] CreateShaderResourceView for D3D12-owned shared texture failed");
			output = {};
			return false;
		}
		if ((flags & rhi::RF_AllowUnorderedAccess) != 0 &&
			FAILED(device11->CreateUnorderedAccessView(output.d3d11.get(), nullptr, output.uav11.put()))) {
			logger::error("[DX12Interop] CreateUnorderedAccessView for D3D12-owned shared texture failed");
			output = {};
			return false;
		}
		output.graphBacking = RetainNative(nativeResource.get());
		if (!output.graphBacking) { output = {}; return false; }
		output.d3d11->GetDesc(&output.description);
		return true;
	} catch (...) {
		output = {};
		logger::error("[DX12Interop] Exception while creating D3D12-owned shared Texture2D");
		return false;
	}
}

bool D3D11InteropBridge::CreateGraphOwnedSharedTextureArray(
	uint32_t width,
	uint32_t height,
	uint16_t arraySize,
	rhi::Format format,
	rhi::ResourceFlags flags,
	bool createD3D11SRV,
	SharedTexture& output) const noexcept
{
	output = {};
	auto* nativeDevice = rhi::dx12::get_device(device);
	const auto dxgiFormat = ToDXGI(format);
	if (!device11 || !nativeDevice || !width || !height || !arraySize || dxgiFormat == DXGI_FORMAT_UNKNOWN)
		return false;
	try {
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		heap.CreationNodeMask = heap.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC description{};
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.Width = width;
		description.Height = height;
		description.DepthOrArraySize = arraySize;
		description.MipLevels = 1;
		description.Format = dxgiFormat;
		description.SampleDesc.Count = 1;
		description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		description.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags) | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		winrt::com_ptr<ID3D12Resource> nativeResource;
		if (FAILED(nativeDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
			&description, D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(nativeResource.put()))))
			return false;
		HANDLE handle{};
		if (FAILED(nativeDevice->CreateSharedHandle(nativeResource.get(), nullptr,
			GENERIC_ALL, nullptr, &handle))) {
			output = {};
			return false;
		}
		const HRESULT opened = device11->OpenSharedResource1(handle,
			IID_PPV_ARGS(output.d3d11.put()));
		CloseHandle(handle);
		if (FAILED(opened)) {
			output = {};
			return false;
		}
		if (createD3D11SRV && FAILED(device11->CreateShaderResourceView(
			output.d3d11.get(), nullptr, output.srv11.put()))) {
			output = {};
			return false;
		}
		output.graphBacking = RetainNative(nativeResource.get());
		if (!output.graphBacking) { output = {}; return false; }
		output.d3d11->GetDesc(&output.description);
		return true;
	} catch (...) {
		output = {};
		logger::error("[DX12Interop] Exception while creating D3D12-owned shared Texture2DArray");
		return false;
	}
}

bool D3D11InteropBridge::ImportReadOnlySharedTexture(
	ID3D11Texture2D* source,
	bool createD3D11SRV,
	SharedTexture& imported) const noexcept
{
	if (!source)
		return false;
	D3D11_TEXTURE2D_DESC description{};
	source->GetDesc(&description);
	if (imported.d3d11.get() == source && imported.graphBacking &&
		imported.description.Width == description.Width && imported.description.Height == description.Height &&
		imported.description.Format == description.Format)
		return true;

	SharedTexture candidate;
	candidate.d3d11.copy_from(source);
	candidate.description = description;
	if (!OpenSharedTexture(device, source, candidate.graphBacking))
		return false;
	if (createD3D11SRV && FAILED(device11->CreateShaderResourceView(source, nullptr, candidate.srv11.put()))) {
		logger::error("[DX12Interop] Failed to create D3D11 SRV for directly imported shared texture");
		return false;
	}
	imported = std::move(candidate);
	return true;
}

bool D3D11InteropBridge::CreateSharedTexture(
	const D3D11_TEXTURE2D_DESC& requested,
	bool createSRV,
	bool createUAV,
	SharedTexture& output) const noexcept
{
	output = {};
	if (!device11 || !device || !requested.Width || !requested.Height ||
		requested.Format == DXGI_FORMAT_UNKNOWN || requested.SampleDesc.Count != 1)
		return false;
	try {
		auto description = requested;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.CPUAccessFlags = 0;
		description.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (createSRV) description.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
		if (createUAV) description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
		const HRESULT createResult = device11->CreateTexture2D(&description, nullptr, output.d3d11.put());
		if (FAILED(createResult)) {
			logger::error("[DX12Interop] D3D11 CreateTexture2D(NT shared) failed: HRESULT=0x{:08X}, format={}, bind=0x{:X}, misc=0x{:X}",
				static_cast<unsigned>(createResult), static_cast<unsigned>(description.Format),
				description.BindFlags, description.MiscFlags);
			return false;
		}
		if (!OpenSharedTexture(device, output.d3d11.get(), output.graphBacking)) {
			output = {};
			return false;
		}
		if (createSRV && FAILED(device11->CreateShaderResourceView(output.d3d11.get(), nullptr, output.srv11.put()))) {
			output = {};
			return false;
		}
		if (createUAV && FAILED(device11->CreateUnorderedAccessView(output.d3d11.get(), nullptr, output.uav11.put()))) {
			output = {};
			return false;
		}
		output.description = description;
		return true;
	} catch (...) {
		output = {};
		logger::error("[DX12Interop] Exception while creating shared Texture2D");
		return false;
	}
}

bool D3D11InteropBridge::ImportGraphResource(const SharedTexture& source, rhi::ResourcePtr& output) const noexcept
{
	if (!source.graphBacking) return false;
	return !rhi::Failed(rhi::dx12::import_resource(
		device, static_cast<ID3D12Resource*>(source.graphBacking.get()), output));
}

bool D3D11InteropBridge::EnsureReadOnlyMirror(ID3D11Texture2D* source, SharedTexture& mirror) const noexcept
{
	if (!source)
		return false;
	D3D11_TEXTURE2D_DESC sourceDescription{};
	source->GetDesc(&sourceDescription);
	if (sourceDescription.Usage != D3D11_USAGE_DEFAULT || sourceDescription.SampleDesc.Count != 1) {
		logger::error("[DX12Interop] Cannot mirror texture with usage={} samples={}",
			static_cast<unsigned>(sourceDescription.Usage), sourceDescription.SampleDesc.Count);
		return false;
	}
	if (mirror && mirror.description.Width == sourceDescription.Width &&
		mirror.description.Height == sourceDescription.Height && mirror.description.Format == sourceDescription.Format &&
		mirror.description.MipLevels == sourceDescription.MipLevels && mirror.description.ArraySize == sourceDescription.ArraySize)
		return true;

	SharedTexture candidate;
	// Mirrors are the fallback for engine allocations that cannot be directly
	// imported. The host readiness fence, rather than resource ownership, is what
	// guarantees visibility of the D3D11 copy to D3D12.
	if (!CreateGraphOwnedSharedTexture(sourceDescription,
		rhi::RF_AllowRenderTarget, true, candidate))
		return false;
	mirror = std::move(candidate);
	return true;
}

bool D3D11InteropBridge::CopyToMirror(
	ID3D11DeviceContext* context,
	ID3D11Texture2D* source,
	SharedTexture& mirror) const noexcept
{
	if (!context || !EnsureReadOnlyMirror(source, mirror))
		return false;
	context->CopyResource(mirror.d3d11.get(), source);
	return true;
}
