#pragma once

#include "RenderGraph/Engine/Scaleform/GRendererLayout.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace RE
{
	class GTexture;
}

/**
 * @brief The UI command stream: GFx's calls on the renderer in call order, with what they point at copied.
 *
 * The recorder (Renderer.cpp's hooks, native mode) appends one command per GRenderer call. The replay applies each command to a
 * RendererState, the renderer's state as CS keeps it, and turns the draws into graph draws. A record-only DrawInterface (an
 * unpaused world frame) is replayed whole at the UI point (ReplaySnapshotUi); other frames replay each display at its EndDisplay.
 * A stream is finished at the end of MenuManager::DrawInterface and is immutable from then on; it goes into its frame snapshot
 * (Snapshot/UiCommandStream.h). Each stream starts with a state header, the renderer state the
 * streams before it left, so it replays the same alone, again or after skipped streams.
 */
namespace RenderGraph::Engine::Scaleform
{
	class UiStream;

	/** @brief What the replay needs of a GFx texture, read when a command first used it. */
	struct TextureInfo
	{
		winrt::com_ptr<ID3D11ShaderResourceView> view;  // the raw view, null when the texture has none or it cannot be unwrapped
		bool hasView = false;                           // the texture had a view (null `view` then means a native handle the graph cannot read)
		bool plain = false;                             // a plain GTextureD3D1x
		bool hasData = false;                           // its D3D11 texture exists (+0x70)
		std::uint32_t format = 0;                       // DXGI_FORMAT
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		bool operator==(const TextureInfo& a_other) const
		{
			return view == a_other.view && hasView == a_other.hasView && plain == a_other.plain && hasData == a_other.hasData &&
			       format == a_other.format && width == a_other.width && height == a_other.height;
		}
	};

	/** @brief Reads a_texture's fields now. A null texture gives an empty info. */
	TextureInfo ReadTextureInfo(const RE::GTexture* a_texture);

	/** @brief The render target and depth-stencil a display (or a part of it) draws onto. */
	struct DisplayTargets
	{
		winrt::com_ptr<ID3D11Texture2D> target;
		DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN;  // its RTV's format
		winrt::com_ptr<ID3D11Texture2D> depth;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
	};

	/**
	 * @brief The renderer's state that draws read, as GRendererD3D1x keeps it (the fields in GRendererLayout.h). The setters below
	 * reproduce the D3D1x renderer's pure setters (SE 82916-82935, 82843-82846, 82874-82875) bit for bit.
	 */
	struct RendererState
	{
		bool videoModeSet = false;
		float userMatrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		float matrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		float cxform[8] = { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f };
		// 3D: the view (+0x558) and perspective (+0x598) matrices, their product with the user matrix (+0x5D8) and its dirty flag.
		float view3D[16] = {};
		float perspective3D[16] = {};
		float viewPerspective[16] = {};
		bool viewPerspectiveDirty = false;
		bool world3D = false;  // +0x618 non-null
		float worldMatrix[16] = {};
		const void* vertexData = nullptr;
		std::int32_t vertexCount = 0;
		std::int32_t vertexFormat = 0;  // Layout::VertexFormat
		std::uint32_t vertexStride = 0;
		const void* indexData = nullptr;
		std::int32_t indexCount = 0;
		std::uint32_t indexFormat = 0;  // DXGI_FORMAT_R16_UINT, R32_UINT or 0
		Layout::FillStyle fill{};
		Layout::FillStyle line{};
		// Guest: the D3D1x renderer, whose fields are read live and whose ApplyMatrix draws in 3D. Null in the replay.
		void* renderer = nullptr;
		// The replay's stream and the info index each texture was last named with. Null in guest, where textures are read live.
		const UiStream* stream = nullptr;
		const std::unordered_map<const RE::GTexture*, std::uint32_t>* textures = nullptr;

		void SetVertexData(const void* a_data, std::int32_t a_count, std::int32_t a_format);
		void SetIndexData(const void* a_data, std::int32_t a_count, std::int32_t a_format);
		void SetPerspective3D(const float (&a_matrix)[16]);
		void SetView3D(const float (&a_matrix)[16]);
		void LineStyleDisable();
		void LineStyleColor(std::uint32_t a_color);
		void FillStyleDisable();
		void FillStyleColor(std::uint32_t a_color);
		void FillStyleBitmap(const Layout::FillTexture& a_texture);
		void FillStyleGouraud(std::int32_t a_type, const Layout::FillTexture* a_texture0, const Layout::FillTexture* a_texture1, bool a_texture2);

		/** @brief The renderer's state read from its fields (guest). Textures stay live reads. */
		static void ReadFields(void* a_renderer, RendererState& a_out);
	};

