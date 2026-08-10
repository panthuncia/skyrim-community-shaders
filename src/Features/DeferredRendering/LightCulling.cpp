#include "LightCulling.h"

#include "RenderGraph/DX12RenderRuntime.h"
#include "Features/DeferredRendering.h"
#include "Globals.h"

#include <ORGModuleServices/ShaderCompiler.h>
#include <ORGModuleServices/PipelineService.h>

namespace
{
	constexpr uint32_t kMaxLights = DeferredRendering::MAX_LIGHTS;
	constexpr uint32_t kLightsPerPage = 12;
	constexpr uint32_t kPagesPerCluster = 10;

	struct Constants
	{
		Matrix projectionInverse;
		Matrix view;
		uint32_t grid[4];
		float screen[2];
		float nearPlane;
		float farPlane;
		uint32_t lightCount;
		uint32_t contextCount;
		uint32_t pageCapacity;
		uint32_t materialCount;
	};

	constexpr const char* kShader = R"(
struct Light {
 float3 color; float fade; float radius; float invRadius; float fadeZone; float sizeBias;
 float3 positionWS; uint positionPad; uint4 roomFlags; uint lightFlags; uint shadowMaskIndex; uint2 pad;
};
struct LightingContext {
 float4 directionalLightDirection; float4 directionalLightColor; float4 directionalAmbient[3];
 float4 ambientSpecularTintAndFresnelPower; float4 emissiveColor; float4 specularColorAndShininess;
 float4 lightingEffectParams;
 int roomIndex; uint shadowMask; uint featureFlags; uint reserved;
};
struct PBRLandscapeLayerRecord {
 uint baseColorTexture; uint normalTexture; uint rmaosTexture; uint displacementTexture;
 float4 materialParameters; float4 glintParameters;
};
struct PBRMaterialRecord {
 uint abiVersion; uint flags; uint samplerPolicy; uint generation;
 uint4 objectTextures0; uint4 objectTextures1; float4 materialParameters[6];
 PBRLandscapeLayerRecord landscapeLayers[6];
};
struct Cluster { float4 minPoint; float4 maxPoint; uint numLights; uint ptrFirstPage; uint2 pad; };
struct LightPage { uint ptrNextPage; uint numLightsInPage; uint lightIndices[12]; };
cbuffer Frame : register(b0) {
 row_major float4x4 projectionInverse; row_major float4x4 viewMatrix; uint4 grid;
 float2 screen; float nearPlane; float farPlane; uint lightCount; uint contextCount; uint pageCapacity; uint materialCount;
};
StructuredBuffer<Light> uploadedLights : register(t0);
StructuredBuffer<LightingContext> uploadedContexts : register(t1);
StructuredBuffer<PBRMaterialRecord> uploadedPBRMaterials : register(t2);
RWStructuredBuffer<Cluster> clusters : register(u0);
RWStructuredBuffer<LightPage> pages : register(u1);
RWStructuredBuffer<uint> pageCounter : register(u2);
RWStructuredBuffer<uint> diagnostics : register(u3);
RWStructuredBuffer<Light> lights : register(u4);
RWStructuredBuffer<LightingContext> lightingContexts : register(u5);
RWStructuredBuffer<PBRMaterialRecord> pbrMaterials : register(u6);
float3 ScreenToView(float2 pixel) {
 float3 ndc=float3(2.0*pixel.x/screen.x-1.0,2.0*(screen.y-pixel.y-1.0)/screen.y-1.0,1.0);
 float4 p=mul(projectionInverse,float4(ndc,1)); return p.xyz/p.w;
}
float3 AtZ(float3 ray,float z) { return ray*(z/ray.z); }
[numthreads(128,1,1)] void UploadFrameData(uint3 id:SV_DispatchThreadID) {
 if(id.x<lightCount)lights[id.x]=uploadedLights[id.x];
 if(id.x<contextCount)lightingContexts[id.x]=uploadedContexts[id.x];
 if(id.x<materialCount)pbrMaterials[id.x]=uploadedPBRMaterials[id.x];
}
[numthreads(1,1,1)] void BuildClusters(uint3 id:SV_GroupID) {
 if(any(id>=grid.xyz)) return;
 uint index=id.x+id.y*grid.x+id.z*grid.x*grid.y;
 float2 tile=screen/float2(grid.xy); float3 mn=ScreenToView(id.xy*tile); float3 mx=ScreenToView((id.xy+1)*tile);
 float zn=nearPlane*pow(abs(farPlane/nearPlane),id.z/(float)grid.z);
 float zf=nearPlane*pow(abs(farPlane/nearPlane),(id.z+1)/(float)grid.z);
 float3 p0=AtZ(mn,zn),p1=AtZ(mx,zn),p2=AtZ(mn,zf),p3=AtZ(mx,zf);
 clusters[index].minPoint=float4(min(min(p0,p1),min(p2,p3)),0);
 clusters[index].maxPoint=float4(max(max(p0,p1),max(p2,p3)),0);
}
bool Intersects(float3 center,float radius,Cluster c) {
 float3 closest=max(c.minPoint.xyz,min(center,c.maxPoint.xyz)); float3 d=closest-center; return dot(d,d)<=radius*radius;
}
uint AllocatePage() { uint p; InterlockedAdd(pageCounter[0],1,p); if(p>=pageCapacity)InterlockedAdd(diagnostics[0],1); return p<pageCapacity?p:0xffffffff; }
[numthreads(128,1,1)] void CullLights(uint3 dtid:SV_DispatchThreadID) {
 uint total=grid.x*grid.y*grid.z,index=dtid.x; if(index>=total)return;
 Cluster c=clusters[index]; uint page=AllocatePage(); c.numLights=0;c.ptrFirstPage=page;
 if(page==0xffffffff){clusters[index]=c;return;} pages[page].ptrNextPage=0xffffffff; uint inPage=0;
 for(uint i=0;i<lightCount;i++) {
  Light l=lights[i]; float3 center=mul(viewMatrix,float4(l.positionWS,1)).xyz;
  if(!Intersects(center,l.radius,c))continue;
  if(inPage>=12){pages[page].numLightsInPage=12;uint old=page;page=AllocatePage();if(page==0xffffffff)break;
   pages[page].ptrNextPage=old;c.ptrFirstPage=page;inPage=0;}
  pages[page].lightIndices[inPage++]=i;c.numLights++;
 }
 if(page!=0xffffffff)pages[page].numLightsInPage=inPage;InterlockedMax(diagnostics[1],c.numLights);clusters[index]=c;
}
)";

	struct NativePipeline { winrt::com_ptr<ID3D12PipelineState> state; };

	D3D12_RESOURCE_BARRIER Uav(ID3D12Resource* resource)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		barrier.UAV.pResource = resource;
		return barrier;
	}
}

