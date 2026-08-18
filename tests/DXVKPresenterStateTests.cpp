#include <catch2/catch_test_macros.hpp>

#include "Features/Upscaling/DXVKPresenterState.h"

TEST_CASE("DXVK presenter encoding is classified from requested and effective color spaces")
{
	using State = DXVKPresenterState::SurfaceState;
	using Encoding = DXVKPresenterState::Encoding;

	CHECK(DXVKPresenterState::Classify(State{}) == Encoding::kUnknown);
	CHECK(DXVKPresenterState::Classify({ 1, VK_FORMAT_B8G8R8A8_UNORM,
		VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }) == Encoding::kSDR);
	CHECK(DXVKPresenterState::Classify({ 2, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		VK_COLOR_SPACE_HDR10_ST2084_EXT, VK_COLOR_SPACE_HDR10_ST2084_EXT }) == Encoding::kHDR10);
	CHECK(DXVKPresenterState::Classify({ 3, VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_COLOR_SPACE_HDR10_ST2084_EXT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT }) ==
		Encoding::kHDR10ScRGBFallback);
}

TEST_CASE("DXVK presenter matching accepts only the requested output family")
{
	using State = DXVKPresenterState::SurfaceState;
	const State sdr{ 1, VK_FORMAT_B8G8R8A8_UNORM,
		VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
	const State hdrFallback{ 2, VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_COLOR_SPACE_HDR10_ST2084_EXT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT };

	CHECK(DXVKPresenterState::Matches(sdr, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));
	CHECK_FALSE(DXVKPresenterState::Matches(sdr, VK_COLOR_SPACE_HDR10_ST2084_EXT));
	CHECK(DXVKPresenterState::Matches(hdrFallback, VK_COLOR_SPACE_HDR10_ST2084_EXT));
}
