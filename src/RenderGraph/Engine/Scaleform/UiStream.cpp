#include "RenderGraph/Engine/Scaleform/UiStream.h"

#include "RenderGraph/Engine/Scaleform/UiResources.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"

#include <algorithm>
#include <cstring>

namespace RenderGraph::Engine::Scaleform
{
	namespace
	{
		using Layout::Field;

		// The renderer's 255.0 (DAT_1415535d8) and 1.0 (DAT_1415232d8).
		constexpr float kByteMax = 255.0f;
		constexpr float kAddLimit = 1.0f;

		/** @brief GMatrix3D from a GMatrix2D (SE 0x140ed8630). */
		void From2D(const float (&a_m)[6], float (&a_out)[16])
		{
			a_out[0] = a_m[0];
			a_out[4] = a_m[1];
			a_out[8] = 0.0f;
			a_out[12] = a_m[2];
			a_out[1] = a_m[3];
			a_out[5] = a_m[4];
			a_out[9] = 0.0f;
			a_out[13] = a_m[5];
			a_out[2] = a_out[3] = 0.0f;
			a_out[6] = a_out[7] = 0.0f;
			a_out[10] = 1.0f;
			a_out[11] = 0.0f;
			a_out[14] = 0.0f;
			a_out[15] = 1.0f;
		}

		/** @brief a_out = a_a * a_b (SE 0x140ed9430), each sum in the renderer's order so results match bit for bit. */
		void Multiply(float (&a_out)[16], const float (&a)[16], const float (&b)[16])
		{
			a_out[0] = a[0] * b[0] + b[4] * a[1] + a[2] * b[8] + a[3] * b[12];
			a_out[1] = b[5] * a[1] + b[1] * a[0] + b[9] * a[2] + a[3] * b[13];
			a_out[2] = b[6] * a[1] + b[2] * a[0] + a[2] * b[10] + a[3] * b[14];
			a_out[3] = a[0] * b[3] + b[7] * a[1] + a[2] * b[11] + b[15] * a[3];
			a_out[4] = a[5] * b[4] + a[4] * b[0] + a[6] * b[8] + a[7] * b[12];
			a_out[5] = b[5] * a[5] + b[1] * a[4] + b[9] * a[6] + a[7] * b[13];
			a_out[6] = b[6] * a[5] + b[2] * a[4] + a[6] * b[10] + a[7] * b[14];
			a_out[7] = a[5] * b[7] + a[4] * b[3] + a[6] * b[11] + a[7] * b[15];
			a_out[8] = a[9] * b[4] + a[8] * b[0] + a[10] * b[8] + a[11] * b[12];
			a_out[9] = b[5] * a[9] + b[1] * a[8] + b[9] * a[10] + a[11] * b[13];
			a_out[10] = b[6] * a[9] + b[2] * a[8] + a[10] * b[10] + a[11] * b[14];
			a_out[11] = a[9] * b[7] + a[8] * b[3] + a[10] * b[11] + a[11] * b[15];
			a_out[12] = a[13] * b[4] + a[12] * b[0] + b[8] * a[14] + a[15] * b[12];
			a_out[13] = a[12] * b[1] + b[5] * a[13] + b[9] * a[14] + a[15] * b[13];
			a_out[14] = b[6] * a[13] + a[12] * b[2] + a[14] * b[10] + a[15] * b[14];
			a_out[15] = a[12] * b[3] + a[13] * b[7] + b[11] * a[14] + a[15] * b[15];
		}

		/** @brief The fill style's colour transform and whether an add exceeds 1 (SE 0x140f63350). */
		void SetStyleCxform(Layout::FillStyle& a_style, const float (&a_cxform)[8])
		{
			std::memcpy(a_style.cxform, a_cxform, sizeof(a_style.cxform));
			a_style.cxformAdds = (a_style.cxform[1] > kAddLimit || a_style.cxform[3] > kAddLimit || a_style.cxform[5] > kAddLimit || a_style.cxform[7] > kAddLimit) ? 1 : 0;
		}

		/** @brief FillStyle::SetBitmap (SE 0x140f632c0). */
		void SetStyleBitmap(Layout::FillStyle& a_style, const Layout::FillTexture& a_texture, const float (&a_cxform)[8])
		{
			a_style.mode = 2;
			a_style.textures[0] = a_texture;
			a_style.color = 0xFFFFFFFFu;
			SetStyleCxform(a_style, a_cxform);
		}

