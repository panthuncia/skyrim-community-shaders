#pragma once

// The Effects feature draws BSEffectShaderProperty objects through DCLF (dclf-architecture.md, "The Effect shader in the forward epoch",
// "Opaque effects"). Blended meshes go in the forward epoch's sort, decals in the decal groups, and opaque ones through decal group 1.
//
// The Effect port itself is split across files. EffectMaterial.cpp holds the material port, EffectDescriptors.cpp the descriptors
// and variants, EffectPrograms.cpp the shader programs, EffectFrame.cpp the frame inputs, and EffectWrites.cpp the write hooks.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <utility>

namespace DCLF::Features::Effects
{
	Api::Feature Feature();

	void RegisterReport(Api::InstallContext& a_context);

	void RegisterPort(Api::InstallContext& a_context);
	void CaptureEffectProperty(const Api::MaterialPortCapture& a_capture);
	void SampleEffectShading(Api::MaterialPortShading& a_shading);
	const void* EffectEmissive(const RE::BSShaderProperty& a_property);
	void RegisterPrograms(Api::InstallContext& a_context);
	void RegisterFrame(Api::InstallContext& a_context);
	void InstallWrites();
}
