#pragma once

#include <CommunityShaders/DX12GraphAPI.h>

#include "Features/DeferredRendering/DeferredTypes.h"
#include "RenderGraph/DX12InteropCoordinator.h"
#include <array>

class DX12RenderRuntime;

class DX12DeferredShading
{
public:
	static DX12DeferredShading& Get();
	bool Initialize(DX12RenderRuntime& runtime) noexcept;
	void Shutdown() noexcept;
	bool PrepareLinearDepth(uint32_t width, uint32_t height) noexcept;
	bool PrepareCompatibilityInput(ID3D11Texture2D* source) noexcept;
	bool PrepareLocalShadowMask(ID3D11Texture2D* source) noexcept;
	bool PrepareScreenSpaceShadow(ID3D11Texture2D* source) noexcept;
	bool PreparePackedSurfaceMirror(ID3D11Texture2D* source) noexcept;
	bool PrepareGBufferInputs() noexcept;
	bool PrepareIndirectLightingInputs() noexcept;
	bool ShouldCommitComposite() const noexcept;
	bool CommitComposite(ID3D11Texture2D* destination) noexcept;
	ID3D11ShaderResourceView* GetCompositeSRV() const noexcept { return composite.srv11.get(); }

private:
	static CSDX12Status CS_DX12_GRAPH_CALL Build(void* userData, CSDX12BuildHandle build);
	static CSDX12Status CS_DX12_GRAPH_CALL Execute(void* userData, const CSDX12ExecutionContext* context);
	static CSDX12Status CS_DX12_GRAPH_CALL ExecuteBinClear(void* userData, const CSDX12ExecutionContext* context);
	static CSDX12Status CS_DX12_GRAPH_CALL ExecuteBinHistogram(void* userData, const CSDX12ExecutionContext* context);
	static CSDX12Status CS_DX12_GRAPH_CALL ExecuteBinPrefix(void* userData, const CSDX12ExecutionContext* context);
	static CSDX12Status CS_DX12_GRAPH_CALL ExecuteBinScatter(void* userData, const CSDX12ExecutionContext* context);
	static void CS_DX12_GRAPH_CALL OnShutdown(void* userData);
	CSDX12Status Record(const CSDX12ExecutionContext& context) noexcept;
	enum class BinningStage { Clear, Histogram, Prefix, Scatter };
	CSDX12Status RecordBinning(const CSDX12ExecutionContext& context, BinningStage stage) noexcept;
	bool EnsureComposite(uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept;
	bool EnsureGBufferInputs() noexcept;
	bool EnsureLinearDepth(uint32_t width, uint32_t height) noexcept;
	bool EnsureFrameMarker() noexcept;
	bool EnsureCompositeBlit(ID3D11Texture2D* destination) noexcept;
	bool CreatePipeline() noexcept;
	bool CreateBinningPipelines() noexcept;
	std::uint32_t GetEnabledEvaluatorMask() const noexcept;

	struct ImportedInput
	{
		winrt::com_ptr<ID3D11Texture2D> source;
		DX12InteropCoordinator::SharedTexture mirror;
		CSDX12ResourceHandle handle{};
	};

	DX12RenderRuntime* runtime{};
	CSDX12RegistrationHandle registration{};
	CSDX12ResourceHandle compositeHandle{};
	CSDX12ResourceHandle specularCompositeHandle{};
	DX12InteropCoordinator::SharedTexture composite;
	DX12InteropCoordinator::SharedTexture specularComposite;
	DX12InteropCoordinator::SharedTexture linearDepth;
	DX12InteropCoordinator::SharedTexture localShadowMask;
	DX12InteropCoordinator::SharedTexture screenSpaceShadow;
	DX12InteropCoordinator::SharedTexture compatibilityReference;
	DX12InteropCoordinator::SharedTexture frameMarker;
	DX12InteropCoordinator::SharedTexture packedSurfaceMirror;
	CSDX12ResourceHandle localShadowMaskHandle{};
	CSDX12ResourceHandle screenSpaceShadowHandle{};
	CSDX12ResourceHandle compatibilityReferenceHandle{};
	CSDX12ResourceHandle linearDepthHandle{};
	CSDX12ResourceHandle frameMarkerHandle{};
	CSDX12ResourceHandle packedSurfaceMirrorHandle{};
	CSDX12ResourceHandle lightsHandle{};
	CSDX12ResourceHandle contextsHandle{};
	CSDX12ResourceHandle clustersHandle{};
	CSDX12ResourceHandle pagesHandle{};
	CSDX12ResourceHandle evaluatorCountsHandle{};
	CSDX12ResourceHandle evaluatorOffsetsHandle{};
	CSDX12ResourceHandle evaluatorCursorsHandle{};
	CSDX12ResourceHandle evaluatorPixelListHandle{};
	CSDX12ResourceHandle evaluatorIndirectArgsHandle{};
	winrt::com_ptr<ID3D11ComputeShader> linearizeDepthShader;
	winrt::com_ptr<ID3D11VertexShader> compositeBlitVS;
	winrt::com_ptr<ID3D11PixelShader> compositeBlitPS;
	winrt::com_ptr<ID3D11PixelShader> compositeSelectiveBlitPS;
	winrt::com_ptr<ID3D11PixelShader> compositeCoverageOverlayPS;
	winrt::com_ptr<ID3D11Texture2D> compositeBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> compositeBlitRTV;
	winrt::com_ptr<ID3D11Texture2D> specularBlitDestination;
	winrt::com_ptr<ID3D11RenderTargetView> specularBlitRTV;
	winrt::com_ptr<ID3D11Buffer> handoffConstants;
	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	std::array<winrt::com_ptr<ID3D12PipelineState>, CS::Deferred::kEvaluatorCount> evaluatorPipelines;
	winrt::com_ptr<ID3D12RootSignature> binningRootSignature;
	std::array<winrt::com_ptr<ID3D12PipelineState>, 4> binningPipelines;
	winrt::com_ptr<ID3D12CommandSignature> evaluatorCommandSignature;
	std::array<ImportedInput, 6> inputs;
	ImportedInput envIBLInput;
	ImportedInput skyIBLInput;
	ImportedInput skylightingProbeInput;
	ImportedInput skylightingVisibilityInput;
	bool iblInputsAvailable{};
	bool skylightingInputsAvailable{};
	CSDX12ResourceHandle envIBLHandle{};
	CSDX12ResourceHandle skyIBLHandle{};
	CSDX12ResourceHandle skylightingProbeHandle{};
	CSDX12ResourceHandle skylightingVisibilityHandle{};
	winrt::com_ptr<ID3D11Texture2D> parityCounterReadback;
	bool parityCounterPending{};
	uint64_t dispatchCount{};
};
