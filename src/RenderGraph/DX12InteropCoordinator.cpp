#include "DX12InteropCoordinator.h"

#include "PCH.h"

DX12InteropCoordinator::DX12InteropCoordinator(ID3D11Device5* d3d11, ID3D12Device* d3d12)
{
	device11.copy_from(d3d11);
	device12.copy_from(d3d12);
}

bool DX12InteropCoordinator::CreateD3D12OwnedSharedTexture(
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT format,
	D3D12_RESOURCE_FLAGS flags,
	bool createD3D11SRV,
	SharedTexture& output) const noexcept
{
	D3D11_TEXTURE2D_DESC description{};
	description.Width = width;
	description.Height = height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	return CreateD3D12OwnedSharedTexture(description, flags, createD3D11SRV, output);
}

bool DX12InteropCoordinator::CreateD3D12OwnedSharedTexture(
	const D3D11_TEXTURE2D_DESC& requested,
	D3D12_RESOURCE_FLAGS flags,
	bool createD3D11SRV,
	SharedTexture& output) const noexcept
{
	output = {};
	if (!device11 || !device12 || !requested.Width || !requested.Height ||
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
		description.Flags = flags | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		const HRESULT createResult = device12->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_SHARED, &description, D3D12_RESOURCE_STATE_COMMON,
			nullptr, IID_PPV_ARGS(output.d3d12.put()));
		if (FAILED(createResult)) {
			logger::error("[DX12Interop] D3D12 CreateCommittedResource(shared) failed: HRESULT=0x{:08X}, format={}, flags=0x{:X}",
				static_cast<unsigned>(createResult), static_cast<unsigned>(requested.Format), static_cast<unsigned>(description.Flags));
			return false;
		}
		HANDLE handle{};
		const HRESULT shareResult = device12->CreateSharedHandle(
			output.d3d12.get(), nullptr, GENERIC_ALL, nullptr, &handle);
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
		if ((flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 &&
			FAILED(device11->CreateUnorderedAccessView(output.d3d11.get(), nullptr, output.uav11.put()))) {
			logger::error("[DX12Interop] CreateUnorderedAccessView for D3D12-owned shared texture failed");
			output = {};
			return false;
		}
		output.d3d11->GetDesc(&output.description);
		return true;
	} catch (...) {
		output = {};
		logger::error("[DX12Interop] Exception while creating D3D12-owned shared Texture2D");
		return false;
	}
}

bool DX12InteropCoordinator::CreateD3D12OwnedSharedTextureArray(
	uint32_t width,
	uint32_t height,
	uint16_t arraySize,
	DXGI_FORMAT format,
	D3D12_RESOURCE_FLAGS flags,
	bool createD3D11SRV,
	SharedTexture& output) const noexcept
{
	output = {};
	if (!device11 || !device12 || !width || !height || !arraySize || format == DXGI_FORMAT_UNKNOWN)
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
		description.Format = format;
		description.SampleDesc.Count = 1;
		description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		description.Flags = flags | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		if (FAILED(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
			&description, D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(output.d3d12.put()))))
			return false;
		HANDLE handle{};
		if (FAILED(device12->CreateSharedHandle(output.d3d12.get(), nullptr,
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
		output.d3d11->GetDesc(&output.description);
		return true;
	} catch (...) {
		output = {};
		logger::error("[DX12Interop] Exception while creating D3D12-owned shared Texture2DArray");
		return false;
	}
}

bool DX12InteropCoordinator::OpenSharedTexture(ID3D11Texture2D* texture, ID3D12Resource** output) const noexcept
{
	if (!texture || !output || !device12)
		return false;
	*output = nullptr;
	try {
		D3D11_TEXTURE2D_DESC description{};
		texture->GetDesc(&description);
		if (description.Usage != D3D11_USAGE_DEFAULT || description.SampleDesc.Count != 1 ||
			(description.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) == 0) {
			logger::error("[DX12Interop] Rejected incompatible shared texture: usage={}, samples={}, misc=0x{:X}",
				static_cast<unsigned>(description.Usage), description.SampleDesc.Count, description.MiscFlags);
			return false;
		}

		winrt::com_ptr<IDXGIResource1> dxgiResource;
		if (FAILED(texture->QueryInterface(dxgiResource.put())))
			return false;
		HANDLE handle{};
		const HRESULT createResult = dxgiResource->CreateSharedHandle(
			nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
		if (FAILED(createResult)) {
			logger::error("[DX12Interop] CreateSharedHandle(Texture2D) failed: HRESULT=0x{:08X}",
				static_cast<unsigned>(createResult));
			return false;
		}
		const HRESULT openResult = device12->OpenSharedHandle(handle, IID_PPV_ARGS(output));
		CloseHandle(handle);
		if (FAILED(openResult)) {
			logger::error("[DX12Interop] D3D12 OpenSharedHandle(Texture2D) failed: HRESULT=0x{:08X}",
				static_cast<unsigned>(openResult));
			return false;
		}
		return true;
	} catch (...) {
		logger::error("[DX12Interop] Exception while opening shared Texture2D");
		return false;
	}
}

bool DX12InteropCoordinator::ImportReadOnlySharedTexture(
	ID3D11Texture2D* source,
	bool createD3D11SRV,
	SharedTexture& imported) const noexcept
{
	if (!source)
		return false;
	D3D11_TEXTURE2D_DESC description{};
	source->GetDesc(&description);
	if (imported.d3d11.get() == source && imported.d3d12 &&
		imported.description.Width == description.Width && imported.description.Height == description.Height &&
		imported.description.Format == description.Format)
		return true;

	SharedTexture candidate;
	candidate.d3d11.copy_from(source);
	candidate.description = description;
	if (!OpenSharedTexture(source, candidate.d3d12.put()))
		return false;
	if (createD3D11SRV && FAILED(device11->CreateShaderResourceView(source, nullptr, candidate.srv11.put()))) {
		logger::error("[DX12Interop] Failed to create D3D11 SRV for directly imported shared texture");
		return false;
	}
	imported = std::move(candidate);
	return true;
}

bool DX12InteropCoordinator::CreateSharedTexture(
	const D3D11_TEXTURE2D_DESC& requested,
	bool createSRV,
	bool createUAV,
	SharedTexture& output) const noexcept
{
	output = {};
	if (!device11 || !device12 || !requested.Width || !requested.Height ||
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
		if (!OpenSharedTexture(output.d3d11.get(), output.d3d12.put())) {
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

bool DX12InteropCoordinator::EnsureReadOnlyMirror(ID3D11Texture2D* source, SharedTexture& mirror) const noexcept
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
	if (!CreateD3D12OwnedSharedTexture(sourceDescription,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, true, candidate))
		return false;
	mirror = std::move(candidate);
	return true;
}

bool DX12InteropCoordinator::CopyToMirror(
	ID3D11DeviceContext* context,
	ID3D11Texture2D* source,
	SharedTexture& mirror) const noexcept
{
	if (!context || !EnsureReadOnlyMirror(source, mirror))
		return false;
	context->CopyResource(mirror.d3d11.get(), source);
	return true;
}