DX12LightCulling& DX12LightCulling::Get() { static DX12LightCulling value; return value; }

bool DX12LightCulling::Initialize(DX12RenderRuntime& owner) noexcept
{
#if !defined(CS_HAS_ORG_MODULE_SERVICES) || !defined(ORG_MODULE_SERVICES_HAS_DXC)
	(void)owner;
	logger::warn("[DX12LightCulling] ORGModuleServices is unavailable; clustered-lighting contributor is disabled");
	return true;
#else
	runtime = &owner;
	device.copy_from(owner.GetNativeDevice());
	if (!device || !CreatePipeline()) return false;
	CSDX12ContributorDesc desc{};
	desc.structSize = sizeof(desc); desc.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	desc.id = "community-shaders.clustered-lighting"; desc.kind = CS_DX12_CONTRIBUTOR_REQUIRED;
	desc.userData = this; desc.build = &Build; desc.shutdown = &OnShutdown;
	return owner.Register(&desc, &registration) == CS_DX12_OK;
#endif
}

CSDX12Status DX12LightCulling::Build(void* userData, CSDX12BuildHandle build)
{
	auto* self = static_cast<DX12LightCulling*>(userData);
	const uint32_t width = std::max(1u, self->runtime->GetRenderWidth());
	const uint32_t height = std::max(1u, self->runtime->GetRenderHeight());
	const uint32_t clusterCount = ((width + 63) / 64) * ((height + 63) / 64) * 32;
	self->clusterCapacity = clusterCount;
	self->pageCapacity = clusterCount * kPagesPerCluster;
	CSDX12Status declarationStatus = CS_DX12_OK;
	auto declareBuffer = [&](const char* id, uint64_t size, uint32_t stride, uint32_t allowed, CSDX12ResourceHandle& handle) {
		CSDX12ResourceDesc desc{};
		desc.structSize = sizeof(desc); desc.apiVersion = CS_DX12_GRAPH_API_CURRENT;
		desc.id = id; desc.lifetime = CS_DX12_RESOURCE_PERSISTENT; desc.dimension = CS_DX12_RESOURCE_BUFFER;
		desc.sizing = CS_DX12_SIZE_ABSOLUTE; desc.byteSize = size; desc.structureByteStride = stride;
		desc.allowedAccess = allowed; desc.initialAccess = CS_DX12_ACCESS_NONE; desc.finalAccess = CS_DX12_ACCESS_SHADER_READ;
		const auto status = self->runtime->DeclareResource(build, &desc, &handle);
		if (status != CS_DX12_OK && declarationStatus == CS_DX12_OK) {
			declarationStatus = status;
			logger::error("[DX12LightCulling] Resource declaration failed for {} (status={})", id,
				static_cast<unsigned>(status));
		}
		return status;
	};
	const uint32_t sharedReadWrite = CS_DX12_ACCESS_SHADER_READ | CS_DX12_ACCESS_UNORDERED_WRITE | CS_DX12_ACCESS_COPY_DESTINATION;
	if (declareBuffer("community-shaders.clustered-lighting.lights", uint64_t(kMaxLights) * sizeof(DeferredRendering::LightData), sizeof(DeferredRendering::LightData), sharedReadWrite, self->lightsHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.clustered-lighting.contexts", uint64_t(DeferredRendering::INVALID_CONTEXT) * sizeof(DeferredRendering::LightingContext), sizeof(DeferredRendering::LightingContext), sharedReadWrite, self->contextsHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.clustered-lighting.pbr-materials", uint64_t(CS::Deferred::kMaxPBRMaterials) * sizeof(CS::Deferred::PBRMaterialRecord), sizeof(CS::Deferred::PBRMaterialRecord), sharedReadWrite, self->pbrMaterialsHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.clustered-lighting.clusters", uint64_t(clusterCount) * 48, 48, sharedReadWrite, self->clustersHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.clustered-lighting.pages", uint64_t(self->pageCapacity) * 56, 56, sharedReadWrite, self->pagesHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.clustered-lighting.page-counter", 4, 4, CS_DX12_ACCESS_UNORDERED_WRITE | CS_DX12_ACCESS_COPY_DESTINATION, self->pageCounterHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.clustered-lighting.diagnostics", 16, 4, CS_DX12_ACCESS_UNORDERED_WRITE | CS_DX12_ACCESS_COPY_DESTINATION | CS_DX12_ACCESS_SHADER_READ, self->diagnosticsHandle) != CS_DX12_OK)
		return declarationStatus;
	const char* after[]{ "cs.gbuffer.ready" };
	const char* before[]{ "cs.deferred-lighting.begin" };
	CSDX12ResourceAccessDesc accesses[] = {
		{ sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT, self->lightsHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} },
		{ sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT, self->contextsHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} },
		{ sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT, self->pbrMaterialsHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} },
		{ sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT, self->clustersHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} },
		{ sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT, self->pagesHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} },
		{ sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT, self->pageCounterHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} },
		{ sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT, self->diagnosticsHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} }
	};
	CSDX12PassDesc pass{};
	pass.structSize = sizeof(pass); pass.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	pass.id = "community-shaders.clustered-lighting.cull"; pass.queuePolicy = CS_DX12_QUEUE_REQUIRE_COMPUTE;
	pass.after = after; pass.afterCount = 1; pass.before = before; pass.beforeCount = 1;
	pass.accesses = accesses; pass.accessCount = static_cast<uint32_t>(std::size(accesses)); pass.execute = &Execute;
	CSDX12PassHandle handle{};
	return self->runtime->DeclarePass(build, &pass, &handle);
}