		std::uint8_t Channel(std::uint8_t a_value, float a_mul, float a_add)
		{
			float v = static_cast<float>(a_value) * a_mul;
			v += a_add;
			if (!(v < kByteMax))
				v = kByteMax;
			else if (v < 0.0f)
				v = 0.0f;
			return static_cast<std::uint8_t>(static_cast<std::int64_t>(v));
		}
	}

	std::uint32_t TransformColor(const float (&a_cxform)[8], std::uint32_t a_color)
	{
		std::uint8_t in[4];
		std::memcpy(in, &a_color, sizeof(in));
		std::uint8_t out[4];
		out[3] = Channel(in[3], a_cxform[6], a_cxform[7]);  // alpha
		out[0] = Channel(in[0], a_cxform[4], a_cxform[5]);  // blue
		out[1] = Channel(in[1], a_cxform[2], a_cxform[3]);  // green
		out[2] = Channel(in[2], a_cxform[0], a_cxform[1]);  // red
		std::uint32_t result;
		std::memcpy(&result, out, sizeof(result));
		return result;
	}

	void RefreshViewPerspective(RendererState& a_state)
	{
		if (!a_state.viewPerspectiveDirty)
			return;
		a_state.viewPerspectiveDirty = false;
		float temp[16];
		From2D(a_state.userMatrix, temp);
		Multiply(a_state.viewPerspective, temp, a_state.view3D);
		std::memcpy(temp, a_state.viewPerspective, sizeof(temp));
		Multiply(a_state.viewPerspective, temp, a_state.perspective3D);
	}

	void BlendStackPush(std::uint32_t& a_mode, std::vector<std::uint32_t>& a_stack, std::uint32_t a_push)
	{
		a_stack.push_back(a_mode);
		if (a_push > 2)
			a_mode = a_push;
	}

	void BlendStackPop(std::uint32_t& a_mode, std::vector<std::uint32_t>& a_stack)
	{
		if (a_stack.empty())
			return;
		std::uint32_t mode = 0;
		for (auto it = a_stack.rbegin(); it != a_stack.rend(); ++it)
			if (*it > 2) {
				mode = *it;
				break;
			}
		a_stack.pop_back();
		a_mode = mode;
	}

	void ApplyMatrix3D(RendererState& a_state, const float (&a_matrix)[6], float (&a_mvp)[4][4])
	{
		RefreshViewPerspective(a_state);
		float temp[16];
		float local[16];
		From2D(a_matrix, temp);
		Multiply(local, temp, a_state.worldMatrix);
		std::memcpy(temp, local, sizeof(temp));
		Multiply(local, temp, a_state.viewPerspective);
		// SE 0x140eda2f0 transposes it into the vertex constants' rows.
		for (int row = 0; row < 4; ++row)
			for (int column = 0; column < 4; ++column)
				a_mvp[row][column] = local[column * 4 + row];
	}

	void RendererState::SetVertexData(const void* a_data, std::int32_t a_count, std::int32_t a_format)
	{
		vertexData = a_data;
		vertexFormat = a_format;
		vertexCount = a_count;
		vertexStride = a_format == 0 ? 0 : a_format == 3 ? 8 :
		                               a_format == 4     ? 12 :
		                                                   4;
	}

	void RendererState::SetIndexData(const void* a_data, std::int32_t a_count, std::int32_t a_format)
	{
		indexData = a_data;
		indexCount = a_count;
		indexFormat = a_format == 1 ? DXGI_FORMAT_R16_UINT : a_format == 2 ? DXGI_FORMAT_R32_UINT :
		                                                                     0;
	}

	void RendererState::SetPerspective3D(const float (&a_matrix)[16])
	{
		std::memcpy(perspective3D, a_matrix, sizeof(perspective3D));
		viewPerspectiveDirty = true;
	}

	void RendererState::SetView3D(const float (&a_matrix)[16])
	{
		std::memcpy(view3D, a_matrix, sizeof(view3D));
		viewPerspectiveDirty = true;
	}

	void RendererState::LineStyleDisable()
	{
		line.mode = 0;
		line.textures[0].texture = nullptr;
	}

	void RendererState::LineStyleColor(std::uint32_t a_color)
	{
		line.mode = 1;
		line.color = TransformColor(cxform, a_color);
	}

	void RendererState::FillStyleDisable()
	{
		fill.mode = 0;
		fill.textures[0].texture = nullptr;
	}

