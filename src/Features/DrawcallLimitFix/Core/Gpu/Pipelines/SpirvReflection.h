#pragma once

#include <ORGModuleServices/ShaderReflection.h>

#include <cstddef>
#include <span>

namespace DCLF
{
	/** @brief OMS's stage reflection, with DXIL bindings numbered as DCLF's SPIR-V builds shift them. */
	struct SpirvReflection : org::services::ShaderReflection
	{
		/** @brief Parses a SPIR-V module, or a DXIL one with DCLF's binding layout. False if it is neither. */
		bool Parse(std::span<const std::byte> a_module);
	};
}
