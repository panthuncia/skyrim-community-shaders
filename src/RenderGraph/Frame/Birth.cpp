#include "RenderGraph/Frame/Birth.h"

#include <DirectXPackedVector.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RenderGraph::Frame
{
	namespace
	{
		template <class T>
		void BirthPut(std::array<std::byte, 16>& a_out, std::uint32_t a_index, T a_value)
		{
			std::memcpy(a_out.data() + a_index * sizeof(T), &a_value, sizeof(T));
		}

		std::uint32_t BirthUnorm(float a_value, std::uint32_t a_bits)
		{
			const float max = static_cast<float>((1u << a_bits) - 1u);
			return static_cast<std::uint32_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * max));
		}

		std::int32_t BirthSnorm(float a_value, std::uint32_t a_bits)
		{
			const float max = static_cast<float>((1u << (a_bits - 1)) - 1u);
			return static_cast<std::int32_t>(std::lround(std::clamp(a_value, -1.0f, 1.0f) * max));
		}

		float BirthSrgbEncode(float a_linear)
		{
			const float c = std::clamp(a_linear, 0.0f, 1.0f);
			return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
		}
	}

	std::uint32_t EncodeTexel(std::uint32_t a_format, const std::array<float, 4>& a_value, std::array<std::byte, 16>& a_out)
	{
		using namespace DirectX::PackedVector;
		a_out = {};
		const auto& v = a_value;
		switch (static_cast<DXGI_FORMAT>(a_format)) {
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, v[i]);
			return 16;
		case DXGI_FORMAT_R32G32B32A32_UINT:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, static_cast<std::uint32_t>(v[i]));
			return 16;
		case DXGI_FORMAT_R32G32B32_FLOAT:
			for (std::uint32_t i = 0; i < 3; ++i)
				BirthPut(a_out, i, v[i]);
			return 12;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, XMConvertFloatToHalf(v[i]));
			return 8;
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, static_cast<std::uint16_t>(BirthUnorm(v[i], 16)));
			return 8;
		case DXGI_FORMAT_R16G16B16A16_SNORM:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, static_cast<std::int16_t>(BirthSnorm(v[i], 16)));
			return 8;
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
			for (std::uint32_t i = 0; i < 2; ++i)
				BirthPut(a_out, i, v[i]);
			return 8;
		case DXGI_FORMAT_R32G32_UINT:
			for (std::uint32_t i = 0; i < 2; ++i)
				BirthPut(a_out, i, static_cast<std::uint32_t>(v[i]));
			return 8;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			BirthPut(a_out, 0, BirthUnorm(v[0], 10) | (BirthUnorm(v[1], 10) << 10) | (BirthUnorm(v[2], 10) << 20) | (BirthUnorm(v[3], 2) << 30));
			return 4;
		case DXGI_FORMAT_R11G11B10_FLOAT:
			{
				XMFLOAT3PK packed;
				const DirectX::XMFLOAT3 rgb{ v[0], v[1], v[2] };
				XMStoreFloat3PK(&packed, DirectX::XMLoadFloat3(&rgb));
				BirthPut(a_out, 0, packed.v);
				return 4;
			}
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, static_cast<std::uint8_t>(BirthUnorm(v[i], 8)));
			return 4;
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, static_cast<std::uint8_t>(BirthUnorm(i < 3 ? BirthSrgbEncode(v[i]) : v[i], 8)));
			return 4;
		case DXGI_FORMAT_R8G8B8A8_SNORM:
			for (std::uint32_t i = 0; i < 4; ++i)
				BirthPut(a_out, i, static_cast<std::int8_t>(BirthSnorm(v[i], 8)));
			return 4;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			BirthPut(a_out, 0, static_cast<std::uint8_t>(BirthUnorm(v[2], 8)));
			BirthPut(a_out, 1, static_cast<std::uint8_t>(BirthUnorm(v[1], 8)));
			BirthPut(a_out, 2, static_cast<std::uint8_t>(BirthUnorm(v[0], 8)));
			BirthPut(a_out, 3, static_cast<std::uint8_t>(BirthUnorm(v[3], 8)));
			return 4;
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
			for (std::uint32_t i = 0; i < 2; ++i)
				BirthPut(a_out, i, XMConvertFloatToHalf(v[i]));
			return 4;
		case DXGI_FORMAT_R16G16_UNORM:
			for (std::uint32_t i = 0; i < 2; ++i)
				BirthPut(a_out, i, static_cast<std::uint16_t>(BirthUnorm(v[i], 16)));
			return 4;
		case DXGI_FORMAT_R16G16_SNORM:
			for (std::uint32_t i = 0; i < 2; ++i)
				BirthPut(a_out, i, static_cast<std::int16_t>(BirthSnorm(v[i], 16)));
			return 4;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
			BirthPut(a_out, 0, v[0]);
			return 4;
		case DXGI_FORMAT_R32_UINT:
			BirthPut(a_out, 0, static_cast<std::uint32_t>(v[0]));
			return 4;
		case DXGI_FORMAT_R8G8_TYPELESS:
		case DXGI_FORMAT_R8G8_UNORM:
			for (std::uint32_t i = 0; i < 2; ++i)
				BirthPut(a_out, i, static_cast<std::uint8_t>(BirthUnorm(v[i], 8)));
			return 2;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
			BirthPut(a_out, 0, XMConvertFloatToHalf(v[0]));
			return 2;
		case DXGI_FORMAT_R16_UNORM:
			BirthPut(a_out, 0, static_cast<std::uint16_t>(BirthUnorm(v[0], 16)));
			return 2;
		case DXGI_FORMAT_R16_UINT:
			BirthPut(a_out, 0, static_cast<std::uint16_t>(v[0]));
			return 2;
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
			BirthPut(a_out, 0, static_cast<std::uint8_t>(BirthUnorm(v[0], 8)));
			return 1;
		case DXGI_FORMAT_R8_UINT:
			BirthPut(a_out, 0, static_cast<std::uint8_t>(v[0]));
			return 1;
		default:
			return 0;
		}
	}
}