	void RendererState::FillStyleColor(std::uint32_t a_color)
	{
		fill.mode = 1;
		fill.color = TransformColor(cxform, a_color);
	}

	void RendererState::FillStyleBitmap(const Layout::FillTexture& a_texture)
	{
		SetStyleBitmap(fill, a_texture, cxform);
	}

	void RendererState::FillStyleGouraud(std::int32_t a_type, const Layout::FillTexture* a_texture0, const Layout::FillTexture* a_texture1, bool a_texture2)
	{
		// SE 0x140f63580. Without any texture only the colour transform changes. Stage 1 keeps its texture unless one is given.
		if (!a_texture0 && !a_texture1 && !a_texture2) {
			SetStyleCxform(fill, cxform);
			fill.textures[0].texture = nullptr;
		} else {
			SetStyleBitmap(fill, a_texture0 ? *a_texture0 : *a_texture1, cxform);
			if (a_texture1)
				fill.textures[1] = *a_texture1;
		}
		fill.gouraudType = a_type;
		fill.mode = 3;
	}

	void RendererState::ReadFields(void* a_renderer, RendererState& a_out)
	{
		a_out.renderer = a_renderer;
		a_out.textures = nullptr;
		a_out.videoModeSet = Field<std::uint8_t>(a_renderer, Layout::kVideoModeSet) != 0;
		std::memcpy(a_out.userMatrix, &Field<float>(a_renderer, Layout::kUserMatrix), sizeof(a_out.userMatrix));
		std::memcpy(a_out.matrix, &Field<float>(a_renderer, Layout::kMatrix), sizeof(a_out.matrix));
		std::memcpy(a_out.cxform, &Field<float>(a_renderer, Layout::kCxform), sizeof(a_out.cxform));
		a_out.world3D = Field<const float*>(a_renderer, Layout::kWorld3D) != nullptr;
		a_out.vertexData = Field<const void*>(a_renderer, Layout::kVertexData);
		a_out.vertexCount = Field<std::int32_t>(a_renderer, Layout::kVertexCount);
		a_out.vertexFormat = Field<std::int32_t>(a_renderer, Layout::kVertexFormat);
		a_out.vertexStride = Field<std::uint32_t>(a_renderer, Layout::kVertexStride);
		a_out.indexData = Field<const void*>(a_renderer, Layout::kIndexData);
		a_out.indexCount = Field<std::int32_t>(a_renderer, Layout::kIndexCount);
		a_out.indexFormat = Field<std::uint32_t>(a_renderer, Layout::kIndexFormat);
		a_out.fill = Field<Layout::FillStyle>(a_renderer, Layout::kFillStyle);
		a_out.line = Field<Layout::FillStyle>(a_renderer, Layout::kLineStyle);
	}

	TextureInfo ReadTextureInfo(const RE::GTexture* a_texture)
	{
		TextureInfo info;
		if (!a_texture)
			return info;
		// CS's own textures (native) keep GTextureD3D1x's fields.
		static const std::uintptr_t vtable = REL::RelocationID(292188, 243114).address();
		info.plain = *reinterpret_cast<const std::uintptr_t*>(a_texture) == vtable || UiResources::IsTexture(a_texture);
		if (!info.plain)
			return info;
		auto* texture = const_cast<RE::GTexture*>(a_texture);
		info.hasData = Field<void*>(texture, Layout::kTextureResource) != nullptr;
		info.format = Field<std::uint32_t>(texture, Layout::kTextureFormat);
		info.width = Field<std::uint32_t>(texture, Layout::kTextureWidth);
		info.height = Field<std::uint32_t>(texture, Layout::kTextureHeight);
		auto* view = Field<ID3D11ShaderResourceView*>(texture, Layout::kTextureView);
		info.hasView = view != nullptr;
		if (view)
			info.view.copy_from(Tripwire::Unwrap(view));
		return info;
	}

	std::uint32_t UiStream::Append(const void* a_bytes, std::size_t a_size)
	{
		const std::size_t offset = (payload.size() + 15) / 16 * 16;
		payload.resize(offset + a_size);
		if (a_size)
			std::memcpy(payload.data() + offset, a_bytes, a_size);
		return static_cast<std::uint32_t>(offset);
	}

	UiCommand& UiStream::Add(UiOp a_op, const void* a_bytes, std::size_t a_size)
	{
		UiCommand command{ .op = a_op };
		if (a_size) {
			command.payload = Append(a_bytes, a_size);
			command.size = static_cast<std::uint32_t>(a_size);
		}
		commands.push_back(command);
		return commands.back();
	}

