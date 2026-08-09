#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

class DX12InteropCoordinator
{
public:
	struct SharedTexture
	{
		winrt::com_ptr<ID3D11Texture2D> d3d11;
		winrt::com_ptr<ID3D12Resource> d3d12;
		winrt::com_ptr<ID3D11ShaderResourceView> srv11;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav11;
		D3D11_TEXTURE2D_DESC description{};

		explicit operator bool() const noexcept { return d3d11 && d3d12; }
	};

	DX12InteropCoordinator(ID3D11Device5* device11, ID3D12Device* device12);

	bool CreateSharedTexture(
		const D3D11_TEXTURE2D_DESC& description,
		bool createSRV,
		bool createUAV,
		SharedTexture& output) const noexcept;
	// Preferred D3D12 -> D3D11 handoff. The D3D12 device owns a committed
	// shared-heap allocation and exports its NT handle; D3D11 only opens views.
	// Callers must use a format supported by SharedResourceCompatibilityTier.
	// Do not re-share the opened D3D11 object or combine legacy KMT handles with
	// this path. Queue ownership is synchronized exclusively by the host fences.
	bool CreateD3D12OwnedSharedTexture(
		uint32_t width,
		uint32_t height,
		DXGI_FORMAT format,
		D3D12_RESOURCE_FLAGS flags,
		bool createD3D11SRV,
		SharedTexture& output) const noexcept;

	bool OpenSharedTexture(ID3D11Texture2D* texture, ID3D12Resource** output) const noexcept;
	// Preferred D3D11 -> D3D12 read-only path when the producer allocation was
	// created with D3D11_RESOURCE_MISC_SHARED_NTHANDLE. No texture copy is
	// required: the host readiness fence orders all preceding D3D11 writes before
	// the first ORG transition/read of this imported allocation.
	bool ImportReadOnlySharedTexture(ID3D11Texture2D* source, bool createD3D11SRV, SharedTexture& imported) const noexcept;
	bool EnsureReadOnlyMirror(ID3D11Texture2D* source, SharedTexture& mirror) const noexcept;
	bool CopyToMirror(ID3D11DeviceContext* context, ID3D11Texture2D* source, SharedTexture& mirror) const noexcept;

private:
	winrt::com_ptr<ID3D11Device5> device11;
	winrt::com_ptr<ID3D12Device> device12;
};
