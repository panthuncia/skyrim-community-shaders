// DCLF's binding layout for OMS's stage reflection.

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"

namespace DCLF
{
	bool SpirvReflection::Parse(std::span<const std::byte> a_module)
	{
		// DXIL keeps its HLSL registers. They are shifted as the SPIR-V builds shift them. The pass's push block is push
		// data; the draw's (kDrawPushBinding) is kept for the checks.
		org::services::DxilBindingLayout layout;
		layout.constantBufferShift = kBindingShiftB;
		layout.textureShift = kBindingShiftT;
		layout.samplerShift = kBindingShiftS;
		layout.unorderedShift = kBindingShiftU;
		layout.pushConstantRegister = kFramePushBinding;
		return ShaderReflection::Parse(a_module, layout);
	}
}