CSDX12Status DX12LightCulling::Execute(void* userData, const CSDX12ExecutionContext* context)
{
	if (!userData || !context || !context->borrowedD3D12GraphicsCommandList || !context->frame) return CS_DX12_E_INVALID_ARGUMENT;
	return static_cast<DX12LightCulling*>(userData)->Record(*context);
}

void DX12LightCulling::OnShutdown(void* userData) { static_cast<DX12LightCulling*>(userData)->Shutdown(); }

bool DX12LightCulling::CreatePipeline() noexcept
{
#if !defined(ORG_MODULE_SERVICES_HAS_DXC)
	return false;
#else
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
	ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 7, 0, 0, 3 };
	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor.ShaderRegister = 0; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 2, ranges }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC root{ 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	winrt::com_ptr<ID3DBlob> serialized, errors;
	if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put())) ||
		FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(rootSignature.put())))) return false;
	auto* compiler = runtime->GetShaderCompiler();
	auto* pipelines = runtime->GetPipelineService();
	if (!compiler || !pipelines || !compiler->Available()) {
		logger::error("[DX12LightCulling] ORGModuleServices DXC compiler or pipeline service is unavailable");
		return false;
	}
	auto build = [&](const wchar_t* entry, const char* id, winrt::com_ptr<ID3D12PipelineState>& output) {
		org::services::ShaderCompileRequest request{};
		request.sourceName = "community-shaders.clustered-lighting.hlsl";
		request.source = { reinterpret_cast<const std::byte*>(kShader), std::strlen(kShader) };
		request.entryPoint = entry; request.target = L"cs_6_0";
		auto artifact = compiler->Compile(request);
		if (!artifact) { logger::error("[DX12LightCulling] DXC failed for {}: {}", id, artifact.diagnostics); return false; }
		org::services::PipelineRecipe recipe{};
		recipe.id = id; recipe.layoutKey = reinterpret_cast<uintptr_t>(rootSignature.get()); recipe.shaderKey = artifact.key;
		recipe.deviceKey = reinterpret_cast<uintptr_t>(device.get());
		recipe.build = [nativeDevice = device, signature = rootSignature, bytecode = std::move(artifact.binary), pipelineName = std::string(id)]() mutable {
			auto payload = std::make_shared<NativePipeline>();
			D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = signature.get(); desc.CS = { bytecode.data(), bytecode.size() };
			const HRESULT result = nativeDevice->CreateComputePipelineState(&desc, IID_PPV_ARGS(payload->state.put()));
			if (FAILED(result)) {
				logger::error("[DX12LightCulling] CreateComputePipelineState failed for {} with HRESULT 0x{:08X}", pipelineName, static_cast<unsigned>(result));
				return org::services::PipelinePayload{};
			}
			return std::static_pointer_cast<void>(payload);
		};
		auto pipeline = pipelines->Request(std::move(recipe)).get();
		if (!pipeline) { logger::error("[DX12LightCulling] Pipeline creation failed for {}: {}", id, pipeline.error); return false; }
		output = std::static_pointer_cast<NativePipeline>(pipeline.payload)->state;
		return true;
	};
	const bool result = build(L"UploadFrameData", "community-shaders.clustered-lighting.upload-frame", uploadPipeline) &&
		build(L"BuildClusters", "community-shaders.clustered-lighting.build-clusters", clusterPipeline) &&
		build(L"CullLights", "community-shaders.clustered-lighting.cull-lights", cullPipeline);
	pipelines->PublishReady(0);
	return result;
