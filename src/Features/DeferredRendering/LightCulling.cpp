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
		uint32_t pageCapacity;
		uint32_t padding[2];
	};

	constexpr const char* kShader = R"(
struct Light {
 float3 color; float fade; float radius; float invRadius; float fadeZone; float sizeBias;
 float3 positionWS; uint positionPad; uint4 roomFlags; uint lightFlags; uint shadowMaskIndex; uint2 pad;
};
struct Cluster { float4 minPoint; float4 maxPoint; uint numLights; uint ptrFirstPage; uint2 pad; };
struct LightPage { uint ptrNextPage; uint numLightsInPage; uint lightIndices[12]; };
cbuffer Frame : register(b0) {
 row_major float4x4 projectionInverse; row_major float4x4 viewMatrix; uint4 grid;
 float2 screen; float nearPlane; float farPlane; uint lightCount; uint pageCapacity; uint2 padding;
};
StructuredBuffer<Light> lights : register(t0);
RWStructuredBuffer<Cluster> clusters : register(u0);
RWStructuredBuffer<LightPage> pages : register(u1);
RWStructuredBuffer<uint> pageCounter : register(u2);
float3 ScreenToView(float2 pixel) {
 float3 ndc=float3(2.0*pixel.x/screen.x-1.0,2.0*(screen.y-pixel.y-1.0)/screen.y-1.0,1.0);
 float4 p=mul(float4(ndc,1),projectionInverse); return p.xyz/p.w;
}
float3 AtZ(float3 ray,float z) { return ray*(z/ray.z); }
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
uint AllocatePage() { uint p; InterlockedAdd(pageCounter[0],1,p); return p<pageCapacity?p:0xffffffff; }
[numthreads(128,1,1)] void CullLights(uint3 dtid:SV_DispatchThreadID) {
 uint total=grid.x*grid.y*grid.z,index=dtid.x; if(index>=total)return;
 Cluster c=clusters[index]; uint page=AllocatePage(); c.numLights=0;c.ptrFirstPage=page;
 if(page==0xffffffff){clusters[index]=c;return;} pages[page].ptrNextPage=0xffffffff; uint inPage=0;
 for(uint i=0;i<lightCount;i++) {
  Light l=lights[i]; float3 center=mul(float4(l.positionWS,1),viewMatrix).xyz;
  if(!Intersects(center,l.radius,c))continue;
  if(inPage>=12){pages[page].numLightsInPage=12;uint old=page;page=AllocatePage();if(page==0xffffffff)break;
   pages[page].ptrNextPage=old;c.ptrFirstPage=page;inPage=0;}
  pages[page].lightIndices[inPage++]=i;c.numLights++;
 }
 if(page!=0xffffffff)pages[page].numLightsInPage=inPage;clusters[index]=c;
}
)";

	struct NativePipeline { winrt::com_ptr<ID3D12PipelineState> state; };

	D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
		return barrier;
	}

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
	const char* after[]{ "cs.gbuffer.ready" };
	const char* before[]{ "cs.deferred-lighting.begin" };
	CSDX12PassDesc pass{};
	pass.structSize = sizeof(pass); pass.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	pass.id = "community-shaders.clustered-lighting.cull"; pass.queuePolicy = CS_DX12_QUEUE_REQUIRE_COMPUTE;
	pass.after = after; pass.afterCount = 1; pass.before = before; pass.beforeCount = 1; pass.execute = &Execute;
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
	ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 1 };
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
		recipe.build = [nativeDevice = device, signature = rootSignature, bytecode = std::move(artifact.binary)]() mutable {
			auto payload = std::make_shared<NativePipeline>();
			D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = signature.get(); desc.CS = { bytecode.data(), bytecode.size() };
			if (FAILED(nativeDevice->CreateComputePipelineState(&desc, IID_PPV_ARGS(payload->state.put())))) return org::services::PipelinePayload{};
			return std::static_pointer_cast<void>(payload);
		};
		auto pipeline = pipelines->Request(std::move(recipe)).get();
		if (!pipeline) { logger::error("[DX12LightCulling] Pipeline creation failed for {}: {}", id, pipeline.error); return false; }
		output = std::static_pointer_cast<NativePipeline>(pipeline.payload)->state;
		return true;
	};
	const bool result = build(L"BuildClusters", "community-shaders.clustered-lighting.build-clusters", clusterPipeline) &&
		build(L"CullLights", "community-shaders.clustered-lighting.cull-lights", cullPipeline);
	pipelines->PublishReady(0);
	return result;
#endif
}

bool DX12LightCulling::EnsureResources(uint32_t width, uint32_t height) noexcept
{
	const uint32_t x = (width + 63) / 64, y = (height + 63) / 64, count = x * y * 32;
	if (count <= clusterCapacity) return true;
	clusterCapacity = count; pageCapacity = count * kPagesPerCluster;
	auto buffer = [&](uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, winrt::com_ptr<ID3D12Resource>& out) {
		D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
		D3D12_RESOURCE_DESC rd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
			{1,0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
		return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(out.put())));
	};
	if (!buffer(uint64_t(count) * 48, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, clusters) ||
		!buffer(uint64_t(pageCapacity) * 56, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pages) ||
		!buffer(4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pageCounter)) return false;
	return true;
}