	/** @brief GRenderer::Cxform::TransformColor (SE 0x140f45060): a BGRA colour through a colour transform, clamped to bytes. */
	std::uint32_t TransformColor(const float (&a_cxform)[8], std::uint32_t a_color);

	/** @brief The D3D1x renderer's ApplyMatrix in 3D (SE 82817 / AE 84940): the vertex constants' mvp rows. Updates the cached product. */
	void ApplyMatrix3D(RendererState& a_state, const float (&a_matrix)[6], float (&a_mvp)[4][4]);

	/** @brief A GRenderer call. The arguments' meaning depends on the op; copied data lives in the stream's payload. */
	enum class UiOp : std::uint8_t
	{
		BeginDisplay,  // payload: BeginDisplayArgs; a = the targets' index, b = video mode set
		EndDisplay,
		SetMatrix,      // payload: float[6]
		SetUserMatrix,  // payload: float[6]
		SetCxform,      // payload: float[8]
		PushBlendMode,  // a = mode
		PopBlendMode,
		SetPerspective3D,    // payload: float[16]
		SetView3D,           // payload: float[16]
		SetWorld3D,          // payload: float[16], or none for null
		SetVertexData,       // payload: the array; a = count, b = format
		SetIndexData,        // payload: the array; a = count, b = format
		DrawIndexedTriList,  // a = base, b = min, c = count, d = start, e = triangles
		DrawLineStrip,       // a = base, b = lines
		LineStyleDisable,
		LineStyleColor,  // a = colour
		FillStyleDisable,
		FillStyleColor,    // a = colour
		FillStyleBitmap,   // payload: FillTexture
		FillStyleGouraud,  // payload: FillTexture[2]; a = type, b = bit 0 texture 0, bit 1 texture 1, bit 2 texture 2
		DrawBitmaps,       // payload: BitmapDesc[count] then float[6]; a = count, key = texture
		BeginSubmitMask,   // a = mode
		EndSubmitMask,
		DisableMask,
		DrawBlurRect,         // payload: float[4] source, float[4] dest, BlurFilterParams; key = source
		DrawColorMatrixRect,  // payload: float[4] source, float[4] dest, float[20]; key = source
		PushRenderTarget,     // payload: float[4] frame; key = the target's texture, a = width, b = height
		PopRenderTarget,
		SetDisplayRenderTarget,  // a = the targets' index, or -1 when the new target has no view
		Texture,                 // key = the texture, a = its info's index
	};

	struct UiCommand
	{
		UiOp op{};
		std::int32_t a = 0, b = 0, c = 0, d = 0, e = 0;
		const RE::GTexture* key = nullptr;
		std::uint32_t payload = 0;  // byte offset in UiStream::payload
		std::uint32_t size = 0;
	};

	/**
	 * @brief The renderer state a stream starts with (decision 19): what the replay carries from one stream into the next. Array
	 * pointers are left out, since the recorder copies the arrays again at each stream's first draw. Texgen rows are not kept: a
	 * draw writes the rows of every stage it binds, so the replay starts each stream with them zero.
	 */
	struct StreamHeader
	{
		RendererState state;
		std::uint32_t blendMode = 0;
		std::vector<std::uint32_t> blendStack;
	};

	/** @brief BeginDisplay's arguments. */
	struct BeginDisplayArgs
	{
		std::uint32_t background;
		std::int32_t viewport[14];  // GViewport, 0x38 bytes
		float x0, x1, y0, y1;
	};

	/** @brief One frame's UI commands. Immutable once finished. Holds references to the textures and targets it names. */
	class UiStream
	{
	public:
		std::uint64_t sequence = 0;  // one more per stream, the stream's identity; uploads are applied by it (UiResources)
		StreamHeader header;
		std::vector<UiCommand> commands;
		std::vector<std::byte> payload;
		std::vector<TextureInfo> textures;
		std::vector<DisplayTargets> targets;
		// The recorder's bookkeeping: the info index each texture was last named with.
		std::unordered_map<const RE::GTexture*, std::uint32_t> named;

		/** @brief Appends a_size bytes, 16-byte aligned, and returns their offset. */
		std::uint32_t Append(const void* a_bytes, std::size_t a_size);

		const std::byte* At(const UiCommand& a_command) const { return payload.data() + a_command.payload; }

		/** @brief Appends a command whose payload is a_size bytes at a_bytes. */
		UiCommand& Add(UiOp a_op, const void* a_bytes = nullptr, std::size_t a_size = 0);

		/** @brief Names the texture's current info with a Texture command, when it changed since this stream last named it. */
		void NoteTexture(const RE::GTexture* a_texture);

		/**
		 * @brief The header the next stream starts with: this one's header advanced over its commands by the replay's pure setters
		 * (and the cached 3D product, updated by 3D draws inside displays, as ApplyMatrix3D does).
		 */
		StreamHeader EndState() const;
	};
}
