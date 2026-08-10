struct Light {
 float3 color; float fade; float radius; float invRadius; float fadeZone; float sizeBias;
 float3 positionWS; uint positionPad; uint4 roomFlags; uint lightFlags; uint shadowMaskIndex; uint2 pad;
};
struct Cluster { float4 minPoint; float4 maxPoint; uint numLights; uint ptrFirstPage; uint2 pad; };
struct LightPage { uint ptrNextPage; uint numLightsInPage; uint lightIndices[12]; };
cbuffer Frame : register(b0) {
 row_major float4x4 projectionInverse; row_major float4x4 viewMatrix; uint4 grid;
 float2 screen; float nearPlane; float farPlane; uint lightCount; uint contextCount; uint pageCapacity; uint materialCount;
 uint lightsIndex; uint contextsIndex; uint materialsIndex; uint clustersIndex; uint pagesIndex; uint pageCounterIndex; uint diagnosticsIndex;
};
float3 ScreenToView(float2 pixel) {
 float3 ndc=float3(2.0*pixel.x/screen.x-1.0,2.0*(screen.y-pixel.y-1.0)/screen.y-1.0,1.0);
 float4 p=mul(projectionInverse,float4(ndc,1)); return p.xyz/p.w;
}
float3 AtZ(float3 ray,float z) { return ray*(z/ray.z); }
[numthreads(1,1,1)] void ClearCounters(uint3 id:SV_DispatchThreadID) {
 RWStructuredBuffer<uint> pageCounter=ResourceDescriptorHeap[pageCounterIndex];
 RWStructuredBuffer<uint> diagnostics=ResourceDescriptorHeap[diagnosticsIndex];
 pageCounter[0]=0; diagnostics[0]=0; diagnostics[1]=0; diagnostics[2]=0; diagnostics[3]=0;
}
[numthreads(1,1,1)] void BuildClusters(uint3 id:SV_GroupID) {
 RWStructuredBuffer<Cluster> clusters=ResourceDescriptorHeap[clustersIndex];
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
[numthreads(128,1,1)] void CullLights(uint3 dtid:SV_DispatchThreadID) {
 StructuredBuffer<Light> lights=ResourceDescriptorHeap[lightsIndex];
 RWStructuredBuffer<Cluster> clusters=ResourceDescriptorHeap[clustersIndex];
 RWStructuredBuffer<LightPage> pages=ResourceDescriptorHeap[pagesIndex];
 RWStructuredBuffer<uint> pageCounter=ResourceDescriptorHeap[pageCounterIndex];
 RWStructuredBuffer<uint> diagnostics=ResourceDescriptorHeap[diagnosticsIndex];
 uint total=grid.x*grid.y*grid.z,index=dtid.x; if(index>=total)return;
 Cluster c=clusters[index]; uint page; InterlockedAdd(pageCounter[0],1,page);
 if(page>=pageCapacity)InterlockedAdd(diagnostics[0],1); page=page<pageCapacity?page:0xffffffff;
 c.numLights=0;c.ptrFirstPage=page;
 if(page==0xffffffff){clusters[index]=c;return;} pages[page].ptrNextPage=0xffffffff; uint inPage=0;
 for(uint i=0;i<lightCount;i++) {
  Light l=lights[i]; float3 center=mul(viewMatrix,float4(l.positionWS,1)).xyz;
  if(!Intersects(center,l.radius,c))continue;
  if(inPage>=12){pages[page].numLightsInPage=12;uint old=page;InterlockedAdd(pageCounter[0],1,page);
   if(page>=pageCapacity){InterlockedAdd(diagnostics[0],1);page=0xffffffff;}if(page==0xffffffff)break;
   pages[page].ptrNextPage=old;c.ptrFirstPage=page;inPage=0;}
  pages[page].lightIndices[inPage++]=i;c.numLights++;
 }
 if(page!=0xffffffff)pages[page].numLightsInPage=inPage;InterlockedMax(diagnostics[1],c.numLights);clusters[index]=c;
}
