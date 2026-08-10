#pragma once

#include <rhi.h>

#include "Features/DeferredRendering/DeferredTypes.h"
#include "Features/DeferredRendering.h"
#include <OpenRenderGraph/D3D11Interop.h>
#include <array>
#include <cstddef>

namespace org { struct PassExecutionContext; class Resource; }

class RenderGraphRuntime;
class DeferredShadingExtension;

class DX12DeferredShading
{
public:
	static DX12DeferredShading& Get();
	bool Initialize(RenderGraphRuntime& runtime) noexcept;
	void Shutdown() noexcept;
	bool PrepareLinearDepth(uint32_t width, uint32_t height) noexcept;
	bool PrepareCompatibilityInput(ID3D11Texture2D* source) noexcept;
	bool PrepareLocalShadowMask(ID3D11Texture2D* source) noexcept;
	bool PrepareScreenSpaceShadow(ID3D11Texture2D* source) noexcept;
	bool PreparePackedSurfaceMirror(ID3D11Texture2D* source) noexcept;
	bool PrepareGBufferInputs() noexcept;
	bool PrepareIndirectLightingInputs() noexcept;
	bool PrepareGlintNoiseInput() noexcept;
	bool ShouldCommitComposite() const noexcept;
	bool CommitComposite(ID3D11Texture2D* destination) noexcept;
	ID3D11ShaderResourceView* GetCompositeSRV() const noexcept { return composite.srv11.get(); }
	std::uint32_t GetEnabledEvaluatorMask() const noexcept;
	enum class NativeBinningStage : std::uint32_t { Clear, Histogram, Prefix, Scatter };
	struct NativeBinningBindings
	{
		std::uint32_t packedSurface{ UINT32_MAX };
		std::uint32_t linearDepth{ UINT32_MAX };
		std::uint32_t counts{ UINT32_MAX };
		std::uint32_t offsets{ UINT32_MAX };
		std::uint32_t cursors{ UINT32_MAX };
		std::uint32_t pixels{ UINT32_MAX };
		std::uint32_t indirectArguments{ UINT32_MAX };
		std::uint32_t marker{ UINT32_MAX };
	};
	struct NativeEvaluatorBindings
	{
		std::array<std::uint32_t, 28> descriptors{};
		std::uint32_t frameConstants{ UINT32_MAX };
		rhi::ResourceHandle indirectArguments{};
	};
	struct NativeSeedBindings
	{
		std::array<std::uint32_t, 14> descriptors{};
	};
	bool RecordNativeSeed(org::PassExecutionContext& context,
		const NativeSeedBindings& bindings) noexcept;
	bool RecordNativeBinning(org::PassExecutionContext& context,
		NativeBinningStage stage, const NativeBinningBindings& bindings) noexcept;
	bool RecordNativeEvaluator(org::PassExecutionContext& context,
		std::size_t evaluator, const NativeEvaluatorBindings& bindings) noexcept;
	void UpdateNativeFrame(const std::shared_ptr<org::Resource>& constantsResource);

private:
	friend class DeferredShadingExtension;
	bool EnsureComposite(uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept;
	bool EnsureGBufferInputs() noexcept;
	bool EnsureLinearDepth(uint32_t width, uint32_t height) noexcept;
	bool EnsureFrameMarker() noexcept;
	bool EnsureCompositeBlit(ID3D11Texture2D* destination) noexcept;
	bool CreatePipeline() noexcept;
	bool CreateBinningPipelines() noexcept;

	struct ImportedInput
	{
		winrt::com_ptr<ID3D11Texture2D> source;
		org::interop::D3D11Interop::SharedTexture mirror;
	};

	RenderGraphRuntime* runtime{};
	std::uint64_t nativeRegistration{};
	org::interop::D3D11Interop::SharedTexture composite;
	org::interop::D3D11Interop::SharedTexture specularComposite;
	org::interop::D3D11Interop::SharedTexture reflectanceComposite;
	org::interop::D3D11Interop::SharedTexture albedoComposite;
	org::interop::D3D11Interop::SharedTexture normalComposite;
	org::interop::D3D11Interop::SharedTexture masksComposite;
	org::interop::D3D11Interop::SharedTexture linearDepth;
	org::interop::D3D11Interop::SharedTexture localShadowMask;
	org::interop::D3D11Interop::SharedTexture screenSpaceShadow;
	org::interop::D3D11Interop::SharedTexture compatibilityReference;
	org::interop::D3D11Interop::SharedTexture frameMarker;
	org::interop::D3D11Interop::SharedTexture packedSurfaceMirror;
	winrt::com_ptr<ID3D11ComputeShader> linearizeDepthShader;
	winrt::com_ptr<ID3D11VertexShader> compositeBlitVS;
	winrt::com_ptr<ID3D11PixelShader> compositeBlitPS;
	winrt::com_ptr<ID3D11PixelShader> compositeSelectiveBlitPS;
	winrt::com_ptr<ID3D11PixelShader> compositeCoverageOverlayPS;
	winrt::com_ptr<ID3D11Texture2D> compositeBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> compositeBlitRTV;
	winrt::com_ptr<ID3D11Texture2D> specularBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> specularBlitRTV;
	winrt::com_ptr<ID3D11Texture2D> reflectanceBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> reflectanceBlitRTV;
	winrt::com_ptr<ID3D11Texture2D> albedoBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> albedoBlitRTV;
	winrt::com_ptr<ID3D11Texture2D> normalBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> normalBlitRTV;
	winrt::com_ptr<ID3D11Texture2D> masksBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> masksBlitRTV;
	winrt::com_ptr<ID3D11Buffer> handoffConstants;
	rhi::PipelineLayoutPtr evaluatorLayout;
	std::array<rhi::PipelinePtr, CS::Deferred::kEvaluatorCount> evaluatorPipelines;
	rhi::PipelineLayoutPtr binningLayout;
	rhi::PipelinePtr seedPipeline;
	std::array<rhi::PipelinePtr, 4> binningPipelines;
	rhi::CommandSignaturePtr evaluatorCommandSignature;
	std::array<ImportedInput, 6> inputs;
	ImportedInput envIBLInput;
	ImportedInput skyIBLInput;
	ImportedInput skylightingProbeInput;
	ImportedInput skylightingVisibilityInput;
	ImportedInput glintNoiseInput;
	bool iblInputsAvailable{};
	bool skylightingInputsAvailable{};
	bool glintNoiseAvailable{};
	// Deliberately false until material assets can be opened directly by D3D12
	// (or are natively D3D12-owned). Material textures are never mirrored.
	bool materialTextureSharingAvailable{};
	winrt::com_ptr<ID3D11Texture2D> parityCounterReadback;
	bool parityCounterPending{};
	uint64_t dispatchCount{};
	std::shared_ptr<const DeferredRendering::FrameSnapshot> nativeFrameSnapshot;
};
