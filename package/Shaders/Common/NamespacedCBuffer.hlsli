#ifndef __NAMESPACED_CBUFFER_DEPENDENCY_HLSL__
#define __NAMESPACED_CBUFFER_DEPENDENCY_HLSL__

// FXC scopes the members of a cbuffer declared inside a namespace to that namespace. DXC puts them at
// global scope instead: NS::member no longer resolves, and names can collide with other globals (its
// FXC-compatibility mode, -Gec, is not available for SPIR-V). Members of such cbuffers are therefore
// declared as NSCB(NS, name); under DXC they get the namespace as a prefix, and NSCB_ALIAS /
// NSCB_ARRAY_ALIAS, placed in the namespace after the cbuffer, give them back their names there. FXC
// builds are unchanged.
#if defined(__hlsl_dx_compiler)
#	define NSCB(ns, name) ns##_##name
#	define NSCB_ALIAS(ns, type, name) static const type name = ns##_##name;
#	define NSCB_ARRAY_ALIAS(ns, type, name, count) static const type name[count] = ns##_##name;
#else
#	define NSCB(ns, name) name
#	define NSCB_ALIAS(ns, type, name)
#	define NSCB_ARRAY_ALIAS(ns, type, name, count)
#endif

// CS's shared constant buffers (FrameBuffer b12, SharedData b5, FeatureData b6, Permutation b4) are declared with the
// ORG_SHARED_* macros below. Under FXC, and under DXC by default, they are the cbuffers above. An ORG pass shader
// (CS_ORG_BINDLESS) that defines ORG_SHARED_CONSTANTS reads them from the descriptor heap instead: each becomes a
// struct, read as a ConstantBuffer through the handle its push block holds (a SHARED entry in
// Common/OrgResources.hlsli), and the aliases give its members back their names in the namespace. Shader bodies
// read SharedData::BufferDim and the rest unchanged on every path.
// - ORG_SHARED_CBUFFER(cbuffer, view, register): opens the cbuffer, or the view's struct.
// - ORG_SHARED_PACKOFFSET(c#): a member's packoffset; none in the struct, whose members are in order.
// - ORG_SHARED_VIEW(view): after the closing "};". The ConstantBuffer the aliases read.
// - ORG_SHARED_ALIAS(view, namespace, type, name): as NSCB_ALIAS, reading the view under ORG_SHARED_CONSTANTS.
// - ORG_SHARED_BOOL: a bool member, a uint in the struct. DXC 1.9 emits invalid SPIR-V for a bool read from a
//   ConstantBuffer<T>; the aliases still give it as a bool.
#if defined(__hlsl_dx_compiler) && defined(CS_ORG_BINDLESS) && defined(ORG_SHARED_CONSTANTS)
// A D3D12 program build (ORG_BOUND_PUSH_TABLES, ORGModuleServices' ComputeProgram) reads each block as a bound constant buffer in space
// 11, at the register of the push word holding its handle (the SHARED entry is first, so words 0-3). A bound constant buffer's values
// are free operands on NVIDIA, where a heap-indexed one's cost a register each.
#	if defined(ORG_BOUND_PUSH_TABLES)
#		define ORG_SHARED_HEAP_VIEW(view) ConstantBuffer<OrgSharedValues_##view> OrgSharedView_##view : register(ORG_SHARED_REGISTER_##view, space11);
#		define ORG_SHARED_REGISTER_FrameBuffer b0
#		define ORG_SHARED_REGISTER_SharedData b1
#		define ORG_SHARED_REGISTER_FeatureData b2
#		define ORG_SHARED_REGISTER_Permutation b3
#	else
#		define ORG_SHARED_HEAP_VIEW(view) static ConstantBuffer<OrgSharedValues_##view> OrgSharedView_##view = ResourceDescriptorHeap[OrgHandle_##view];
#	endif
#	define ORG_SHARED_CBUFFER(cb, view, reg) struct OrgSharedValues_##view
#	define ORG_SHARED_PACKOFFSET(c)
#	define ORG_SHARED_BOOL uint
#	if defined(ORG_SHARED_FRAMEBUFFER_AT)
// An ORG pass that draws an engine draw in its place (RenderGraph/Engine/ImageSpace) reads FrameBuffer from the bytes that
// draw had bound at b12, which the engine-constants capture copied: ORG_SHARED_FRAMEBUFFER_AT names the shader's latched
// location (OrgEngineConstantsAt: FrameSrv, FrameOffset). The struct is loaded raw. DXIL keeps its members' row_major in a
// templated raw load, but DXC's SPIR-V ignores it and reads each matrix column by column, so on SPIR-V the aliases transpose
// the matrices back: every backend then reads the matrices the cbuffer would hold.
float4 OrgSharedRawValue(float4 a_value) { return a_value; }
float4x4 OrgSharedRawValue(float4x4 a_value)
{
#		if defined(__spirv__)
	return transpose(a_value);
#		else
	return a_value;
#		endif
}
#		define ORG_SHARED_FIX_FrameBuffer(value) OrgSharedRawValue(value)
#		define ORG_SHARED_VIEW(view) ORG_SHARED_VIEW_##view
#		define ORG_SHARED_VIEW_FrameBuffer                                                                                  \
			static ByteAddressBuffer OrgSharedRing_FrameBuffer = ResourceDescriptorHeap[ORG_SHARED_FRAMEBUFFER_AT.FrameSrv]; \
			static OrgSharedValues_FrameBuffer OrgSharedView_FrameBuffer = OrgSharedRing_FrameBuffer.Load<OrgSharedValues_FrameBuffer>(ORG_SHARED_FRAMEBUFFER_AT.FrameOffset);
#		define ORG_SHARED_VIEW_SharedData ORG_SHARED_HEAP_VIEW(SharedData)
#		define ORG_SHARED_VIEW_FeatureData ORG_SHARED_HEAP_VIEW(FeatureData)
#		define ORG_SHARED_VIEW_Permutation ORG_SHARED_HEAP_VIEW(Permutation)
#	else
#		define ORG_SHARED_VIEW(view) ORG_SHARED_HEAP_VIEW(view)
#		define ORG_SHARED_FIX_FrameBuffer(value) value
#	endif
#	define ORG_SHARED_FIX_SharedData(value) value
#	define ORG_SHARED_FIX_FeatureData(value) value
#	define ORG_SHARED_FIX_Permutation(value) value
#	define ORG_SHARED_ALIAS(view, ns, type, name) static const type name = ORG_SHARED_FIX_##view(OrgSharedView_##view.ns##_##name);
#	define ORG_SHARED_ARRAY_ALIAS(view, ns, type, name, count) static const type name[count] = OrgSharedView_##view.ns##_##name;
#else
#	define ORG_SHARED_CBUFFER(cb, view, reg) cbuffer cb : register(b##reg)
#	define ORG_SHARED_PACKOFFSET(c) : packoffset(c)
#	define ORG_SHARED_BOOL bool
#	define ORG_SHARED_VIEW(view)
#	define ORG_SHARED_ALIAS(view, ns, type, name) NSCB_ALIAS(ns, type, name)
#	define ORG_SHARED_ARRAY_ALIAS(view, ns, type, name, count) NSCB_ARRAY_ALIAS(ns, type, name, count)
#endif

#endif  // __NAMESPACED_CBUFFER_DEPENDENCY_HLSL__