#endif
}

CSDX12Status DX12LightCulling::Record(const CSDX12ExecutionContext& context) noexcept
{
	const auto& frame = *context.frame;
	auto* commandList = static_cast<ID3D12GraphicsCommandList*>(context.borrowedD3D12GraphicsCommandList);
	if (!context.GetResource) return CS_DX12_E_UNSUPPORTED_CAPABILITY;
	auto get = [&](CSDX12ResourceHandle handle, ID3D12Resource*& output) {
		void* resource{};
		const auto status = context.GetResource(&context, handle, &resource);
		output = static_cast<ID3D12Resource*>(resource);
		return status == CS_DX12_OK && output;
	};
	ID3D12Resource* lights{}; ID3D12Resource* contexts{}; ID3D12Resource* pbrMaterials{}; ID3D12Resource* clusters{};
	ID3D12Resource* pages{}; ID3D12Resource* pageCounter{}; ID3D12Resource* diagnostics{};
	if (!get(lightsHandle, lights) || !get(contextsHandle, contexts) || !get(pbrMaterialsHandle, pbrMaterials) || !get(clustersHandle, clusters) ||
		!get(pagesHandle, pages) || !get(pageCounterHandle, pageCounter) || !get(diagnosticsHandle, diagnostics))
		return CS_DX12_E_NOT_READY;
	auto snapshot = globals::features::deferredRendering.GetFrameSnapshot();
	if (!snapshot) return CS_DX12_E_NOT_READY;
	if (!context.AllocateUpload || !context.AllocateDescriptors) return CS_DX12_E_UNSUPPORTED_CAPABILITY;
	CSDX12UploadAllocation lightUpload{}, contextUpload{}, materialUpload{}, constantsUpload{};
	if (context.AllocateUpload(&context, std::max<size_t>(1, snapshot->lights.size()) * sizeof(DeferredRendering::LightData), 16, &lightUpload) != CS_DX12_OK ||
		context.AllocateUpload(&context, std::max<size_t>(1, snapshot->contexts.size()) * sizeof(DeferredRendering::LightingContext), 16, &contextUpload) != CS_DX12_OK ||
		context.AllocateUpload(&context, std::max<size_t>(1, snapshot->pbrMaterials.size()) * sizeof(CS::Deferred::PBRMaterialRecord), 16, &materialUpload) != CS_DX12_OK ||
		context.AllocateUpload(&context, 256, 256, &constantsUpload) != CS_DX12_OK) return CS_DX12_E_INTERNAL;
	if (!snapshot->lights.empty())
		std::memcpy(lightUpload.cpuAddress, snapshot->lights.data(), snapshot->lights.size() * sizeof(DeferredRendering::LightData));
	if (!snapshot->contexts.empty())
		std::memcpy(contextUpload.cpuAddress, snapshot->contexts.data(), snapshot->contexts.size() * sizeof(DeferredRendering::LightingContext));
	if (!snapshot->pbrMaterials.empty())
		std::memcpy(materialUpload.cpuAddress, snapshot->pbrMaterials.data(), snapshot->pbrMaterials.size() * sizeof(CS::Deferred::PBRMaterialRecord));
	Constants constants{}; constants.projectionInverse=snapshot->projectionInverse; constants.view=snapshot->cameraView;
	constants.grid[0]=(frame.width+63)/64;constants.grid[1]=(frame.height+63)/64;constants.grid[2]=32;constants.screen[0]=(float)frame.width;constants.screen[1]=(float)frame.height;
	constants.nearPlane=snapshot->nearPlane;constants.farPlane=snapshot->farPlane;constants.lightCount=(uint32_t)snapshot->lights.size();constants.contextCount=(uint32_t)snapshot->contexts.size();constants.pageCapacity=pageCapacity;constants.materialCount=(uint32_t)snapshot->pbrMaterials.size();
	std::memcpy(constantsUpload.cpuAddress,&constants,sizeof(constants));
	CSDX12DescriptorAllocation descriptorAllocation{};
	if (context.AllocateDescriptors(&context, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12, &descriptorAllocation) != CS_DX12_OK) return CS_DX12_E_INTERNAL;
	auto* descriptors = static_cast<ID3D12DescriptorHeap*>(descriptorAllocation.borrowedNativeHeap);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu{ descriptorAllocation.cpuHandle };
	auto cpuAt=[&](uint32_t index){auto value=cpu;value.ptr+=uint64_t(index)*descriptorAllocation.descriptorSize;return value;};
	auto gpuAt=[&](uint32_t index){D3D12_GPU_DESCRIPTOR_HANDLE value{descriptorAllocation.gpuHandle};value.ptr+=uint64_t(index)*descriptorAllocation.descriptorSize;return value;};
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Buffer.NumElements=std::max<uint32_t>(1, constants.lightCount); srv.Buffer.StructureByteStride=sizeof(DeferredRendering::LightData);
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(lightUpload.borrowedNativeResource),&srv,cpuAt(0));
	srv.Buffer.NumElements=std::max<uint32_t>(1, constants.contextCount); srv.Buffer.StructureByteStride=sizeof(DeferredRendering::LightingContext);
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(contextUpload.borrowedNativeResource),&srv,cpuAt(1));
	srv.Buffer.NumElements=std::max<uint32_t>(1, constants.materialCount); srv.Buffer.StructureByteStride=sizeof(CS::Deferred::PBRMaterialRecord);
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(materialUpload.borrowedNativeResource),&srv,cpuAt(2));
	auto makeUav=[&](ID3D12Resource* resource,uint32_t index,uint32_t elements,uint32_t stride){D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=elements;u.Buffer.StructureByteStride=stride;device->CreateUnorderedAccessView(resource,nullptr,&u,cpuAt(index));};
	makeUav(clusters,3,clusterCapacity,48); makeUav(pages,4,pageCapacity,56); makeUav(pageCounter,5,1,4); makeUav(diagnostics,6,4,4);
	makeUav(lights,7,kMaxLights,sizeof(DeferredRendering::LightData)); makeUav(contexts,8,DeferredRendering::INVALID_CONTEXT,sizeof(DeferredRendering::LightingContext));
	makeUav(pbrMaterials,9,CS::Deferred::kMaxPBRMaterials,sizeof(CS::Deferred::PBRMaterialRecord));
	// ClearUnorderedAccessView requires its CPU descriptor to come from a
	// non-shader-visible heap. It also rejects structured UAV descriptors, so
	// use equivalent raw views solely for clearing the two uint buffers.
	D3D12_DESCRIPTOR_HEAP_DESC clearHeapDesc{}; clearHeapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; clearHeapDesc.NumDescriptors=2;
	winrt::com_ptr<ID3D12DescriptorHeap> clearHeap;
	if (FAILED(device->CreateDescriptorHeap(&clearHeapDesc, IID_PPV_ARGS(clearHeap.put())))) return CS_DX12_E_INTERNAL;
	auto clearCpu=clearHeap->GetCPUDescriptorHandleForHeapStart();
	auto clearCpuAt=[&](uint32_t index){auto value=clearCpu;value.ptr+=uint64_t(index)*descriptorAllocation.descriptorSize;return value;};
	auto makeRawClearUav=[&](ID3D12Resource* resource,uint32_t index,uint32_t elements){D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=elements;u.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(resource,nullptr,&u,clearCpuAt(index));device->CopyDescriptorsSimple(1,cpuAt(10+index),clearCpuAt(index),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);};
	makeRawClearUav(pageCounter,0,1); makeRawClearUav(diagnostics,1,4);
	ID3D12DescriptorHeap* heaps[]{descriptors};commandList->SetDescriptorHeaps(1,heaps);commandList->SetComputeRootSignature(rootSignature.get());commandList->SetComputeRootConstantBufferView(0,constantsUpload.gpuAddress);commandList->SetComputeRootDescriptorTable(1,{descriptorAllocation.gpuHandle});
	const UINT clear[4]{}; commandList->ClearUnorderedAccessViewUint(gpuAt(10),clearCpuAt(0),pageCounter,clear,0,nullptr); commandList->ClearUnorderedAccessViewUint(gpuAt(11),clearCpuAt(1),diagnostics,clear,0,nullptr);
	const uint32_t uploadCount = std::max({constants.lightCount, constants.contextCount, constants.materialCount});
	if (uploadCount != 0) {
		commandList->SetPipelineState(uploadPipeline.get());
		commandList->Dispatch((uploadCount + 127) / 128, 1, 1);
	}
	D3D12_RESOURCE_BARRIER uploaded[]{Uav(lights),Uav(contexts),Uav(pbrMaterials)};commandList->ResourceBarrier(3,uploaded);
	commandList->SetPipelineState(clusterPipeline.get());commandList->Dispatch(constants.grid[0],constants.grid[1],constants.grid[2]);auto clusterBarrier=Uav(clusters);commandList->ResourceBarrier(1,&clusterBarrier);
	commandList->SetPipelineState(cullPipeline.get());const uint32_t total=constants.grid[0]*constants.grid[1]*constants.grid[2];commandList->Dispatch((total+127)/128,1,1);D3D12_RESOURCE_BARRIER done[]{Uav(clusters),Uav(pages),Uav(pageCounter),Uav(diagnostics)};commandList->ResourceBarrier(4,done);
	globals::features::deferredRendering.RetainSubmittedFrame(snapshot, frame.completionValue);
	if (dispatchCount++ == 0 || (constants.lightCount != 0 && !loggedActiveLights)) {
		logger::info("[DX12LightCulling] ORG dispatched BasicRenderer-style clustered culling: {} clusters, {} lights, {} contexts, {} PBR materials, {} page capacity",total,constants.lightCount,snapshot->contexts.size(),snapshot->pbrMaterials.size(),pageCapacity);
		loggedActiveLights |= constants.lightCount != 0;
	}
	return CS_DX12_OK;
}

void DX12LightCulling::Shutdown() noexcept
{
	uploadPipeline=nullptr;cullPipeline=nullptr;clusterPipeline=nullptr;rootSignature=nullptr;device=nullptr;
}