	void UiStream::NoteTexture(const RE::GTexture* a_texture)
	{
		if (!a_texture)
			return;
		TextureInfo info = ReadTextureInfo(a_texture);
		if (const auto found = named.find(a_texture); found != named.end() && textures[found->second] == info)
			return;
		const auto index = static_cast<std::uint32_t>(textures.size());
		textures.push_back(std::move(info));
		named[a_texture] = index;
		auto& command = Add(UiOp::Texture);
		command.a = static_cast<std::int32_t>(index);
		command.key = a_texture;
	}

	StreamHeader UiStream::EndState() const
	{
		StreamHeader end = header;
		auto& state = end.state;
		bool inDisplay = false;
		const auto floats = [&](const UiCommand& a_command) { return reinterpret_cast<const float*>(At(a_command)); };
		for (const auto& c : commands) {
			switch (c.op) {
			case UiOp::BeginDisplay:
				{
					// TranslateBeginDisplay's state changes for a display it captures.
					state.videoModeSet = c.b != 0;
					if (state.videoModeSet)
						state.world3D = false;
					inDisplay = state.videoModeSet && c.a >= 0;
					if (!inDisplay)
						break;
					end.blendStack.clear();
					const auto& args = *reinterpret_cast<const BeginDisplayArgs*>(At(c));
					if (reinterpret_cast<const std::uint8_t*>(&args.background)[3] != 0)
						std::memcpy(state.matrix, kIdentityMatrix, sizeof(kIdentityMatrix));
					break;
				}
			case UiOp::EndDisplay:
				inDisplay = false;
				break;
			case UiOp::SetMatrix:
				std::memcpy(state.matrix, floats(c), sizeof(state.matrix));
				break;
			case UiOp::SetUserMatrix:
				std::memcpy(state.userMatrix, floats(c), sizeof(state.userMatrix));
				break;
			case UiOp::SetCxform:
				std::memcpy(state.cxform, floats(c), sizeof(state.cxform));
				break;
			case UiOp::PushBlendMode:
				BlendStackPush(end.blendMode, end.blendStack, static_cast<std::uint32_t>(c.a));
				break;
			case UiOp::PopBlendMode:
				BlendStackPop(end.blendMode, end.blendStack);
				break;
			case UiOp::SetPerspective3D:
				state.SetPerspective3D(*reinterpret_cast<const float (*)[16]>(At(c)));
				break;
			case UiOp::SetView3D:
				state.SetView3D(*reinterpret_cast<const float (*)[16]>(At(c)));
				break;
			case UiOp::SetWorld3D:
				state.world3D = c.size != 0;
				if (state.world3D)
					std::memcpy(state.worldMatrix, At(c), sizeof(state.worldMatrix));
				break;
			case UiOp::SetVertexData:
				state.SetVertexData(nullptr, c.a, c.b);
				break;
			case UiOp::SetIndexData:
				state.SetIndexData(nullptr, c.a, c.b);
				break;
			case UiOp::DrawIndexedTriList:
			case UiOp::DrawLineStrip:
			case UiOp::DrawBitmaps:
			case UiOp::DrawBlurRect:
			case UiOp::DrawColorMatrixRect:
				// ApplyMatrix3D's cached product, which a 3D draw brings up to date.
				if (inDisplay && state.world3D)
					RefreshViewPerspective(state);
				break;
			case UiOp::LineStyleDisable:
				state.LineStyleDisable();
				break;
			case UiOp::LineStyleColor:
				state.LineStyleColor(static_cast<std::uint32_t>(c.a));
				break;
			case UiOp::FillStyleDisable:
				state.FillStyleDisable();
				break;
			case UiOp::FillStyleColor:
				state.FillStyleColor(static_cast<std::uint32_t>(c.a));
				break;
			case UiOp::FillStyleBitmap:
				state.FillStyleBitmap(*reinterpret_cast<const Layout::FillTexture*>(At(c)));
				break;
			case UiOp::FillStyleGouraud:
				{
					const auto* fills = reinterpret_cast<const Layout::FillTexture*>(At(c));
					state.FillStyleGouraud(c.a, (c.b & 1) ? &fills[0] : nullptr, (c.b & 2) ? &fills[1] : nullptr, (c.b & 4) != 0);
					break;
				}
			default:
				break;
			}
		}
		state.vertexData = nullptr;
		state.indexData = nullptr;
		return end;
	}
}
