#pragma once

#include <cstdint>
#include <stdexcept>

namespace DCLF::Portals
{
	/** @brief Vulkan portable two-dimensional dispatch, with the shader's fixed 65535-group row pitch. */
	struct BatchDispatch
	{
		std::uint32_t x = 0, y = 1;
	};
	inline BatchDispatch DispatchFor(std::uint32_t a_elements, std::uint32_t a_width)
	{
		if (!a_width)
			throw std::logic_error("portal batch: zero workgroup width");
		const auto groups = (std::uint64_t(a_elements) + a_width - 1) / a_width;
		if (groups > 65535ull * 65535ull)
			throw std::overflow_error("portal batch: dispatch range exhausted");
		return { static_cast<std::uint32_t>(groups < 65535 ? groups : 65535),
			static_cast<std::uint32_t>(groups ? (groups + 65534) / 65535 : 1) };
	}
}
