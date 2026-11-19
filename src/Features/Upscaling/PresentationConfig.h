#pragma once

#include <cstdint>
#include <format>
#include <string_view>

// What the swap chain presents with. A narrow header, since the frame variables (RenderGraph/Frame/Variables.h) carry it.
namespace FrameGen
{
	enum class Method : uint8_t
	{
		kNone,
		kFSR,
		kDLSSG,
	};

	/**
	 * @brief What the swap chain presents with. A change replaces the swap chain at a present boundary. It is the frame variable
	 * PresentationConfig, and a frame revision that changes it waits until the swap chain has it.
	 */
	struct PresentationConfig
	{
		Method method = Method::kNone;
		bool hdr = false;
		bool vsync = false;
		bool tearing = true;
		// The frame generation controller's request count. A request with unchanged settings still replaces the swap chain.
		std::uint32_t request = 0;

		friend bool operator==(const PresentationConfig&, const PresentationConfig&) = default;
	};
}

template <>
struct std::formatter<FrameGen::PresentationConfig> : std::formatter<std::string_view>
{
	auto format(const FrameGen::PresentationConfig& a_value, std::format_context& a_context) const
	{
		std::string_view method = "off";
		if (a_value.method == FrameGen::Method::kFSR)
			method = "FSR-FG";
		else if (a_value.method == FrameGen::Method::kDLSSG)
			method = "DLSS-G";
		return std::format_to(a_context.out(), "frame generation {}, {}, vsync {}, tearing {}, request {}", method, a_value.hdr ? "HDR" : "SDR", a_value.vsync,
			a_value.tearing, a_value.request);
	}
};