CSDX12Status DX12LightCulling::Record(const CSDX12ExecutionContext& context) noexcept
{
	const auto& frame = *context.frame;
	auto* commandList = static_cast<ID3D12GraphicsCommandList*>(context.borrowedD3D12GraphicsCommandList);
	if (!EnsureResources(frame.width, frame.height)) return CS_DX12_E_INTERNAL;
	auto snapshot = globals::features::deferredRendering.GetFrameSnapshot();
	if (!context.AllocateUpload || !context.AllocateDescriptors) return CS_DX12_E_UNSUPPORTED_CAPABILITY;
	CSDX12UploadAllocation lightUpload{}, contextUpload{}, constantsUpload{}, zeroUpload{};
	if (context.AllocateUpload(&context, std::max<size_t>(1, snapshot.lights.size()) * sizeof(DeferredRendering::LightData), 16, &lightUpload) != CS_DX12_OK ||
		context.AllocateUpload(&context, std::max<size_t>(1, snapshot.contexts.size()) * sizeof(DeferredRendering::LightingContext), 16, &contextUpload) != CS_DX12_OK ||
		context.AllocateUpload(&context, 256, 256, &constantsUpload) != CS_DX12_OK ||
		context.AllocateUpload(&context, 4, 4, &zeroUpload) != CS_DX12_OK) return CS_DX12_E_INTERNAL;
	std::memcpy(lightUpload.cpuAddress, snapshot.lights.data(), snapshot.lights.size()*sizeof(DeferredRendering::LightData));
	std::memcpy(contextUpload.cpuAddress, snapshot.contexts.data(), snapshot.contexts.size()*sizeof(DeferredRendering::LightingContext));
	*static_cast<uint32_t*>(zeroUpload.cpuAddress) = 0;
	Constants constants{}; constants.projectionInverse=globals::game::frameBufferCached.GetCameraProjInverse(); constants.view=globals::game::frameBufferCached.GetCameraView();
	constants.grid[0]=(frame.width+63)/64;constants.grid[1]=(frame.height+63)/64;constants.grid[2]=32;constants.screen[0]=(float)frame.width;constants.screen[1]=(float)frame.height;
	constants.nearPlane=snapshot.nearPlane;constants.farPlane=snapshot.farPlane;constants.lightCount=(uint32_t)snapshot.lights.size();constants.pageCapacity=pageCapacity;
	std::memcpy(constantsUpload.cpuAddress,&constants,sizeof(constants));
	CSDX12DescriptorAllocation descriptorAllocation{};
	if (context.AllocateDescriptors(&context, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, &descriptorAllocation) != CS_DX12_OK) return CS_DX12_E_INTERNAL;
	auto* descriptors = static_cast<ID3D12DescriptorHeap*>(descriptorAllocation.borrowedNativeHeap);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu{ descriptorAllocation.cpuHandle };
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Buffer.NumElements=std::max<uint32_t>(1, constants.lightCount); srv.Buffer.StructureByteStride=sizeof(DeferredRendering::LightData);
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(lightUpload.borrowedNativeResource),&srv,cpu);
	auto makeUav=[&](ID3D12Resource* resource,uint32_t index,uint32_t elements,uint32_t stride){D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=elements;u.Buffer.StructureByteStride=stride;auto h=cpu;h.ptr+=uint64_t(index)*descriptorAllocation.descriptorSize;device->CreateUnorderedAccessView(resource,nullptr,&u,h);};
	makeUav(clusters.get(),1,clusterCapacity,48); makeUav(pages.get(),2,pageCapacity,56); makeUav(pageCounter.get(),3,1,4);
	auto toCopy=Transition(pageCounter.get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);commandList->ResourceBarrier(1,&toCopy);commandList->CopyBufferRegion(pageCounter.get(),0,static_cast<ID3D12Resource*>(zeroUpload.borrowedNativeResource),0,4);
	auto toUav=Transition(pageCounter.get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);commandList->ResourceBarrier(1,&toUav);
	ID3D12DescriptorHeap* heaps[]{descriptors};commandList->SetDescriptorHeaps(1,heaps);commandList->SetComputeRootSignature(rootSignature.get());commandList->SetComputeRootConstantBufferView(0,constantsUpload.gpuAddress);commandList->SetComputeRootDescriptorTable(1,{descriptorAllocation.gpuHandle});
	commandList->SetPipelineState(clusterPipeline.get());commandList->Dispatch(constants.grid[0],constants.grid[1],constants.grid[2]);auto clusterBarrier=Uav(clusters.get());commandList->ResourceBarrier(1,&clusterBarrier);
	commandList->SetPipelineState(cullPipeline.get());const uint32_t total=constants.grid[0]*constants.grid[1]*constants.grid[2];commandList->Dispatch((total+127)/128,1,1);D3D12_RESOURCE_BARRIER done[]{Uav(clusters.get()),Uav(pages.get()),Uav(pageCounter.get())};commandList->ResourceBarrier(3,done);
	if (dispatchCount++ == 0) logger::info("[DX12LightCulling] ORG dispatched BasicRenderer-style clustered culling: {} clusters, {} lights, {} contexts, {} page capacity",total,constants.lightCount,snapshot.contexts.size(),pageCapacity);
	return CS_DX12_OK;
}

void DX12LightCulling::Shutdown() noexcept
{
	pageCounter=nullptr;pages=nullptr;clusters=nullptr;cullPipeline=nullptr;clusterPipeline=nullptr;rootSignature=nullptr;device=nullptr;
}
