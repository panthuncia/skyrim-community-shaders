#include "RenderGraph/Engine/Scaleform/Scaleform.h"

#include "Globals.h"
#include "RenderGraph/Engine/Scaleform/Display.h"
#include "RenderGraph/Engine/Scaleform/GRendererLayout.h"
#include "RenderGraph/Engine/Scaleform/GfxTextures.h"
#include "RenderGraph/Engine/Scaleform/Passes.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Switches.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace RenderGraph::Engine::Scaleform
{
	namespace
	{
		using Layout::Field;
		using Layout::FillStyle;
		using Layout::FillTexture;

		constexpr std::size_t kSlots = 58;

		// The slots CS takes (engine-d3d11-surface.md R5, "Vtable"; CommonLib numbers 0-45 the same).
		enum Slot : std::size_t
		{
			kSetDisplayRenderTarget = 7,
			kPushRenderTarget = 8,
			kPopRenderTarget = 9,
			kPushTempRenderTarget = 10,
			kBeginDisplay = 12,
			kEndDisplay = 13,
			kSetMatrix = 14,
			kSetUserMatrix = 15,
			kSetCxform = 16,
			kPushBlendMode = 17,
			kPopBlendMode = 18,
			kSetPerspective3D = 21,
			kSetView3D = 22,
			kSetWorld3D = 23,
			kSetVertexData = 27,
			kSetIndexData = 28,
			kDrawIndexedTriList = 30,
			kDrawLineStrip = 31,
			kLineStyleDisable = 32,
			kLineStyleColor = 33,
			kFillStyleDisable = 34,
			kFillStyleColor = 35,
			kFillStyleBitmap = 36,
			kFillStyleGouraud = 37,
			kDrawBitmaps = 38,
			kBeginSubmitMask = 40,
			kEndSubmitMask = 41,
			kDisableMask = 42,
			kDrawBlurRect = 44,
			kDrawColorMatrixRect = 45,
		};

		// The renderer's 1/255 (DAT_141540648), multiplied as it multiplies.
		constexpr float kByteToUnit = 0.003921568859368563f;
		// GRenderer blend modes 0-14 to the renderer's blend states (table 0x1417d4468).
		constexpr std::array<std::uint8_t, 15> kBlendStates{ 1, 1, 1, 4, 1, 5, 6, 1, 2, 3, 1, 7, 7, 1, 1 };
		constexpr std::uint32_t kBlendMultiply = 3;
		constexpr std::uint32_t kBlendDarken = 6;
		constexpr std::uint32_t kNoOffset = ~0u;
		// GViewport flags.
		constexpr std::uint32_t kViewportRenderTexture = 1;
		constexpr std::uint32_t kViewportScissor = 4;
		// A display count after which the totals are logged.
		constexpr std::uint64_t kReportEvery = 1000;

		/** @brief GRenderer::BitmapDesc. */
		struct BitmapDesc
		{
			float coords[4];     // left, top, right, bottom
			float texCoords[4];  // the same
			std::uint32_t color;
		};
		static_assert(sizeof(BitmapDesc) == 0x24);

		/** @brief GViewport. */
		struct Viewport
		{
			std::int32_t bufferWidth, bufferHeight, left, top, width, height;
			std::int32_t scissorLeft, scissorTop, scissorWidth, scissorHeight;
			float scale, aspectRatio;
			std::uint32_t flags, padding;
		};
		static_assert(sizeof(Viewport) == 0x38);

		/** @brief The D3D11 renderer's own slots. Written once, before the renderer is used. */
		std::array<void*, kSlots + 1> g_original{};
		/** @brief CS's vtable: element 0 is the word before the original's, so the object points at element 1. */
		std::array<void*, kSlots + 1> g_vtable{};

		template <class Fn>
		Fn Original(Slot a_slot)
		{
			return reinterpret_cast<Fn>(g_original[a_slot + 1]);
		}

		using SetMatrixFn = void (*)(void*, const float*);
		using SetBlendFn = void (*)(void*, std::uint32_t);
		using VoidFn = void (*)(void*);
		using DataFn = void (*)(void*, const void*, std::int32_t, std::int32_t, void*);
		using DrawTrianglesFn = void (*)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t);
		using DrawLinesFn = void (*)(void*, std::int32_t, std::int32_t);
		using ColorFn = void (*)(void*, const std::uint32_t*);
		using FillBitmapFn = void (*)(void*, const FillTexture*);
		using FillGouraudFn = void (*)(void*, std::int32_t, const FillTexture*, const FillTexture*, const FillTexture*);
		using DrawBitmapsFn = void (*)(void*, const BitmapDesc*, std::int32_t, std::int32_t, std::int32_t, RE::GTexture*, const float*, void*);
		using MaskFn = void (*)(void*, std::int32_t);
		using BeginDisplayFn = void (*)(void*, const std::uint32_t*, const Viewport*, float, float, float, float);
		using DisplayTargetFn = void (*)(void*, void*, bool);
		using PushTargetFn = void (*)(void*, const float*, void*);
		using PushTempTargetFn = RE::GTexture* (*)(void*, const float*, std::uint32_t, std::uint32_t, bool);
		using BlurFn = void (*)(void*, RE::GTexture*, const float*, const float*, const void*);
		using ColorMatrixFn = void (*)(void*, RE::GTexture*, const float*, const float*, const float*);

		/**
		 * @brief The census (docs/development/p5/scaleform.md, "Census"): what drew each display, and every D3D11 part by reason. A
		 * display is the graph's (every draw on the graph), mixed (graph segments with D3D11 parts between them: filters, render
		 * targets, a draw the graph does not take) or D3D11's (no graph segment). Any thread (a loading-screen display counts too).
		 */
		struct Counters
		{
			std::mutex mutex;
			std::uint64_t graph = 0;
			std::uint64_t mixed = 0;
			std::uint64_t d3d11 = 0;
			std::uint64_t segments = 0;                    // graph segments drawn
			std::map<std::string, std::uint64_t> reasons;  // D3D11 parts (a whole display counts once)
		};

		Counters& Count()
		{
			static Counters s_counters;
			return s_counters;
		}

		void Report(Counters& a_counters)
		{
			const std::uint64_t total = a_counters.graph + a_counters.mixed + a_counters.d3d11;
			if (total % kReportEvery)
				return;
			std::string reasons;
			for (const auto& [reason, count] : a_counters.reasons)
				reasons += fmt::format("{}{} {}", reasons.empty() ? "" : ", ", count, reason);
			logger::info("[ORG] Scaleform census: {} displays: {} on the graph, {} on the graph with D3D11 parts, {} on D3D11; {} graph segments; D3D11 parts: {}",
				total, a_counters.graph, a_counters.mixed, a_counters.d3d11, a_counters.segments, reasons.empty() ? "none" : reasons);
		}

		/** @brief A part of a display (or a whole one) the D3D11 renderer draws, and why: logged the first time a reason comes up. */
		void NoteD3D11(const char* a_reason)
		{
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			if (counters.reasons[a_reason]++ == 0)
				logger::info("[ORG] Scaleform: the D3D11 renderer draws: {}", a_reason);
		}

		/** @brief A display that never began a capture: D3D11's whole. */
		void CountD3D11(const char* a_reason)
		{
			NoteD3D11(a_reason);
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			++counters.d3d11;
			Report(counters);
		}

		/** @brief The most D3D11 parts a display has between graph segments; past it the rest of the display is D3D11's. */
		constexpr std::uint32_t kMaxD3D11Parts = 8;

		/** @brief The display being captured. Render thread only. */
		struct Capture
		{
			bool active = false;
			Display display;  // the segment being captured
			// The segment's calls, to replay into the D3D11 renderer, and the copies they point at (alive until the next display).
			std::vector<std::function<void(void*)>> calls;
			std::deque<std::vector<std::byte>> copies;
			// BeginDisplay's arguments, for the D3D11 renderer's BeginDisplay when it draws a part (D3D11Ready).
			Viewport viewport{};
			float frame[4]{};  // x0, x1, y0, y1
			// The mirrored state.
			std::uint32_t blendMode = 0;
			std::uint32_t savedBlendMode = 0;  // the renderer's when the display began
			std::vector<std::uint32_t> blendStack;
			bool inMask = false;
			std::int32_t maskLevel = 0;
			Stencil stencil = Stencil::Off;
			std::uint32_t stencilRef = 0;
			float viewMatrix[6]{};  // the user matrix times the viewport matrix
			float texgen[4][4]{};   // the vertex constants' texgen rows, as the renderer keeps them between draws
			std::unordered_map<ID3D11ShaderResourceView*, std::int32_t> textureIndex;
			/** @brief The blend and mask state a segment begins in, which a replay of it starts from. */
			struct MaskState
			{
				std::uint32_t blendMode = 0;
				bool inMask = false;
				std::int32_t maskLevel = 0;
				Stencil stencil = Stencil::Off;
				std::uint32_t stencilRef = 0;
			} segmentStart;
			// D3D11 parts: whether the D3D11 renderer has begun the display, the render targets it has pushed (their draws are its),
			// a filter it is drawing, and the display's fate so far.
			bool d3d11Begun = false;
			std::uint32_t targetDepth = 0;
			bool inFilter = false;
			std::uint32_t d3d11Parts = 0;
			std::uint32_t graphSegments = 0;
			// The vertex and index arrays the draws read: the renderer's fields when they were copied, the copy, its offset in the
			// segment's staging bytes, and the cell the kept SetVertexData/SetIndexData replays from.
			struct Array
			{
				const void* source = nullptr;
				std::int32_t count = 0;
				std::uint32_t elementSize = 0;
				const std::byte* copy = nullptr;
				std::uint32_t staging = kNoOffset;
				std::shared_ptr<const void*> cell;
				// GFx's last call, put back after a replay.
				bool set = false;
				const void* data = nullptr;
				std::int32_t elements = 0;
				std::int32_t format = 0;
				void* cache = nullptr;
			};
			Array vertices;
			Array indices;
		};

		Capture& Current()
		{
			static Capture s_capture;
			return s_capture;
		}

		/** @brief Whether calls are captured now: a display is, and the D3D11 renderer is not drawing a render target or filter of it. */
		bool Capturing()
		{
			const auto& capture = Current();
			return capture.active && capture.targetDepth == 0 && !capture.inFilter && FrameAnchors::OnRenderThread();
		}

		void Keep(std::function<void(void*)> a_call)
		{
			Current().calls.push_back(std::move(a_call));
		}

		/** @brief a_size bytes of a_source kept until the next display (a replay's argument). */
		const std::byte* Copy(const void* a_source, std::size_t a_size)
		{
			auto& copy = Current().copies.emplace_back(a_size);
			if (a_size && a_source)
				std::memcpy(copy.data(), a_source, a_size);
			return copy.data();
		}

		/** @brief The D3D11 renderer's blend and mask fields set to a_state, and its depth-stencil state bound as its mask calls bind it. */
		void SyncD3D11(void* a_renderer, const Capture::MaskState& a_state)
		{
			Field<std::uint32_t>(a_renderer, Layout::kBlendMode) = a_state.blendMode;
			Field<std::uint8_t>(a_renderer, Layout::kMaskSubmitting) = a_state.inMask ? 1 : 0;
			Field<std::int32_t>(a_renderer, Layout::kMaskLevel) = a_state.maskLevel;
			auto* const* states = &Field<ID3D11DepthStencilState*>(a_renderer, Layout::kDepthStencilStates);
			auto* state = states[static_cast<std::size_t>(a_state.stencil)];
			Field<ID3D11DepthStencilState*>(a_renderer, Layout::kDepthStencilState) = state;
			if (auto* context = Field<ID3D11DeviceContext*>(a_renderer, Layout::kContext))
				context->OMSetDepthStencilState(state, a_state.stencilRef);
		}

		Capture::MaskState MirroredState()
		{
			const auto& capture = Current();
			return { capture.blendMode, capture.inMask, capture.maskLevel, capture.stencil, capture.stencilRef };
		}

		/**
		 * @brief The D3D11 renderer ready to draw a part of the display: its BeginDisplay run once (without the background, which
		 * the graph's first segment drew; the world 3D matrix it resets put back), and its blend and mask state the mirrored one.
		 */
		void D3D11Ready(void* a_renderer)
		{
			auto& capture = Current();
			if (!capture.d3d11Begun) {
				const float* world = Field<const float*>(a_renderer, Layout::kWorld3D);
				const std::uint32_t none = 0;
				Original<BeginDisplayFn>(kBeginDisplay)(a_renderer, &none, &capture.viewport, capture.frame[0], capture.frame[1], capture.frame[2], capture.frame[3]);
				Field<const float*>(a_renderer, Layout::kWorld3D) = world;
				capture.d3d11Begun = true;
			}
			SyncD3D11(a_renderer, MirroredState());
		}

		/** @brief The segment's kept calls replayed into the D3D11 renderer; GFx's arrays set back after. */
		void Replay(void* a_renderer)
		{
			auto& capture = Current();
			if (capture.d3d11Begun) {
				// A later segment: from the state it began in.
				SyncD3D11(a_renderer, capture.segmentStart);
			} else {
				// The first: its calls begin with the display's BeginDisplay.
				Field<std::uint32_t>(a_renderer, Layout::kBlendMode) = capture.savedBlendMode;
				capture.d3d11Begun = true;
			}
			for (const auto& call : capture.calls)
				call(a_renderer);
			capture.calls.clear();
			if (capture.vertices.set)
				Original<DataFn>(kSetVertexData)(a_renderer, capture.vertices.data, capture.vertices.elements, capture.vertices.format, capture.vertices.cache);
			if (capture.indices.set)
				Original<DataFn>(kSetIndexData)(a_renderer, capture.indices.data, capture.indices.elements, capture.indices.format, capture.indices.cache);
		}

		/** @brief The array's setter kept at a segment's start, so a replay of the segment reads the copy (or GFx's array, not drawn yet). */
		template <Slot S>
		void KeepArray(Capture::Array& a_array)
		{
			a_array.staging = kNoOffset;
			if (!a_array.set)
				return;
			auto cell = std::make_shared<const void*>(static_cast<const void*>(a_array.copy));
			a_array.cell = cell;
			const void* data = a_array.data;
			const std::int32_t count = a_array.elements;
			const std::int32_t format = a_array.format;
			Keep([cell, data, count, format](void* a_self) { Original<DataFn>(S)(a_self, *cell ? *cell : data, count, format, nullptr); });
		}

		/** @brief A new segment: nothing kept yet, the state it begins in noted. a_afterD3D11: the D3D11 renderer drew since the last. */
		void StartSegment(void* a_renderer, bool a_afterD3D11)
		{
			auto& capture = Current();
			capture.calls.clear();
			capture.display.ResetCommands();
			capture.textureIndex.clear();
			capture.segmentStart = MirroredState();
			KeepArray<kSetVertexData>(capture.vertices);
			KeepArray<kSetIndexData>(capture.indices);
			// The renderer's texgen rows are the ones its last draw left.
			if (a_afterD3D11)
				std::memcpy(capture.texgen, &Field<float>(a_renderer, Layout::kTexgen), sizeof(capture.texgen));
		}

		/** @brief A D3D11 part of the display, counted (its reason logged the first time). */
		void NotePart(const char* a_reason)
		{
			++Current().d3d11Parts;
			NoteD3D11(a_reason);
		}

		/** @brief The segment drawn now: by the graph, or replayed into the D3D11 renderer when the graph cannot draw it. */
		void Flush(void* a_renderer)
		{
			auto& capture = Current();
			if (capture.display.commands.empty())
				return;
			const char* reason = nullptr;
			if (Passes::Submit(capture.display, reason)) {
				++capture.graphSegments;
				auto& counters = Count();
				std::scoped_lock lock(counters.mutex);
				++counters.segments;
				return;
			}
			NotePart(reason ? reason : "the graph did not draw a segment");
			Replay(a_renderer);
		}

		/** @brief The display's fate, counted once its capture ends. */
		void FinishDisplay()
		{
			const auto& capture = Current();
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			if (capture.d3d11Parts == 0)
				++counters.graph;
			else if (capture.graphSegments == 0)
				++counters.d3d11;
			else
				++counters.mixed;
			Report(counters);
		}

		/** @brief The capture ends: the rest of the display goes to the D3D11 renderer directly (it is ready for it). */
		void EndCapture(const char* a_reason)
		{
			auto& capture = Current();
			capture.active = false;
			capture.calls.clear();
			capture.display.ResetCommands();
			NotePart(a_reason);
			FinishDisplay();
		}

		/**
		 * @brief The D3D11 renderer draws a part of the display now (a render target, a filter): the segment so far is drawn first,
		 * so the order is D3D11's, and the D3D11 renderer is made ready for it.
		 */
		void BeginD3D11Part(void* a_renderer, const char* a_reason)
		{
			NotePart(a_reason);
			Flush(a_renderer);
			D3D11Ready(a_renderer);
		}

		/**
		 * @brief The capture goes on after a D3D11 part, onto the target it began on. When the D3D11 renderer left another target
		 * bound (its render target stack restores the display render target it was given), the rest of the display is D3D11's.
		 */
		void ResumeAfterD3D11(void* a_renderer)
		{
			auto& capture = Current();
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			globals::d3d::context->OMGetRenderTargets(1, rtv.put(), nullptr);
			winrt::com_ptr<ID3D11Resource> resource;
			if (rtv)
				rtv->GetResource(resource.put());
			if (resource.get() != static_cast<ID3D11Resource*>(capture.display.target)) {
				EndCapture("another render target bound after a D3D11 part (the rest of the display)");
				return;
			}
			if (capture.d3d11Parts >= kMaxD3D11Parts) {
				EndCapture("too many D3D11 parts in one display (the rest of the display)");
				return;
			}
			StartSegment(a_renderer, true);
		}

		/**
		 * @brief A call the graph does not take, the last one kept: the segment before it is drawn, the D3D11 renderer draws the call,
		 * and the capture goes on with a new segment. Returns false.
		 */
		bool Abort(void* a_renderer, const char* a_reason)
		{
			auto& capture = Current();
			auto call = std::move(capture.calls.back());
			capture.calls.pop_back();
			BeginD3D11Part(a_renderer, a_reason);
			call(a_renderer);
			if (capture.d3d11Parts >= kMaxD3D11Parts) {
				EndCapture("too many D3D11 parts in one display (the rest of the display)");
				return false;
			}
			StartSegment(a_renderer, true);
			return false;
		}

		bool VideoModeSet(void* a_renderer)
		{
			return Field<std::uint8_t>(a_renderer, Layout::kVideoModeSet) != 0;
		}

		std::uint8_t BlendState()
		{
			const auto& capture = Current();
			return capture.inMask ? 7 : kBlendStates[capture.blendMode > 14 ? 0 : capture.blendMode];
		}

		bool Multiply()
		{
			const auto mode = Current().blendMode;
			return mode == kBlendMultiply || mode == kBlendDarken;
		}

		/** @brief ApplyColor's constant: the BGRA colour as RGBA floats, towards white by its alpha for Multiply and Darken. */
		void ColorConstant(std::uint32_t a_color, float (&a_out)[4])
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_color);
			a_out[3] = static_cast<float>(bytes[3]) * kByteToUnit;
			a_out[0] = static_cast<float>(bytes[2]) * kByteToUnit;
			a_out[1] = static_cast<float>(bytes[1]) * kByteToUnit;
			a_out[2] = static_cast<float>(bytes[0]) * kByteToUnit;
			if (Multiply())
				for (int i = 0; i < 3; ++i)
					a_out[i] = (a_out[i] - 1.0f) * a_out[3] + 1.0f;
		}

		/** @brief ApplyCxform's constants: the multiplies, then the adds over 255. */
		void CxformConstants(const float (&a_cxform)[8], DrawRecord& a_record)
		{
			for (int i = 0; i < 4; ++i) {
				a_record.cxmul[i] = a_cxform[i * 2];
				a_record.cxadd[i] = static_cast<float>(a_cxform[i * 2 + 1]) * kByteToUnit;
			}
		}

		/** @brief ApplyMatrix in 2D: mvp = (user times viewport) times a_matrix, as GMatrix2D's Prepend computes it. */
		void MatrixConstants(const float (&a_matrix)[6], DrawRecord& a_record)
		{
			const float* a = Current().viewMatrix;
			const float* b = a_matrix;
			const float m0 = a[0] * b[0] + a[1] * b[3];
			const float m3 = a[3] * b[0] + a[4] * b[3];
			const float m1 = a[1] * b[4] + a[0] * b[1];
			const float m4 = a[4] * b[4] + a[3] * b[1];
			const float m2 = a[1] * b[5] + a[0] * b[2] + a[2];
			const float m5 = a[4] * b[5] + a[3] * b[2] + a[5];
			const float rows[4][4] = { { m0, m1, 0.0f, m2 }, { m3, m4, 0.0f, m5 }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
			std::memcpy(a_record.mvp, rows, sizeof(rows));
		}

		/** @brief Whether a_texture is a plain GTextureD3D1x (not a YUV one or another class): its fields are the ones read. */
		bool IsPlainTexture(const RE::GTexture* a_texture)
		{
			static const std::uintptr_t vtable = REL::RelocationID(292188, 243114).address();
			return a_texture && *reinterpret_cast<const std::uintptr_t*>(a_texture) == vtable;
		}

		/** @brief The display's index for a texture's view (DXVK's, referenced once per display); -1, aborted, when there is none. */
		std::int32_t TextureIndex(void* a_renderer, RE::GTexture* a_texture)
		{
			auto& capture = Current();
			auto* view = Field<ID3D11ShaderResourceView*>(a_texture, Layout::kTextureView);
			auto* raw = view ? Tripwire::Unwrap(view) : nullptr;
			if (!raw) {
				Abort(a_renderer, view ? "a texture view the graph cannot read (a native handle)" : "a texture without a view");
				return -1;
			}
			if (const auto found = capture.textureIndex.find(raw); found != capture.textureIndex.end())
				return found->second;
			raw->AddRef();
			const auto index = static_cast<std::int32_t>(capture.display.textures.size());
			capture.display.textures.push_back(raw);
			capture.textureIndex.emplace(raw, index);
			return index;
		}

		/** @brief ApplyFillTexture: stage a_stage's texture and sampler, and its texgen rows. False, aborted, when it cannot be read. */
		bool BindTexture(void* a_renderer, const FillTexture& a_fill, std::uint32_t a_stage, Command& a_command)
		{
			if (!IsPlainTexture(a_fill.texture))
				return Abort(a_renderer, "a fill texture that is not a plain GTextureD3D1x");
			const auto index = TextureIndex(a_renderer, a_fill.texture);
			if (index < 0)
				return false;
			a_command.textures[a_stage] = index;
			a_command.samplers[a_stage] = static_cast<std::uint8_t>((a_fill.wrapMode * 2 + a_fill.sampleMode) & 3);
			auto& texgen = Current().texgen;
			texgen[a_stage * 2][0] = a_fill.matrix[0];
			texgen[a_stage * 2][1] = a_fill.matrix[1];
			texgen[a_stage * 2][3] = a_fill.matrix[2];
			texgen[a_stage * 2 + 1][0] = a_fill.matrix[3];
			texgen[a_stage * 2 + 1][1] = a_fill.matrix[4];
			texgen[a_stage * 2 + 1][3] = a_fill.matrix[5];
			return true;
		}

		/** @brief ApplyFillStyle: the pixel shader, its constants and textures. False, aborted, for what the graph path does not draw. */
		bool ApplyFill(void* a_renderer, const FillStyle& a_fill, std::int32_t a_vertexFormat, Command& a_command, DrawRecord& a_record)
		{
			switch (a_fill.mode) {
			case 1:
				a_command.state.pixelKind = 1;
				ColorConstant(a_fill.color, a_record.cxmul);
				break;
			case 2:
				if (!a_fill.textures[0].texture)
					return Abort(a_renderer, "a bitmap fill without a texture");
				if (!BindTexture(a_renderer, a_fill.textures[0], 0, a_command))
					return false;
				a_command.state.pixelKind = Multiply() ? 3 : 2;
				CxformConstants(a_fill.cxform, a_record);
				break;
			case 3:
				{
					std::uint8_t kind = a_vertexFormat == Layout::kVertexXY16iC32 ? 12 : 11;
					if (a_fill.textures[0].texture) {
						if (!BindTexture(a_renderer, a_fill.textures[0], 0, a_command))
							return false;
						if (static_cast<std::uint32_t>(a_fill.gouraudType) - 1u < 2u) {
							kind = 13;
						} else {
							kind = 14;
							if (!a_fill.textures[1].texture)
								return Abort(a_renderer, "a two-texture gouraud fill without its second texture");
							if (!BindTexture(a_renderer, a_fill.textures[1], 1, a_command))
								return false;
						}
					}
					a_command.state.pixelKind = Multiply() ? kind + 4 : kind;
					CxformConstants(a_fill.cxform, a_record);
					break;
				}
			default:
				return Abort(a_renderer, "a draw with the fill off");
			}
			return true;
		}

		/** @brief The array a field set describes, copied (once while the fields stay the same) into the replay copies and the staging bytes. */
		void EnsureArray(Capture::Array& a_array, const void* a_source, std::int32_t a_count, std::uint32_t a_elementSize)
		{
			const std::size_t size = static_cast<std::size_t>(std::max(a_count, 0)) * a_elementSize;
			if (a_array.copy && a_array.source == a_source && a_array.count == a_count && a_array.elementSize == a_elementSize) {
				// Copied already; a new segment's staging bytes need it again.
				if (a_array.staging == kNoOffset)
					a_array.staging = Current().display.Append(a_array.copy, size, 4);
				return;
			}
			a_array.copy = Copy(a_source, size);
			a_array.source = a_source;
			a_array.count = a_count;
			a_array.elementSize = a_elementSize;
			a_array.staging = Current().display.Append(a_array.copy, size, 4);
			if (a_array.cell)
				*a_array.cell = a_array.copy;
		}

		void EnsureVertices(void* a_renderer)
		{
			EnsureArray(Current().vertices, Field<const void*>(a_renderer, Layout::kVertexData), Field<std::int32_t>(a_renderer, Layout::kVertexCount),
				Field<std::uint32_t>(a_renderer, Layout::kVertexStride));
		}

		void EnsureIndices(void* a_renderer)
		{
			const std::uint32_t size = Field<std::uint32_t>(a_renderer, Layout::kIndexFormat) == DXGI_FORMAT_R16_UINT ? 2 : 4;
			EnsureArray(Current().indices, Field<const void*>(a_renderer, Layout::kIndexData), Field<std::int32_t>(a_renderer, Layout::kIndexCount), size);
		}

		/** @brief A draw: its record appended (192 bytes, 16-aligned) and its command. */
		void Emit(Command& a_command, const DrawRecord& a_record)
		{
			auto& capture = Current();
			a_command.kind = Command::Kind::Draw;
			a_command.state.blend = BlendState();
			a_command.state.stencil = capture.stencil;
			a_command.stencilRef = capture.stencilRef;
			a_command.record = capture.display.Append(&a_record, sizeof(a_record), 16);
			capture.display.commands.push_back(a_command);
		}

		/**
		 * @brief BeginDisplay's viewport and matrix: the viewport clipped to the buffer (and its scissor rectangle), the frame
		 * rectangle moved with it, the matrix mapping it to clip space, times the user matrix (as GMatrix2D's Append does).
		 */
		void ComputeView(void* a_renderer, const Viewport& a_viewport, float a_x0, float a_x1, float a_y0, float a_y1)
		{
			auto& capture = Current();
			float dx = a_x1 - a_x0;
			float dy = a_y1 - a_y0;
			if (dx < 1.0f)
				dx = 1.0f;
			if (dy < 1.0f)
				dy = 1.0f;
			std::int32_t vx = a_viewport.left, vw = a_viewport.width;
			float fx0 = a_x0, fx1 = a_x1;
			if (a_viewport.left < 0) {
				vw = a_viewport.left + a_viewport.width;
				vx = 0;
				if (a_viewport.width <= -a_viewport.left)
					vw = 0;
				fx0 = a_x0 - (static_cast<float>(a_viewport.left) * dx) / static_cast<float>(a_viewport.width);
			}
			if (a_viewport.bufferWidth < a_viewport.width + a_viewport.left) {
				vw = a_viewport.bufferWidth - vx;
				fx1 = (static_cast<float>(a_viewport.bufferWidth - a_viewport.left) * dx) / static_cast<float>(a_viewport.width) + a_x0;
			}
			std::int32_t vy = a_viewport.top, vh = a_viewport.height;
			float fy0 = a_y0, fy1 = a_y1;
			if (a_viewport.top < 0) {
				vh = a_viewport.top + a_viewport.height;
				vy = 0;
				if (a_viewport.height <= -a_viewport.top)
					vh = 0;
				fy0 = a_y0 - (static_cast<float>(a_viewport.top) * dy) / static_cast<float>(a_viewport.height);
			}
			if (a_viewport.bufferHeight < a_viewport.height + a_viewport.top) {
				vh = a_viewport.bufferHeight - vy;
				fy1 = (static_cast<float>(a_viewport.bufferHeight - a_viewport.top) * dy) / static_cast<float>(a_viewport.height) + a_y0;
			}
			float fw = fx1 - fx0;
			float fh = fy1 - fy0;
			float x = static_cast<float>(vx), y = static_cast<float>(vy), w = static_cast<float>(vw), h = static_cast<float>(vh);
			if (a_viewport.flags & kViewportScissor) {
				const std::int32_t sx = std::max(a_viewport.scissorLeft, vx);
				const std::int32_t sy = std::max(a_viewport.scissorTop, vy);
				std::int32_t sw = a_viewport.scissorWidth + (a_viewport.scissorLeft - sx);
				std::int32_t sh = a_viewport.scissorHeight + (a_viewport.scissorTop - sy);
				if (vw + vx < sw + sx)
					sw = (vw - sx) + vx;
				if (vh + vy < sh + sy)
					sh = (vh - sy) + vy;
				w = static_cast<float>(sw);
				h = static_cast<float>(sh);
				fx0 = fx0 + static_cast<float>(sx - vx) * (fw / static_cast<float>(vw));
				fw = (fw / static_cast<float>(vw)) * w;
				fy0 = fy0 + static_cast<float>(sy - vy) * (fh / static_cast<float>(vh));
				fh = (fh / static_cast<float>(vh)) * h;
				x = static_cast<float>(sx);
				y = static_cast<float>(sy);
			}
			capture.display.viewport[0] = x;
			capture.display.viewport[1] = y;
			capture.display.viewport[2] = w;
			capture.display.viewport[3] = h;

			float m[6] = { 2.0f / fw, 0.0f, 0.0f, 0.0f, -2.0f / fh, 0.0f };
			m[2] = -1.0f - m[0] * fx0;
			m[5] = 1.0f - m[4] * fy0;
			// m = user times m.
			const auto& u = Field<float[6]>(a_renderer, Layout::kUserMatrix);
			const float m0 = m[0], m1 = m[1], m2 = m[2];
			auto& out = capture.viewMatrix;
			out[0] = m0 * u[0] + m[3] * u[1];
			out[3] = m[3] * u[4] + m0 * u[3];
			out[1] = m1 * u[0] + m[4] * u[1];
			out[4] = m[4] * u[4] + m1 * u[3];
			out[2] = m2 * u[0] + m[5] * u[1] + u[2];
			out[5] = m[5] * u[4] + m2 * u[3] + u[5];
		}

		/** @brief Render target 0 and the depth-stencil bound now, from DXVK's context (DXVK's objects, whatever the engine holds). */
		bool ReadTargets(Display& a_display)
		{
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			winrt::com_ptr<ID3D11DepthStencilView> dsv;
			globals::d3d::context->OMGetRenderTargets(1, rtv.put(), dsv.put());
			if (!rtv)
				return false;
			winrt::com_ptr<ID3D11Resource> resource;
			rtv->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return false;
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtv->GetDesc(&rtvDesc);
			a_display.target = texture.detach();
			a_display.targetFormat = rtvDesc.Format;
			if (dsv) {
				winrt::com_ptr<ID3D11Resource> depthResource;
				dsv->GetResource(depthResource.put());
				if (auto depth = depthResource.try_as<ID3D11Texture2D>()) {
					D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
					dsv->GetDesc(&dsvDesc);
					a_display.depth = depth.detach();
					a_display.depthFormat = dsvDesc.Format;
				}
			}
			return true;
		}

		// The slots.

		void BeginDisplay(void* a_renderer, const std::uint32_t* a_background, const Viewport* a_viewport, float a_x0, float a_x1, float a_y0, float a_y1)
		{
			auto& capture = Current();
			const auto original = Original<BeginDisplayFn>(kBeginDisplay);
			if (capture.active) {
				// A display that never ended: its last segment is drawn now.
				Flush(a_renderer);
				capture.active = false;
				FinishDisplay();
			}
			const char* reason = nullptr;
			if (!FrameAnchors::OnRenderThread())
				reason = "a whole display off the render thread (a loading screen without CS_ORG_LOADING_THREAD=1)";
			else if (!Passes::Ready())
				reason = "a whole display before the render graph is up";
			else if (!VideoModeSet(a_renderer))
				reason = "a whole display without a video mode";
			else if (a_viewport->flags & kViewportRenderTexture)
				reason = "a whole display into a render texture of GFx's (graph displays read it after)";
			if (reason) {
				CountD3D11(reason);
				original(a_renderer, a_background, a_viewport, a_x0, a_x1, a_y0, a_y1);
				return;
			}

			capture.display.Reset();
			capture.calls.clear();
			capture.copies.clear();
			capture.textureIndex.clear();
			capture.vertices = {};
			capture.indices = {};
			if (!ReadTargets(capture.display)) {
				capture.display.Reset();
				CountD3D11("a whole display with no render target bound");
				original(a_renderer, a_background, a_viewport, a_x0, a_x1, a_y0, a_y1);
				return;
			}
			capture.active = true;
			capture.d3d11Begun = false;
			capture.targetDepth = 0;
			capture.inFilter = false;
			capture.d3d11Parts = 0;
			capture.graphSegments = 0;
			const std::uint32_t background = *a_background;
			const Viewport viewport = *a_viewport;
			capture.viewport = viewport;
			capture.frame[0] = a_x0;
			capture.frame[1] = a_x1;
			capture.frame[2] = a_y0;
			capture.frame[3] = a_y1;
			Keep([=](void* a_self) { Original<BeginDisplayFn>(kBeginDisplay)(a_self, &background, &viewport, a_x0, a_x1, a_y0, a_y1); });

			// What BeginDisplay resets, and the state it starts from.
			capture.blendMode = capture.savedBlendMode = Field<std::uint32_t>(a_renderer, Layout::kBlendMode);
			capture.blendStack.clear();
			capture.inMask = false;
			capture.maskLevel = 0;
			capture.stencil = Stencil::Off;
			capture.stencilRef = 0;
			capture.segmentStart = MirroredState();
			Field<const float*>(a_renderer, Layout::kWorld3D) = nullptr;
			std::memcpy(capture.texgen, &Field<float>(a_renderer, Layout::kTexgen), sizeof(capture.texgen));
			ComputeView(a_renderer, viewport, a_x0, a_x1, a_y0, a_y1);

			// The background: the frame rectangle in its colour, with the identity matrix (which stays set).
			if (reinterpret_cast<const std::uint8_t*>(&background)[3] != 0) {
				static constexpr float kIdentity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
				Original<SetMatrixFn>(kSetMatrix)(a_renderer, kIdentity);
				const std::int16_t x0 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_x0));
				const std::int16_t x1 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_x1));
				const std::int16_t y0 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_y0));
				const std::int16_t y1 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_y1));
				const std::int16_t corners[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
				const std::uint16_t strip[6] = { 0, 1, 2, 1, 3, 2 };  // the strip's two triangles
				DrawRecord record{};
				MatrixConstants(kIdentity, record);
				ColorConstant(background, record.cxmul);
				std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
				record.vertexOffset = capture.display.Append(corners, sizeof(corners), 4);
				record.vertexStride = 4;
				record.indexOffset = capture.display.Append(strip, sizeof(strip), 4);
				record.indexSize = 2;
				Command command{};
				command.state.vertexKind = 1;
				command.state.pixelKind = 1;
				command.vertexCount = 6;
				Emit(command, record);
			}
		}

		void EndDisplay(void* a_renderer)
		{
			auto& capture = Current();
			if (capture.active && FrameAnchors::OnRenderThread()) {
				// The last segment: by the graph, or replayed into D3D11.
				Flush(a_renderer);
				capture.active = false;
				Field<std::uint32_t>(a_renderer, Layout::kBlendMode) = capture.blendMode;
				FinishDisplay();
			}
			capture.calls.clear();
			capture.display.Reset();
			capture.textureIndex.clear();
			capture.targetDepth = 0;
			capture.inFilter = false;
			// Either way: it releases the textures GFx queued for release, and the targets a D3D11 BeginDisplay took.
			Original<VoidFn>(kEndDisplay)(a_renderer);
		}

		void SetDisplayRenderTarget(void* a_renderer, void* a_target, bool a_setState)
		{
			if (Capturing()) {
				BeginD3D11Part(a_renderer, "SetDisplayRenderTarget inside a display (the rest of the display)");
				Current().active = false;
				FinishDisplay();
			}
			Original<DisplayTargetFn>(kSetDisplayRenderTarget)(a_renderer, a_target, a_setState);
		}

		/**
		 * @brief A render target pushed: the D3D11 renderer draws everything into it (render to a texture, a filter's source), the
		 * segment before it drawn first. Its texture is GFx's, which a later graph draw reads like any other.
		 */
		void EnterTarget(void* a_renderer, const char* a_reason)
		{
			auto& capture = Current();
			if (Capturing()) {
				BeginD3D11Part(a_renderer, a_reason);
				capture.targetDepth = 1;
			} else if (capture.active && capture.targetDepth && !capture.inFilter && FrameAnchors::OnRenderThread()) {
				++capture.targetDepth;
			}
		}

		void PushRenderTarget(void* a_renderer, const float* a_frame, void* a_target)
		{
			EnterTarget(a_renderer, "render to a texture (PushRenderTarget)");
			Original<PushTargetFn>(kPushRenderTarget)(a_renderer, a_frame, a_target);
		}

		RE::GTexture* PushTempRenderTarget(void* a_renderer, const float* a_frame, std::uint32_t a_width, std::uint32_t a_height, bool a_stencil)
		{
			EnterTarget(a_renderer, "a filter's source (PushTempRenderTarget)");
			return Original<PushTempTargetFn>(kPushTempRenderTarget)(a_renderer, a_frame, a_width, a_height, a_stencil);
		}

		void PopRenderTarget(void* a_renderer)
		{
			Original<VoidFn>(kPopRenderTarget)(a_renderer);
			auto& capture = Current();
			if (capture.active && capture.targetDepth && !capture.inFilter && FrameAnchors::OnRenderThread() && --capture.targetDepth == 0)
				ResumeAfterD3D11(a_renderer);
		}

		/** @brief A filter drawn onto the display's target: the D3D11 renderer draws it (its passes and its own render targets) in order. */
		template <class Fn>
		void Filter(void* a_renderer, const char* a_reason, Fn&& a_draw)
		{
			auto& capture = Current();
			if (!Capturing())
				return a_draw();
			BeginD3D11Part(a_renderer, a_reason);
			capture.inFilter = true;
			a_draw();
			capture.inFilter = false;
			ResumeAfterD3D11(a_renderer);
		}

		void DrawBlurRect(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params)
		{
			Filter(a_renderer, "a blur, shadow, glow or bevel filter (DrawBlurRect)",
				[&] { Original<BlurFn>(kDrawBlurRect)(a_renderer, a_source, a_sourceRect, a_destRect, a_params); });
		}

		/**
		 * @brief DrawColorMatrixRect as a graph draw: FilterVS.hlsl and FilterPS.hlsl's FILTER 23 (24 for Multiply and Darken), with
		 * the constants the D3D11 renderer writes (SE 0x140f5e840): the current matrix, the source rectangle over the texture's size,
		 * the filters' blend array, the source bound clamped and linear. A source the graph cannot read makes it a D3D11 part.
		 */
		void CaptureColorMatrix(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const float* a_matrix)
		{
			auto& capture = Current();
			std::array<float, 4> source;
			std::array<float, 4> dest;
			std::array<float, 20> matrix;
			std::memcpy(source.data(), a_sourceRect, sizeof(source));
			std::memcpy(dest.data(), a_destRect, sizeof(dest));
			std::memcpy(matrix.data(), a_matrix, sizeof(matrix));
			Keep([=](void* a_self) { Original<ColorMatrixFn>(kDrawColorMatrixRect)(a_self, a_source, source.data(), dest.data(), matrix.data()); });
			if (!IsPlainTexture(a_source) || !Field<void*>(a_source, Layout::kTextureResource)) {
				Abort(a_renderer, "a colour matrix filter of a texture the graph cannot read");
				return;
			}
			const auto index = TextureIndex(a_renderer, a_source);
			if (index < 0)
				return;

			FilterRecord record{};
			const float width = static_cast<float>((std::max)(Field<std::uint32_t>(a_source, Layout::kTextureWidth), 1u));
			const float height = static_cast<float>((std::max)(Field<std::uint32_t>(a_source, Layout::kTextureHeight), 1u));
			const float u0 = source[0] / width, v0 = source[1] / height, u1 = source[2] / width, v1 = source[3] / height;
			const float corners[4][6] = {
				{ dest[0], dest[1], 0.0f, 1.0f, u0, v0 },
				{ dest[2], dest[1], 0.0f, 1.0f, u1, v0 },
				{ dest[0], dest[3], 0.0f, 1.0f, u0, v1 },
				{ dest[2], dest[3], 0.0f, 1.0f, u1, v1 },
			};
			// The renderer's strip of four as two triangles.
			constexpr int kOrder[6] = { 0, 1, 2, 2, 1, 3 };
			for (int i = 0; i < 6; ++i)
				std::memcpy(record.vertices[i], corners[kOrder[i]], sizeof(record.vertices[i]));
			DrawRecord matrixRecord{};
			MatrixConstants(Field<float[6]>(a_renderer, Layout::kMatrix), matrixRecord);
			std::memcpy(record.mvp, matrixRecord.mvp, sizeof(record.mvp));
			std::memcpy(record.cxmul, matrix.data(), sizeof(record.cxmul));
			std::memcpy(record.cxadd, matrix.data() + 16, sizeof(record.cxadd));

			Command command{};
			command.kind = Command::Kind::Draw;
			command.state.vertexKind = 0;
			command.state.pixelKind = 0;
			command.state.filter = Multiply() ? 24 : 23;
			// The filters' blend array for the mode; nothing while a mask is drawn (the colour writes stay off, as the renderer leaves them).
			command.state.blend = capture.inMask ? 7 : static_cast<std::uint8_t>(8 + kBlendStates[capture.blendMode > 14 ? 0 : capture.blendMode]);
			command.state.stencil = capture.stencil;
			command.stencilRef = capture.stencilRef;
			command.textures[0] = index;
			command.samplers[0] = command.samplers[1] = 3;  // Bind(0, clamp, linear)
			command.record = capture.display.Append(&record, sizeof(record), 16);
			command.vertexCount = 6;
			capture.display.commands.push_back(command);
		}

		void DrawColorMatrixRect(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const float* a_matrix)
		{
			if (Capturing() && a_source && a_sourceRect && a_destRect && a_matrix)
				return CaptureColorMatrix(a_renderer, a_source, a_sourceRect, a_destRect, a_matrix);
			Filter(a_renderer, "a colour matrix filter (DrawColorMatrixRect)",
				[&] { Original<ColorMatrixFn>(kDrawColorMatrixRect)(a_renderer, a_source, a_sourceRect, a_destRect, a_matrix); });
		}

		/** @brief A pure setter of a value it copies: forwarded always; inside a capture also kept, with its argument copied. */
		template <Slot S, std::size_t N>
		void MatrixSetter(void* a_renderer, const float* a_values)
		{
			Original<SetMatrixFn>(S)(a_renderer, a_values);
			if (!Capturing())
				return;
			std::array<float, N> values;
			std::memcpy(values.data(), a_values, sizeof(values));
			Keep([values](void* a_self) { Original<SetMatrixFn>(S)(a_self, values.data()); });
		}

		/** @brief SetWorld3D keeps the pointer it is given: a replay gives it a copy that lives until the next display. */
		void SetWorld3D(void* a_renderer, const float* a_matrix)
		{
			Original<SetMatrixFn>(kSetWorld3D)(a_renderer, a_matrix);
			if (!Capturing())
				return;
			const auto* matrix = a_matrix ? reinterpret_cast<const float*>(Copy(a_matrix, sizeof(float) * 16)) : nullptr;
			Keep([matrix](void* a_self) { Original<SetMatrixFn>(kSetWorld3D)(a_self, matrix); });
		}

		void PushBlendMode(void* a_renderer, std::uint32_t a_mode)
		{
			if (!Capturing())
				return Original<SetBlendFn>(kPushBlendMode)(a_renderer, a_mode);
			Keep([a_mode](void* a_self) { Original<SetBlendFn>(kPushBlendMode)(a_self, a_mode); });
			// The renderer's rule: the stack keeps the mode before; Normal and Layer keep the parent's mode.
			auto& capture = Current();
			capture.blendStack.push_back(capture.blendMode);
			if (a_mode > 2)
				capture.blendMode = a_mode;
		}

		void PopBlendMode(void* a_renderer)
		{
			if (!Capturing())
				return Original<VoidFn>(kPopBlendMode)(a_renderer);
			Keep([](void* a_self) { Original<VoidFn>(kPopBlendMode)(a_self); });
			auto& capture = Current();
			if (capture.blendStack.empty())
				return;
			// The renderer's rule: the topmost mode on the stack above Layer, or None.
			std::uint32_t mode = 0;
			for (auto it = capture.blendStack.rbegin(); it != capture.blendStack.rend(); ++it)
				if (*it > 2) {
					mode = *it;
					break;
				}
			capture.blendStack.pop_back();
			capture.blendMode = mode;
		}

		template <Slot S>
		void DataSetter(void* a_renderer, const void* a_data, std::int32_t a_count, std::int32_t a_format, void* a_cache)
		{
			Original<DataFn>(S)(a_renderer, a_data, a_count, a_format, a_cache);
			if (!Capturing())
				return;
			auto& array = S == kSetVertexData ? Current().vertices : Current().indices;
			// The copy is made at the array's first draw (GFx's memory is valid until then); a replay passes it, or GFx's pointer
			// when no draw read the array.
			auto cell = std::make_shared<const void*>(nullptr);
			array.cell = cell;
			array.copy = nullptr;
			array.set = true;
			array.data = a_data;
			array.elements = a_count;
			array.format = a_format;
			array.cache = a_cache;
			Keep([cell, a_data, a_count, a_format](void* a_self) { Original<DataFn>(S)(a_self, *cell ? *cell : a_data, a_count, a_format, nullptr); });
		}

		template <Slot S>
		void StyleDisable(void* a_renderer)
		{
			Original<VoidFn>(S)(a_renderer);
			if (Capturing())
				Keep([](void* a_self) { Original<VoidFn>(S)(a_self); });
		}

		template <Slot S>
		void StyleColor(void* a_renderer, const std::uint32_t* a_color)
		{
			Original<ColorFn>(S)(a_renderer, a_color);
			if (!Capturing())
				return;
			const std::uint32_t color = *a_color;
			Keep([color](void* a_self) { Original<ColorFn>(S)(a_self, &color); });
		}

		void FillStyleBitmap(void* a_renderer, const FillTexture* a_fill)
		{
			Original<FillBitmapFn>(kFillStyleBitmap)(a_renderer, a_fill);
			if (!Capturing())
				return;
			const FillTexture fill = *a_fill;
			Keep([fill](void* a_self) { Original<FillBitmapFn>(kFillStyleBitmap)(a_self, &fill); });
		}

		void FillStyleGouraud(void* a_renderer, std::int32_t a_type, const FillTexture* a_texture0, const FillTexture* a_texture1, const FillTexture* a_texture2)
		{
			Original<FillGouraudFn>(kFillStyleGouraud)(a_renderer, a_type, a_texture0, a_texture1, a_texture2);
			if (!Capturing())
				return;
			struct Fills
			{
				FillTexture textures[3];
				bool present[3];
			} fills{};
			const FillTexture* given[3] = { a_texture0, a_texture1, a_texture2 };
			for (int i = 0; i < 3; ++i)
				if (given[i]) {
					fills.textures[i] = *given[i];
					fills.present[i] = true;
				}
			Keep([a_type, fills](void* a_self) {
				Original<FillGouraudFn>(kFillStyleGouraud)(a_self, a_type, fills.present[0] ? &fills.textures[0] : nullptr, fills.present[1] ? &fills.textures[1] : nullptr,
					fills.present[2] ? &fills.textures[2] : nullptr);
			});
		}

		void DrawIndexedTriList(void* a_renderer, std::int32_t a_base, std::int32_t a_min, std::int32_t a_count, std::int32_t a_start, std::int32_t a_triangles)
		{
			if (!Capturing())
				return Original<DrawTrianglesFn>(kDrawIndexedTriList)(a_renderer, a_base, a_min, a_count, a_start, a_triangles);
			Keep([=](void* a_self) { Original<DrawTrianglesFn>(kDrawIndexedTriList)(a_self, a_base, a_min, a_count, a_start, a_triangles); });
			// The renderer's own early outs: nothing drawn.
			if (!VideoModeSet(a_renderer) || !Field<const void*>(a_renderer, Layout::kVertexData) || !Field<const void*>(a_renderer, Layout::kIndexData) ||
				!Field<std::uint32_t>(a_renderer, Layout::kIndexFormat) || a_triangles <= 0)
				return;
			// Copied first: a replay needs the arrays whatever ends the capture.
			EnsureVertices(a_renderer);
			EnsureIndices(a_renderer);
			if (Field<const float*>(a_renderer, Layout::kWorld3D)) {
				Abort(a_renderer, "a 3D draw");
				return;
			}
			if (Field<std::uint32_t>(a_renderer, Layout::kIndexFormat) != DXGI_FORMAT_R16_UINT) {
				Abort(a_renderer, "32-bit indices");
				return;
			}
			const auto& fill = Field<FillStyle>(a_renderer, Layout::kFillStyle);
			const std::int32_t format = Field<std::int32_t>(a_renderer, Layout::kVertexFormat);
			Command command{};
			if (fill.mode == 3) {
				if (fill.gouraudType == 4 || fill.gouraudType == 5) {
					Abort(a_renderer, "a gouraud fill of type 4 or 5");
					return;
				}
				if (format == Layout::kVertexXY16iC32)
					command.state.vertexKind = 3;
				else if (format == Layout::kVertexXY16iCF32)
					command.state.vertexKind = fill.gouraudType == 3 ? 5 : 4;
				else {
					Abort(a_renderer, "a gouraud fill of vertices without colours");
					return;
				}
			} else if (format != Layout::kVertexXY16i && format != Layout::kVertexXY16iC32 && format != Layout::kVertexXY16iCF32) {
				Abort(a_renderer, "triangles of XY32f vertices");
				return;
			}
			DrawRecord record{};
			if (!ApplyFill(a_renderer, fill, format, command, record))
				return;
			auto& capture = Current();
			MatrixConstants(Field<float[6]>(a_renderer, Layout::kMatrix), record);
			std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
			record.vertexOffset = capture.vertices.staging;
			record.vertexStride = capture.vertices.elementSize;
			record.indexOffset = capture.indices.staging + static_cast<std::uint32_t>(a_start) * 2;
			record.indexSize = 2;
			record.baseVertex = a_base;
			command.vertexCount = static_cast<std::uint32_t>(a_triangles) * 3;
			Emit(command, record);
		}

		void DrawLineStrip(void* a_renderer, std::int32_t a_base, std::int32_t a_lines)
		{
			if (!Capturing())
				return Original<DrawLinesFn>(kDrawLineStrip)(a_renderer, a_base, a_lines);
			Keep([=](void* a_self) { Original<DrawLinesFn>(kDrawLineStrip)(a_self, a_base, a_lines); });
			if (!VideoModeSet(a_renderer) || !Field<const void*>(a_renderer, Layout::kVertexData))
				return;
			EnsureVertices(a_renderer);
			Abort(a_renderer, "a line strip");
		}

		void DrawBitmaps(void* a_renderer, const BitmapDesc* a_bitmaps, std::int32_t a_listSize, std::int32_t a_start, std::int32_t a_count, RE::GTexture* a_texture,
			const float* a_matrix, void* a_cache)
		{
			if (!Capturing())
				return Original<DrawBitmapsFn>(kDrawBitmaps)(a_renderer, a_bitmaps, a_listSize, a_start, a_count, a_texture, a_matrix, a_cache);
			if (!VideoModeSet(a_renderer) || !a_bitmaps || !a_texture || a_count <= 0)
				return;  // the renderer draws nothing (and its state changes are the next draw's anyway)
			// The bitmaps and matrix copied, for a replay.
			const auto* bitmaps = reinterpret_cast<const BitmapDesc*>(Copy(a_bitmaps + a_start, sizeof(BitmapDesc) * static_cast<std::size_t>(a_count)));
			std::array<float, 6> matrix;
			std::memcpy(matrix.data(), a_matrix, sizeof(matrix));
			Keep([=](void* a_self) { Original<DrawBitmapsFn>(kDrawBitmaps)(a_self, bitmaps, a_count, 0, a_count, a_texture, matrix.data(), nullptr); });
			if (!IsPlainTexture(a_texture)) {
				Abort(a_renderer, "bitmaps of a texture that is not a plain GTextureD3D1x (video)");
				return;
			}
			if (!Field<void*>(a_texture, Layout::kTextureResource)) {
				Abort(a_renderer, "bitmaps of a texture whose data is lost");
				return;
			}
			Command command{};
			command.state.vertexKind = 2;
			command.state.pixelKind = Field<std::uint32_t>(a_texture, Layout::kTextureFormat) == DXGI_FORMAT_A8_UNORM ? 4 : (Multiply() ? 6 : 5);
			const auto index = TextureIndex(a_renderer, a_texture);
			if (index < 0)
				return;
			command.textures[0] = index;
			command.samplers[0] = 3;  // Bind(0, clamp, linear)

			auto& capture = Current();
			DrawRecord record{};
			MatrixConstants(*reinterpret_cast<const float(*)[6]>(matrix.data()), record);
			CxformConstants(Field<float[8]>(a_renderer, Layout::kCxform), record);
			std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
			// Six vertices a bitmap (x, y, u, v, colour): left-top, right-top, left-bottom, then left-bottom, right-top, right-bottom.
			struct GlyphVertex
			{
				float x, y, u, v;
				std::uint32_t color;
			};
			static_assert(sizeof(GlyphVertex) == 20);
			std::vector<GlyphVertex> vertices;
			vertices.reserve(static_cast<std::size_t>(a_count) * 6);
			for (std::int32_t i = 0; i < a_count; ++i) {
				const BitmapDesc& b = bitmaps[i];
				const GlyphVertex lt{ b.coords[0], b.coords[1], b.texCoords[0], b.texCoords[1], b.color };
				const GlyphVertex rt{ b.coords[2], b.coords[1], b.texCoords[2], b.texCoords[1], b.color };
				const GlyphVertex lb{ b.coords[0], b.coords[3], b.texCoords[0], b.texCoords[3], b.color };
				const GlyphVertex rb{ b.coords[2], b.coords[3], b.texCoords[2], b.texCoords[3], b.color };
				vertices.insert(vertices.end(), { lt, rt, lb, lb, rt, rb });
			}
			record.vertexOffset = capture.display.Append(vertices.data(), vertices.size() * sizeof(GlyphVertex), 4);
			record.vertexStride = sizeof(GlyphVertex);
			record.indexSize = 0;
			command.vertexCount = static_cast<std::uint32_t>(vertices.size());
			Emit(command, record);
		}

		void BeginSubmitMask(void* a_renderer, std::int32_t a_mode)
		{
			if (!Capturing())
				return Original<MaskFn>(kBeginSubmitMask)(a_renderer, a_mode);
			Keep([a_mode](void* a_self) { Original<MaskFn>(kBeginSubmitMask)(a_self, a_mode); });
			if (!VideoModeSet(a_renderer))
				return;
			auto& capture = Current();
			capture.inMask = true;
			switch (a_mode) {
			case 0:
				capture.display.commands.push_back({ .kind = Command::Kind::ClearStencil });
				capture.stencil = Stencil::Clear;
				capture.stencilRef = 1;
				capture.maskLevel = 1;
				break;
			case 1:
				capture.stencil = Stencil::Increment;
				capture.stencilRef = static_cast<std::uint32_t>(capture.maskLevel++);
				break;
			case 2:
				capture.stencil = Stencil::Decrement;
				capture.stencilRef = static_cast<std::uint32_t>(capture.maskLevel--);
				break;
			default:
				break;
			}
		}

		void EndSubmitMask(void* a_renderer)
		{
			if (!Capturing())
				return Original<VoidFn>(kEndSubmitMask)(a_renderer);
			Keep([](void* a_self) { Original<VoidFn>(kEndSubmitMask)(a_self); });
			if (!VideoModeSet(a_renderer))
				return;
			auto& capture = Current();
			capture.inMask = false;
			capture.stencil = Stencil::Test;
			capture.stencilRef = static_cast<std::uint32_t>(capture.maskLevel);
		}

		void DisableMask(void* a_renderer)
		{
			if (!Capturing())
				return Original<VoidFn>(kDisableMask)(a_renderer);
			Keep([](void* a_self) { Original<VoidFn>(kDisableMask)(a_self); });
			if (!VideoModeSet(a_renderer))
				return;
			auto& capture = Current();
			capture.inMask = false;
			capture.maskLevel = 0;
			capture.stencil = Stencil::Off;
			capture.stencilRef = 0;
		}

		/** @brief Whether the object's vtable is GRendererD3D1x's: the slots CS replaces are the functions R5 names. */
		bool IsD3D1xRenderer(void* const* a_vtable)
		{
			const std::pair<Slot, REL::RelocationID> checks[] = {
				{ kBeginDisplay, REL::RelocationID(82819, 84942) },
				{ kEndDisplay, REL::RelocationID(82841, 84962) },
				{ kDrawIndexedTriList, REL::RelocationID(82838, 84959) },
				{ kDrawBitmaps, REL::RelocationID(82835, 84956) },
				{ kBeginSubmitMask, REL::RelocationID(82820, 84943) },
			};
			for (const auto& [slot, id] : checks)
				if (reinterpret_cast<std::uintptr_t>(a_vtable[slot]) != id.address())
					return false;
			return true;
		}

		/** @brief CS's vtable on the renderer object: a copy of its own with CS's slots. */
		void Wrap(void* a_renderer)
		{
			auto** vtable = *static_cast<void***>(a_renderer);
			if (!IsD3D1xRenderer(vtable)) {
				logger::warn("[ORG] Scaleform: the renderer is not the GRendererD3D1x CS knows; the D3D11 renderer draws");
				return;
			}
			std::memcpy(g_original.data(), vtable - 1, sizeof(g_original));
			g_vtable = g_original;
			auto set = [](Slot a_slot, auto a_function) { g_vtable[a_slot + 1] = reinterpret_cast<void*>(a_function); };
			set(kSetDisplayRenderTarget, &SetDisplayRenderTarget);
			set(kPushRenderTarget, &PushRenderTarget);
			set(kPopRenderTarget, &PopRenderTarget);
			set(kPushTempRenderTarget, &PushTempRenderTarget);
			set(kBeginDisplay, &BeginDisplay);
			set(kEndDisplay, &EndDisplay);
			set(kSetMatrix, &MatrixSetter<kSetMatrix, 6>);
			set(kSetUserMatrix, &MatrixSetter<kSetUserMatrix, 6>);
			set(kSetCxform, &MatrixSetter<kSetCxform, 8>);
			set(kPushBlendMode, &PushBlendMode);
			set(kPopBlendMode, &PopBlendMode);
			set(kSetPerspective3D, &MatrixSetter<kSetPerspective3D, 16>);
			set(kSetView3D, &MatrixSetter<kSetView3D, 16>);
			set(kSetWorld3D, &SetWorld3D);
			set(kSetVertexData, &DataSetter<kSetVertexData>);
			set(kSetIndexData, &DataSetter<kSetIndexData>);
			set(kDrawIndexedTriList, &DrawIndexedTriList);
			set(kDrawLineStrip, &DrawLineStrip);
			set(kLineStyleDisable, &StyleDisable<kLineStyleDisable>);
			set(kLineStyleColor, &StyleColor<kLineStyleColor>);
			set(kFillStyleDisable, &StyleDisable<kFillStyleDisable>);
			set(kFillStyleColor, &StyleColor<kFillStyleColor>);
			set(kFillStyleBitmap, &FillStyleBitmap);
			set(kFillStyleGouraud, &FillStyleGouraud);
			set(kDrawBitmaps, &DrawBitmaps);
			set(kBeginSubmitMask, &BeginSubmitMask);
			set(kEndSubmitMask, &EndSubmitMask);
			set(kDisableMask, &DisableMask);
			set(kDrawBlurRect, &DrawBlurRect);
			set(kDrawColorMatrixRect, &DrawColorMatrixRect);
			*static_cast<void***>(a_renderer) = g_vtable.data() + 1;
			logger::info("[ORG] Scaleform: the renderer has CS's vtable; displays are drawn on the render graph once it is up");
		}

		/** @brief The renderer's factory (SE 82831 / AE 84952, called by BSScaleformManager::InitRenderer, SE 80603 / AE 82730). */
		struct CreateRenderer
		{
			static void* thunk()
			{
				void* renderer = func();
				if (renderer)
					Wrap(renderer);
				return renderer;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Requested()
	{
		return Switches::On("CS_ORG_SCALEFORM");
	}

	void Install()
	{
		if (!Requested())
			return;
#if !defined(CS_HAS_RENDER_GRAPH)
		logger::warn("[ORG] Scaleform: CS_ORG_SCALEFORM=1, but CS was built without the render graph; the D3D11 renderer draws");
		return;
#endif
		if (!Passes::Reserve())
			return;
		logger::info("[ORG] Scaleform: CS_ORG_SCALEFORM=1, hooking the renderer's factory");
		stl::detour_thunk<CreateRenderer>(REL::RelocationID(82831, 84952));
		GfxTextures::Install();
	}
}
