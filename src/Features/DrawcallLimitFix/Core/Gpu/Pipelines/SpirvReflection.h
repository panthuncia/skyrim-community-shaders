#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF
{
	/** @brief What DCLF needs from a DXC SPIR-V module: inputs by semantic, bindings the code uses, and constant buffer members. */
	struct SpirvReflection
	{
		struct Input
		{
			std::string semantic;  // e.g. "TEXCOORD" from DXC's "in.var.TEXCOORD1"
			std::uint32_t semanticIndex;
			std::uint32_t location;
		};

		enum class BindingKind : std::uint32_t
		{
			ConstantBuffer,
			Texture,
			Sampler,
			StorageBuffer,  // structured or byte address buffers
			Other,
		};

		struct Binding
		{
			std::uint32_t set;
			std::uint32_t binding;
			BindingKind kind;
		};

		/** @brief A constant buffer member. DXC keeps every member of a kept cbuffer, used or not, with the cbuffer's layout. */
		struct BlockMember
		{
			std::string block;
			std::string name;
			std::uint32_t offset;  // bytes from the block's start
		};

		std::vector<Input> inputs;
		std::vector<Binding> bindings;
		std::vector<BlockMember> blockMembers;
		// The stage outputs' locations, a bit each.
		std::uint32_t outputLocations = 0;

		/** @brief Parses a module. False if it is not SPIR-V. DXIL is read through DxilStages::Reflect. */
		bool Parse(std::span<const std::byte> a_spirv);
	};
}
