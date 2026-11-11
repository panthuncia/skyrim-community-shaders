#pragma once

#include <cstddef>
#include <cstdint>

namespace RE
{
	class GTexture;
}

/**
 * @brief The fields of GFx 3.3's GRendererD3D1x (the engine's Scaleform renderer, 0x8D8 bytes) that the ORG path reads, and the
 * structures behind them. The layout is the same on SE 1.5.97 and AE 1.6.1170: checked on the pure setters (SetMatrix SE 82922 /
 * AE 85038, SetUserMatrix 82929 / 85045, SetCxform 82916 / 85031, SetVertexData 82930 / 85048, SetIndexData 82921 / 85037) and
 * on DrawBitmaps (82835 / 84956). docs/development/p5/scaleform.md, "The renderer object".
 */
namespace RenderGraph::Engine::Scaleform::Layout
{
	inline constexpr std::size_t kVideoModeSet = 0xC0;     // std::uint8_t: every draw is skipped without it
	inline constexpr std::size_t kVertexConstants = 0x1C0;  // float[32]: mvp[4] then texgen[4], the VS cbuffer's CPU copy
	inline constexpr std::size_t kTexgen = 0x200;           // float[16]: texgen[4]
	inline constexpr std::size_t kDepthStencilStates = 0x468;  // ID3D11DepthStencilState*[5]: Stencil's order (off, clear, increment, decrement, test)
	inline constexpr std::size_t kDevice = 0x490;              // ID3D11Device*: the device the renderer and its textures create with
	inline constexpr std::size_t kContext = 0x498;             // ID3D11DeviceContext*: the context the renderer draws with
	inline constexpr std::size_t kMaskSubmitting = 0x4A0;      // std::uint8_t: inside BeginSubmitMask to EndSubmitMask
	inline constexpr std::size_t kMaskLevel = 0x4A4;           // std::int32_t: the stencil level (the test's reference)
	inline constexpr std::size_t kDepthStencilState = 0x4A8;   // ID3D11DepthStencilState*: the one bound now (PopRenderTarget binds it again)
	inline constexpr std::size_t kUserMatrix = 0x4B8;       // float[6]
	inline constexpr std::size_t kMatrix = 0x51C;           // float[6]
	inline constexpr std::size_t kCxform = 0x534;           // float[8]
	inline constexpr std::size_t kWorld3D = 0x618;          // const float*: null in 2D
	inline constexpr std::size_t kVertexData = 0x6A0;       // const void*
	inline constexpr std::size_t kIndexData = 0x6A8;        // const void*
	inline constexpr std::size_t kVertexFormat = 0x6B0;     // std::int32_t: VertexFormat below
	inline constexpr std::size_t kVertexStride = 0x6B4;     // std::uint32_t
	inline constexpr std::size_t kIndexFormat = 0x6B8;      // std::uint32_t: DXGI_FORMAT_R16_UINT, R32_UINT or 0
	inline constexpr std::size_t kVertexCount = 0x6BC;      // std::int32_t
	inline constexpr std::size_t kIndexCount = 0x6C0;       // std::int32_t
	inline constexpr std::size_t kBlendMode = 0x720;        // std::uint32_t: BlendType below
	// The current render target (a GRenderTargetD3D1x*): SetDisplayRenderTarget's, or the top of the stack Push/PopRenderTarget
	// keep. PopRenderTarget binds its views with OMSetRenderTargets(1, target + 0x50, *(target + 0x58)) (SE 0x140f61270, AE
	// 0x14104be10, the same offsets).
	inline constexpr std::size_t kRenderTarget = 0x740;
	inline constexpr std::size_t kRenderTargetRtv = 0x50;  // in the GRenderTargetD3D1x: ID3D11RenderTargetView*
	inline constexpr std::size_t kRenderTargetDsv = 0x58;  // ID3D11DepthStencilView*
	// The blend stack, a GArray<std::uint32_t> at +0x728 (data, then this size, then the capacity): PushBlendMode appends the mode
	// before, PopBlendMode (a no-op when empty) removes the last. Checked on both (SE 0x140f61770 / 0x140f61120, AE 0x14104c300 /
	// 0x14104bd10, vtable slots 17 and 18).
	inline constexpr std::size_t kBlendStackSize = 0x730;   // std::uint64_t
	inline constexpr std::size_t kFillStyle = 0x7D8;        // FillStyle
	inline constexpr std::size_t kLineStyle = 0x858;        // FillStyle (mode and colour only)

	/** @brief GRenderer::VertexFormat as this build numbers it (sequential, not CommonLib's flags). */
	enum VertexFormat : std::int32_t
	{
		kVertexNone = 0,
		kVertexXY16i = 1,
		kVertexXY32f = 2,
		kVertexXY16iC32 = 3,
		kVertexXY16iCF32 = 4,
	};

	/**
	 * @brief SetIndexData's format argument (GRenderer::IndexFormat: 0 none, 1 16-bit, 2 32-bit) for the DXGI format it stores at
	 * kIndexFormat (SetIndexData SE 82921 / AE 85037). SetVertexData stores its format argument as it is.
	 */
	inline constexpr std::int32_t IndexFormatOf(std::uint32_t a_dxgiFormat)
	{
		return a_dxgiFormat == 0x39 ? 1 : (a_dxgiFormat == 0x2A ? 2 : 0);  // DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT
	}

	/** @brief GRenderer::FillTexture. */
	struct FillTexture
	{
		RE::GTexture* texture;
		float matrix[6];  // GMatrix2D: m00 m01 m02 / m10 m11 m12
		std::uint32_t wrapMode;    // 0 repeat, 1 clamp
		std::uint32_t sampleMode;  // 0 point, 1 linear
	};
	static_assert(sizeof(FillTexture) == 0x28);

	/** @brief The renderer's fill (and line) style. */
	struct FillStyle
	{
		std::int32_t mode;          // 0 off, 1 colour, 2 bitmap, 3 gouraud
		std::int32_t gouraudType;   // GRenderer::GouraudFillType
		std::uint32_t color;        // BGRA, the colour transform applied
		float cxform[8];            // the colour transform when the style was set
		std::uint8_t cxformAdds;    // whether an add exceeds 1
		std::uint8_t padding[3];
		FillTexture textures[2];
	};
	static_assert(sizeof(FillStyle) == 0x80);
	static_assert(offsetof(FillStyle, cxform) == 0xC && offsetof(FillStyle, textures) == 0x30);

	/** @brief GTextureD3D1x's fields. */
	inline constexpr std::size_t kTextureWidth = 0x38;     // std::uint32_t: its D3D11 description's width
	inline constexpr std::size_t kTextureHeight = 0x3C;    // std::uint32_t: and height
	inline constexpr std::size_t kTextureOwner = 0x68;     // the GRendererD3D1x that made it
	inline constexpr std::size_t kTextureResource = 0x70;  // ID3D11Texture2D*: null when the data is lost
	inline constexpr std::size_t kTextureView = 0x78;      // ID3D11ShaderResourceView*
	inline constexpr std::size_t kTextureFormat = 0x48;    // DXGI_FORMAT

	/** @brief The renderer's members and the pure setters' fields, read at a field offset. */
	template <class T>
	T& Field(void* a_renderer, std::size_t a_offset)
	{
		return *reinterpret_cast<T*>(static_cast<std::byte*>(a_renderer) + a_offset);
	}
}
