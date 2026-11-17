#include "RenderGraph/Engine/Scaleform/Scaleform.h"

#include "Globals.h"
#include "RenderGraph/Engine/Scaleform/Display.h"
#include "RenderGraph/Engine/Scaleform/GRendererLayout.h"
#include "RenderGraph/Engine/Scaleform/GfxTextures.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Scaleform/Passes.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Switches.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
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
		using DrawTrianglesFn = void (*)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t);
		using DrawLinesFn = void (*)(void*, std::int32_t, std::int32_t);
		using DrawBitmapsFn = void (*)(void*, const BitmapDesc*, std::int32_t, std::int32_t, std::int32_t, RE::GTexture*, const float*, void*);
		using MaskFn = void (*)(void*, std::int32_t);
		using BeginDisplayFn = void (*)(void*, const std::uint32_t*, const Viewport*, float, float, float, float);
		using DisplayTargetFn = void (*)(void*, void*, bool);
		using PushTargetFn = void (*)(void*, const float*, void*);
		using PushTempTargetFn = RE::GTexture* (*)(void*, const float*, std::uint32_t, std::uint32_t, bool);
		using BlurFn = void (*)(void*, RE::GTexture*, const float*, const float*, const void*);
		using ColorMatrixFn = void (*)(void*, RE::GTexture*, const float*, const float*, const float*);

		/**
		 * @brief The census (docs/parity/scaleform.md, "Census"): what became of each display, and every draw or segment left out by
		 * reason. A display is the graph's (every draw on the graph), on the graph with draws left out, or not on the graph (refused
		 * whole: the D3D11 renderer gets it, which draws nothing in native mode or under the flush latch). Any thread.
		 */
		struct Counters
		{
			std::mutex mutex;
			std::uint64_t graph = 0;
			std::uint64_t mixed = 0;
			std::uint64_t lost = 0;
			std::uint64_t segments = 0;                    // graph segments drawn
			std::uint64_t scratchSegments = 0;             // of them, into a scratch image (a GFx render target or a filter pass)
			std::map<std::string, std::uint64_t> reasons;  // left out (a whole display counts once)
		};

		Counters& Count()
		{
			static Counters s_counters;
			return s_counters;
		}

		void Report(Counters& a_counters)
		{
			const std::uint64_t total = a_counters.graph + a_counters.mixed + a_counters.lost;
			if (total % kReportEvery)
				return;
			std::string reasons;
			for (const auto& [reason, count] : a_counters.reasons)
				reasons += fmt::format("{}{} {}", reasons.empty() ? "" : ", ", count, reason);
			logger::info(
				"[ORG] Scaleform census: {} displays: {} on the graph, {} with draws left out, {} not on the graph; {} graph segments ({} into scratch images: GFx "
				"render targets and filter passes); left out: {}",
				total, a_counters.graph, a_counters.mixed, a_counters.lost, a_counters.segments, a_counters.scratchSegments, reasons.empty() ? "none" : reasons);
		}

		/** @brief Something of a display (or a whole one) the graph does not draw, and why: logged the first time a reason comes up. */
		void NoteLost(const char* a_reason)
		{
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			if (counters.reasons[a_reason]++ == 0)
				logger::warn("[ORG] Scaleform: not drawn on the graph: {}", a_reason);
		}

		/**
		 * @brief A display without a video mode: the D3D11 renderer's BeginDisplay returns at once and every draw skips (SE 82819 tests
		 * +0xC0 first), so it draws nothing either way. Counted as on the graph.
		 */
		void CountEmpty()
		{
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			++counters.graph;
			Report(counters);
		}

		/** @brief A display that never began a capture: the D3D11 renderer gets it whole. */
		void CountLost(const char* a_reason)
		{
			NoteLost(a_reason);
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			++counters.lost;
			Report(counters);
		}

		/**
		 * @brief A vertex or index array a segment sets. GFx's memory is valid while it is set (the D3D11 renderer reads it at every
		 * draw); the copy in the segment's staging bytes is made at the first draw there that reads it.
		 */
		struct ArrayUse
		{
			const void* source = nullptr;
			std::int32_t count = 0;
			std::uint32_t elementSize = 0;
			std::uint32_t staging = kNoOffset;
		};

		/** @brief The display being captured. Render thread only. */
		struct Capture
		{
			bool active = false;
			Display display;  // the segment being captured
			// The arrays the segment's draws read, each copied into its staging bytes once.
			std::vector<ArrayUse> arrays;
			std::int32_t vertexArray = -1;  // the ArrayUse the renderer's vertex fields hold now
			std::int32_t indexArray = -1;   // and its index fields
			// The mirrored state.
			std::uint32_t blendMode = 0;
			// The renderer's blend stack (+0x728): the mode before each push still on it.
			std::vector<std::uint32_t> blendStack;
			bool inMask = false;
			std::int32_t maskLevel = 0;
			Stencil stencil = Stencil::Off;
			std::uint32_t stencilRef = 0;
			float viewMatrix[6]{};  // the user matrix times the viewport matrix
			float texgen[4][4]{};   // the vertex constants' texgen rows, as the renderer keeps them between draws
			std::unordered_map<ID3D11ShaderResourceView*, std::int32_t> textureIndex;
			/**
			 * @brief A render target GFx pushed inside the display (PushRenderTarget), drawn into a scratch image on the graph: what the
			 * segment it interrupted draws onto, and the state PopRenderTarget gives back.
			 */
			struct TargetFrame
			{
				float viewport[4];
				float viewMatrix[6];
				std::int32_t scratch;  // the interrupted segment's target: a scratch image, or -1 for the display's target
				bool inMask;
				std::int32_t maskLevel;
				Stencil stencil;
				std::uint32_t stencilRef;
			};
			std::vector<TargetFrame> targets;
			/**
			 * @brief A texture of GFx's drawn into a scratch image in this display: draws that read the texture read the scratch image, with
			 * the coordinates GFx made for the texture's size scaled to the scratch image's.
			 */
			struct Redirect
			{
				RE::GTexture* texture;
				std::int32_t scratch;
				float scaleU, scaleV;
				std::uint64_t serial;  // the oldest is reused first
			};
			std::vector<Redirect> redirects;
			std::uint64_t redirectSerial = 0;
			std::uint32_t pinnedScratch = 0;  // scratch images a filter holds between its passes (a bit each)
			// The display's fate so far: what was left out, and the segments drawn.
			std::uint32_t leftOut = 0;
			std::uint32_t graphSegments = 0;
			// The pixel stage the renderer has bound, for a draw with the fill off (ApplyFillStyle sets nothing then): its kind,
			// its textures and samplers, and its constants. pixelKind 0: none known yet; nullShader: a bitmap fill without a texture
			// bound a null pixel shader.
			struct BoundFill
			{
				std::uint8_t pixelKind = 0;
				bool nullShader = false;
				RE::GTexture* textures[2] = { nullptr, nullptr };
				std::uint8_t samplers[2] = { 0, 0 };
				float cxmul[4]{};
				float cxadd[4]{};
			} boundFill;
			std::uint8_t boundVertexKind = 1;  // the vertex shader the renderer has bound (DrawIndexedTriList keeps it for some draws)
		};

		Capture& Current()
		{
			static Capture s_capture;
			return s_capture;
		}

		/** @brief Whether calls are captured now: a display is, on the render owner. */
		bool Capturing()
		{
			return Current().active && FrameAnchors::IsRenderOwner();
		}

		/** @brief A new ArrayUse for the array the renderer's fields describe; its index. */
		std::int32_t UseArray(const void* a_source, std::int32_t a_count, std::uint32_t a_elementSize)
		{
			auto& arrays = Current().arrays;
			arrays.push_back({ a_source, a_count, a_elementSize, kNoOffset });
			return static_cast<std::int32_t>(arrays.size() - 1);
		}

		std::uint32_t IndexSize(std::uint32_t a_dxgiFormat)
		{
			return a_dxgiFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
		}

		/** @brief The segment's commands and arrays dropped. */
		void ClearSegment()
		{
			auto& capture = Current();
			capture.arrays.clear();
			capture.vertexArray = capture.indexArray = -1;
			capture.display.ResetCommands();
			capture.textureIndex.clear();
		}

		/** @brief A new segment of the display (after a render target was pushed or popped, or a filter's pass). */
		void StartSegment(void* a_renderer)
		{
			auto& capture = Current();
			ClearSegment();
			// The renderer's texgen rows are the ones its last draw left.
			std::memcpy(capture.texgen, &Field<float>(a_renderer, Layout::kTexgen), sizeof(capture.texgen));
		}

		/** @brief A draw or segment of the display the graph does not draw, counted (its reason logged the first time). */
		void NoteLeftOut(const char* a_reason)
		{
			++Current().leftOut;
			NoteLost(a_reason);
		}

		/**
		 * @brief Native mode: a display the graph refused is a mismatch between the engine and CS's frame (Frame::Driver::NoteTapError,
		 * fatal in development builds, logged once in release). No D3D11 renderer draws it in its place.
		 */
		void NoteRefused(const char* a_reason)
		{
			if (Engine::Native::Active())
				Frame::Driver::NoteTapError(Frame::Driver::TapError::Refused, Frame::SegmentId::Count, fmt::format("a Scaleform display: {}", a_reason));
		}

		/**
		 * @brief The segment drawn now by the graph; when it cannot be, it is left out (counted by the graph's reason). A refusal that is
		 * not the schedule's lag is a mismatch in native mode.
		 */
		void Flush(void*)
		{
			auto& capture = Current();
			if (capture.display.commands.empty() && !capture.display.clearTarget)
				return;
			const char* reason = nullptr;
			bool lagged = false;
			if (Passes::Submit(capture.display, reason, lagged)) {
				++capture.graphSegments;
				auto& counters = Count();
				std::scoped_lock lock(counters.mutex);
				++counters.segments;
				if (capture.display.scratch >= 0)
					++counters.scratchSegments;
				return;
			}
			reason = reason ? reason : "the graph did not draw a segment";
			NoteLeftOut(reason);
			if (!lagged)
				NoteRefused(reason);
		}

		/** @brief The display's fate, counted once its capture ends. */
		void FinishDisplay()
		{
			const auto& capture = Current();
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			if (capture.leftOut == 0)
				++counters.graph;
			else
				++counters.mixed;
			Report(counters);
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

		/** @brief The redirect of a texture of GFx's drawn into a scratch image in this display, or null. */
		const Capture::Redirect* FindRedirect(const RE::GTexture* a_texture)
		{
			for (const auto& redirect : Current().redirects)
				if (redirect.texture == a_texture)
					return &redirect;
			return nullptr;
		}

		/**
		 * @brief The display's index for a texture's view (DXVK's, referenced once per display), ScratchTexture(k) for a texture drawn
		 * into scratch image k in this display; kNoTexture, the draw left out, when there is none.
		 */
		std::int32_t TextureIndex(void*, RE::GTexture* a_texture)
		{
			auto& capture = Current();
			if (const auto* redirect = FindRedirect(a_texture))
				return ScratchTexture(redirect->scratch);
			auto* view = Field<ID3D11ShaderResourceView*>(a_texture, Layout::kTextureView);
			auto* raw = view ? Tripwire::Unwrap(view) : nullptr;
			if (!raw) {
				NoteLeftOut(view ? "a draw of a texture view the graph cannot read (a native handle)" : "a draw of a texture without a view");
				return kNoTexture;
			}
			if (const auto found = capture.textureIndex.find(raw); found != capture.textureIndex.end())
				return found->second;
			raw->AddRef();
			const auto index = static_cast<std::int32_t>(capture.display.textures.size());
			capture.display.textures.push_back(raw);
			capture.textureIndex.emplace(raw, index);
			return index;
		}

		/**
		 * @brief ApplyFillTexture: stage a_stage's texture and sampler, and its texgen rows. False, the draw left out, when it cannot be
		 * read: a texture that is not a plain GTextureD3D1x is a YUV one, which only GFx's video player makes (not linked in Skyrim).
		 */
		bool BindTexture(void* a_renderer, const FillTexture& a_fill, std::uint32_t a_stage, Command& a_command)
		{
			if (!IsPlainTexture(a_fill.texture)) {
				NoteLeftOut("a draw of a fill texture that is not a plain GTextureD3D1x (video)");
				return false;
			}
			const auto index = TextureIndex(a_renderer, a_fill.texture);
			if (index == kNoTexture)
				return false;
			a_command.textures[a_stage] = index;
			a_command.samplers[a_stage] = static_cast<std::uint8_t>((a_fill.wrapMode * 2 + a_fill.sampleMode) & 3);
			// A texture drawn into a scratch image: its coordinates scaled from the texture's size to the scratch image's.
			const auto* redirect = FindRedirect(a_fill.texture);
			const float scaleU = redirect ? redirect->scaleU : 1.0f;
			const float scaleV = redirect ? redirect->scaleV : 1.0f;
			auto& texgen = Current().texgen;
			texgen[a_stage * 2][0] = a_fill.matrix[0] * scaleU;
			texgen[a_stage * 2][1] = a_fill.matrix[1] * scaleU;
			texgen[a_stage * 2][3] = a_fill.matrix[2] * scaleU;
			texgen[a_stage * 2 + 1][0] = a_fill.matrix[3] * scaleV;
			texgen[a_stage * 2 + 1][1] = a_fill.matrix[4] * scaleV;
			texgen[a_stage * 2 + 1][3] = a_fill.matrix[5] * scaleV;
			return true;
		}

		/**
		 * @brief A two-texture gouraud fill without its second texture: ApplyFillTexture binds stage 1 only for a texture it has, so
		 * the one the display bound there last stays, with its texgen rows (inferred from the fill-off rule, not read in Ghidra). Without
		 * one in this display, stage 0's texture is read there (logged once).
		 */
		bool BindStaleSecond(void* a_renderer, Command& a_command)
		{
			const auto& bound = Current().boundFill;
			RE::GTexture* texture = bound.textures[1];
			std::uint8_t sampler = bound.samplers[1];
			if (!texture) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::info("[ORG] Scaleform: a two-texture gouraud fill without its second texture, before any second texture of its display; stage 0's is read there");
				a_command.textures[1] = a_command.textures[0];
				a_command.samplers[1] = a_command.samplers[0];
				return true;
			}
			const auto index = TextureIndex(a_renderer, texture);
			if (index == kNoTexture)
				return false;
			a_command.textures[1] = index;
			a_command.samplers[1] = sampler;
			return true;
		}

		/** @brief The pixel stage a draw set, kept for a later draw with the fill off. */
		void NoteBoundFill(const FillStyle& a_fill, const Command& a_command, const DrawRecord& a_record)
		{
			auto& bound = Current().boundFill;
			bound.pixelKind = a_command.state.pixelKind;
			bound.nullShader = a_command.noColour;
			for (std::uint32_t stage = 0; stage < 2; ++stage) {
				// A stage read from the stale binding (BindStaleSecond) keeps that binding.
				if (a_command.textures[stage] != kNoTexture && !a_fill.textures[stage].texture)
					continue;
				bound.textures[stage] = a_command.textures[stage] != kNoTexture ? a_fill.textures[stage].texture : nullptr;
				bound.samplers[stage] = a_command.samplers[stage];
			}
			std::memcpy(bound.cxmul, a_record.cxmul, sizeof(bound.cxmul));
			std::memcpy(bound.cxadd, a_record.cxadd, sizeof(bound.cxadd));
		}

		/**
		 * @brief A draw with the fill off: ApplyFillStyle sets nothing, so the pixel shader, its textures and its constants are the
		 * ones the renderer's last draw bound in this display. False (nothing drawn, logged once) when no draw of the display bound one.
		 */
		bool ReuseBoundFill(void* a_renderer, Command& a_command, DrawRecord& a_record)
		{
			const auto bound = Current().boundFill;
			if (bound.pixelKind == 0) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn("[ORG] Scaleform: a draw with the fill off before any fill of its display; it draws nothing");
				return false;
			}
			a_command.state.pixelKind = bound.pixelKind;
			a_command.noColour = bound.nullShader;
			for (std::uint32_t stage = 0; stage < 2; ++stage) {
				if (!bound.textures[stage])
					continue;
				const auto index = TextureIndex(a_renderer, bound.textures[stage]);
				if (index == kNoTexture)
					return false;
				a_command.textures[stage] = index;
				a_command.samplers[stage] = bound.samplers[stage];
			}
			std::memcpy(a_record.cxmul, bound.cxmul, sizeof(a_record.cxmul));
			std::memcpy(a_record.cxadd, bound.cxadd, sizeof(a_record.cxadd));
			return true;
		}

		/** @brief ApplyFillStyle: the pixel shader, its constants and textures. False when nothing is drawn (left out, logged). */
		bool ApplyFill(void* a_renderer, const FillStyle& a_fill, std::int32_t a_vertexFormat, Command& a_command, DrawRecord& a_record)
		{
			switch (a_fill.mode) {
			case 1:
				a_command.state.pixelKind = 1;
				ColorConstant(a_fill.color, a_record.cxmul);
				break;
			case 2:
				if (!a_fill.textures[0].texture) {
					// ApplyFillStyle binds a null pixel shader: the draw writes no colour, its stencil still applies.
					a_command.state.pixelKind = 1;
					a_command.noColour = true;
					break;
				}
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
							if (a_fill.textures[1].texture) {
								if (!BindTexture(a_renderer, a_fill.textures[1], 1, a_command))
									return false;
							} else if (!BindStaleSecond(a_renderer, a_command)) {
								return false;
							}
						}
					}
					a_command.state.pixelKind = Multiply() ? kind + 4 : kind;
					CxformConstants(a_fill.cxform, a_record);
					break;
				}
			default:
				return ReuseBoundFill(a_renderer, a_command, a_record);
			}
			NoteBoundFill(a_fill, a_command, a_record);
			return true;
		}

		/**
		 * @brief The vertex shader the draw binds: in 3D (a world matrix set) the 3D twin of its kind (the renderer's index + 6), and
		 * noted as the one bound.
		 */
		void FinishVertexKind(void* a_renderer, Command& a_command)
		{
			Current().boundVertexKind = a_command.state.vertexKind;
			if (Field<const float*>(a_renderer, Layout::kWorld3D))
				a_command.state.vertexKind = static_cast<std::uint8_t>(a_command.state.vertexKind + 6);
		}

		/**
		 * @brief ApplyMatrix (SE 82817, AE 84940) for a_matrix: in 2D from the view the capture computed (BeginDisplay's, which the
		 * D3D11 renderer did not run); in 3D the renderer's own, which reads only the 3D fields and the user matrix and writes the
		 * vertex constants' CPU copy (no D3D11 call), read back from there.
		 */
		void DrawMatrix(void* a_renderer, const float* a_matrix, DrawRecord& a_record)
		{
			if (!Field<const float*>(a_renderer, Layout::kWorld3D)) {
				MatrixConstants(*reinterpret_cast<const float (*)[6]>(a_matrix), a_record);
				return;
			}
			using ApplyMatrixFn = void (*)(void*, const float*);
			static REL::Relocation<ApplyMatrixFn> applyMatrix{ REL::RelocationID(82817, 84940) };
			applyMatrix(a_renderer, a_matrix);
			std::memcpy(a_record.mvp, &Field<float>(a_renderer, Layout::kVertexConstants), sizeof(a_record.mvp));
		}

		/**
		 * @brief The array the renderer's fields hold, in the segment's staging bytes: copied once, at the segment's first draw that
		 * reads it. Its offset.
		 */
		std::uint32_t StageArray(std::int32_t& a_use, const void* a_source, std::int32_t a_count, std::uint32_t a_elementSize)
		{
			auto& capture = Current();
			if (a_use < 0 || capture.arrays[a_use].source != a_source || capture.arrays[a_use].count != a_count || capture.arrays[a_use].elementSize != a_elementSize)
				a_use = UseArray(a_source, a_count, a_elementSize);  // GFx set another array since the segment's last draw
			auto& use = capture.arrays[a_use];
			if (use.staging == kNoOffset)
				use.staging = capture.display.Append(a_source, static_cast<std::size_t>(std::max(a_count, 0)) * a_elementSize, 4);
			return use.staging;
		}

		std::uint32_t StageVertices(void* a_renderer)
		{
			return StageArray(Current().vertexArray, Field<const void*>(a_renderer, Layout::kVertexData), Field<std::int32_t>(a_renderer, Layout::kVertexCount),
				Field<std::uint32_t>(a_renderer, Layout::kVertexStride));
		}

		std::uint32_t StageIndices(void* a_renderer)
		{
			return StageArray(Current().indexArray, Field<const void*>(a_renderer, Layout::kIndexData), Field<std::int32_t>(a_renderer, Layout::kIndexCount),
				IndexSize(Field<std::uint32_t>(a_renderer, Layout::kIndexFormat)));
		}

		/** @brief A draw: its record appended (192 bytes, 16-aligned) and its command. */
		void Emit(Command& a_command, const DrawRecord& a_record)
		{
			auto& capture = Current();
			a_command.kind = Command::Kind::Draw;
			a_command.state.blend = a_command.noColour ? 7 : BlendState();
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

		/**
		 * @brief The renderer's current render target (+0x740: SetDisplayRenderTarget's, or the top of its stack) as the display's
		 * target: its RTV (+0x50) and DSV (+0x58), the views PushRenderTarget and PopRenderTarget bind. False when it has no RTV.
		 */
		bool ReadRendererTarget(void* a_renderer, Display& a_display)
		{
			auto* target = Field<std::byte*>(a_renderer, Layout::kRenderTarget);
			auto* rtv = target ? *reinterpret_cast<ID3D11RenderTargetView**>(target + Layout::kRenderTargetRtv) : nullptr;
			if (!rtv)
				return false;
			rtv = Tripwire::Unwrap(rtv);
			winrt::com_ptr<ID3D11Resource> resource;
			rtv->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return false;
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtv->GetDesc(&rtvDesc);
			a_display.target = texture.detach();
			a_display.targetFormat = rtvDesc.Format;
			if (auto* dsv = *reinterpret_cast<ID3D11DepthStencilView**>(target + Layout::kRenderTargetDsv)) {
				winrt::com_ptr<ID3D11Resource> depthResource;
				Tripwire::Unwrap(dsv)->GetResource(depthResource.put());
				if (auto depth = depthResource.try_as<ID3D11Texture2D>()) {
					D3D11_TEXTURE2D_DESC desc{};
					depth->GetDesc(&desc);
					a_display.depthFormat = desc.Format;
					a_display.depth = depth.detach();
				}
			}
			return true;
		}

		/**
		 * @brief Render target 0 and the depth-stencil UI::DrawInterface bound, from the engine's shadow state (its render target and
		 * depth-stencil indices, as its ApplyState bound them), never from D3D11: under the flush latch the context's state is stale.
		 * The objects are DXVK's, whatever the engine's tables hold. A display into a render texture of GFx's (the book's pages), or one
		 * with no target in the shadow state, draws onto the renderer's own current render target, which SetDisplayRenderTarget bound.
		 */
		bool ReadTargets(void* a_renderer, Display& a_display, bool a_renderTexture)
		{
			if (a_renderTexture)
				return ReadRendererTarget(a_renderer, a_display);
			auto* renderer = globals::game::renderer;
			auto* shadow = globals::game::shadowState;
			if (!renderer || !shadow)
				return false;
			const auto& rss = shadow->GetRuntimeData();
			const auto index = static_cast<std::uint32_t>(rss.renderTargets[0]);
			if (index >= static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL))
				return ReadRendererTarget(a_renderer, a_display);
			auto* rtv = renderer->GetRuntimeData().renderTargets[index].RTV;
			if (!rtv)
				return ReadRendererTarget(a_renderer, a_display);
			rtv = Tripwire::Unwrap(rtv);
			winrt::com_ptr<ID3D11Resource> resource;
			rtv->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return false;
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtv->GetDesc(&rtvDesc);
			a_display.target = texture.detach();
			a_display.targetFormat = rtvDesc.Format;
			if (rss.depthStencil < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL)) {
				const auto& depth = renderer->GetDepthStencilData().depthStencils[rss.depthStencil];
				if (depth.texture) {
					auto* raw = Tripwire::Unwrap(depth.texture);
					raw->AddRef();
					a_display.depth = raw;
					D3D11_TEXTURE2D_DESC desc{};
					raw->GetDesc(&desc);
					a_display.depthFormat = desc.Format;
				}
			}
			return true;
		}

		// The slots.

		/** @brief The capture's state dropped at a display's end (render thread). */
		void ClearDisplay()
		{
			auto& capture = Current();
			ClearSegment();
			capture.display.Reset();
			capture.targets.clear();
			capture.redirects.clear();
			capture.pinnedScratch = 0;
		}

		/** @brief A display that never ended (no EndDisplay before the next BeginDisplay): its last segment drawn. */
		void EndDangling(void* a_renderer)
		{
			auto& capture = Current();
			Flush(a_renderer);
			capture.active = false;
			Field<std::uint32_t>(a_renderer, Layout::kBlendMode) = capture.blendMode;
			FinishDisplay();
			ClearDisplay();
		}

		void BeginDisplay(void* a_renderer, const std::uint32_t* a_background, const Viewport* a_viewport, float a_x0, float a_x1, float a_y0, float a_y1)
		{
			auto& capture = Current();
			const auto original = Original<BeginDisplayFn>(kBeginDisplay);
			// The capture is the render owner's: another thread's display neither reads nor ends it.
			if (!FrameAnchors::IsRenderOwner()) {
				CountLost("a whole display off the render thread (a loading screen without CS_ORG_LOADING_THREAD=1)");
				original(a_renderer, a_background, a_viewport, a_x0, a_x1, a_y0, a_y1);
				return;
			}
			if (capture.active)
				EndDangling(a_renderer);
			if (!Passes::Ready()) {
				CountLost("a whole display while the graph path is off (not set up, or failed)");
				// Native mode: no D3D11 renderer draws in the graph's place.
				if (Engine::Native::Active()) {
					NoteRefused("the graph path is off (not set up, or failed)");
					return;
				}
				original(a_renderer, a_background, a_viewport, a_x0, a_x1, a_y0, a_y1);
				return;
			}
			if (!VideoModeSet(a_renderer)) {
				// The D3D11 renderer draws nothing without a video mode; its calls in this display skip as well.
				CountEmpty();
				return;
			}

			ClearDisplay();
			if (!ReadTargets(a_renderer, capture.display, (a_viewport->flags & kViewportRenderTexture) != 0)) {
				// Neither the shadow state nor the renderer has a target to draw onto.
				capture.display.Reset();
				CountLost("a whole display with no render target bound");
				if (Engine::Native::Active()) {
					NoteRefused("no render target bound");
					return;
				}
				original(a_renderer, a_background, a_viewport, a_x0, a_x1, a_y0, a_y1);
				return;
			}
			capture.active = true;
			capture.leftOut = 0;
			capture.graphSegments = 0;
			const std::uint32_t background = *a_background;
			const Viewport viewport = *a_viewport;

			// What BeginDisplay resets, and the state it starts from.
			capture.blendMode = Field<std::uint32_t>(a_renderer, Layout::kBlendMode);
			capture.blendStack.clear();
			capture.inMask = false;
			capture.maskLevel = 0;
			capture.stencil = Stencil::Off;
			capture.stencilRef = 0;
			// A fill-off draw reuses only what this display bound: an older display's textures may be gone.
			capture.boundFill = {};
			capture.boundVertexKind = 1;
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
				capture.boundFill.pixelKind = 1;
				std::memcpy(capture.boundFill.cxmul, record.cxmul, sizeof(record.cxmul));
				Emit(command, record);
			}
		}

		void EndDisplay(void* a_renderer)
		{
			auto& capture = Current();
			// The capture is the render owner's: another thread's display neither ends nor resets it.
			if (FrameAnchors::IsRenderOwner()) {
				if (capture.active) {
					// The last segment, on the graph.
					Flush(a_renderer);
					capture.active = false;
					Field<std::uint32_t>(a_renderer, Layout::kBlendMode) = capture.blendMode;
					FinishDisplay();
				}
				ClearDisplay();
			}
			// Either way: it releases the textures GFx queued for release, and the targets a D3D11 BeginDisplay took.
			Original<VoidFn>(kEndDisplay)(a_renderer);
		}

		/**
		 * @brief SetDisplayRenderTarget inside a display: the segment so far is drawn onto the old target, and the rest of the display
		 * onto the new one (the renderer's current render target, as the D3D11 renderer then draws onto it).
		 */
		void SetDisplayRenderTarget(void* a_renderer, void* a_target, bool a_setState)
		{
			auto& capture = Current();
			if (!Capturing() || !capture.targets.empty())
				return Original<DisplayTargetFn>(kSetDisplayRenderTarget)(a_renderer, a_target, a_setState);
			Flush(a_renderer);
			Original<DisplayTargetFn>(kSetDisplayRenderTarget)(a_renderer, a_target, a_setState);
			ClearSegment();
			capture.display.Reset();
			if (!ReadRendererTarget(a_renderer, capture.display)) {
				// No target to draw onto: neither has the D3D11 renderer. The rest of the display is left out.
				capture.display.scratch = Display::kLostScratch;
				NoteLeftOut("SetDisplayRenderTarget inside a display to a target without a view (the rest of the display)");
			}
			StartSegment(a_renderer);
		}

		/** @brief The scratch images in use: the targets being drawn into and the textures read from them. */
		std::uint32_t BusyScratch()
		{
			const auto& capture = Current();
			std::uint32_t busy = 0;
			const auto mark = [&](std::int32_t a_scratch) {
				if (a_scratch >= 0)
					busy |= 1u << a_scratch;
			};
			mark(capture.display.scratch);
			for (const auto& frame : capture.targets)
				mark(frame.scratch);
			for (const auto& redirect : capture.redirects)
				mark(redirect.scratch);
			return busy | capture.pinnedScratch;
		}

		/**
		 * @brief A scratch image for a render target GFx pushed, a_width by a_height of a_texture (GFx's, its size at +0x38): a free one,
		 * or the one the oldest redirect not being drawn into holds. The texture's reads go to it until it is pushed again or the display
		 * ends. Display::kLostScratch when the target is larger than the scratch images or every one is being drawn into.
		 */
		std::int32_t AcquireScratch(RE::GTexture* a_texture, std::uint32_t a_width, std::uint32_t a_height)
		{
			auto& capture = Current();
			std::uint32_t width = 0, height = 0;
			const auto count = Passes::ScratchCount();
			if (!Passes::ScratchSize(width, height) || !count || a_width > width || a_height > height)
				return Display::kLostScratch;
			std::erase_if(capture.redirects, [&](const Capture::Redirect& a_redirect) { return a_texture && a_redirect.texture == a_texture; });
			std::uint32_t busy = BusyScratch();
			std::int32_t scratch = -1;
			for (std::uint32_t i = 0; i < count && scratch < 0; ++i)
				if (!(busy >> i & 1u))
					scratch = static_cast<std::int32_t>(i);
			if (scratch < 0) {
				// The oldest texture whose scratch image is not being drawn into gives it up.
				std::uint32_t drawing = capture.pinnedScratch;
				if (capture.display.scratch >= 0)
					drawing |= 1u << capture.display.scratch;
				for (const auto& frame : capture.targets)
					if (frame.scratch >= 0)
						drawing |= 1u << frame.scratch;
				const Capture::Redirect* oldest = nullptr;
				for (const auto& redirect : capture.redirects)
					if (!(drawing >> redirect.scratch & 1u) && (!oldest || redirect.serial < oldest->serial))
						oldest = &redirect;
				if (!oldest)
					return Display::kLostScratch;
				scratch = oldest->scratch;
				std::erase_if(capture.redirects, [&](const Capture::Redirect& a_redirect) { return a_redirect.scratch == scratch; });
			}
			if (a_texture && IsPlainTexture(a_texture)) {
				const float textureWidth = static_cast<float>((std::max)(Field<std::uint32_t>(a_texture, Layout::kTextureWidth), 1u));
				const float textureHeight = static_cast<float>((std::max)(Field<std::uint32_t>(a_texture, Layout::kTextureHeight), 1u));
				capture.redirects.push_back(
					{ a_texture, scratch, textureWidth / static_cast<float>(width), textureHeight / static_cast<float>(height), ++capture.redirectSerial });
			}
			return scratch;
		}

		/**
		 * @brief PushRenderTarget (SE 82887 / AE 85004) inside a captured display: the segment so far is drawn, the D3D11 renderer
		 * keeps its own bookkeeping (its target stack, +0x740, the viewport struct, the 3D matrices through the vtable), and the draws
		 * until the matching PopRenderTarget are drawn into a scratch image on the graph in the target's place, cleared first as the
		 * renderer clears the target whole. The renderer's own transform there: the viewport (0, 0, target +0x60, target +0x64), the
		 * view matrix mapping the frame rectangle to clip space ([2 / w, 0, -1 - 2 left / w; 0, -2 / h, 1 + 2 top / h], w and h at least
		 * 1, no user matrix), the depth-stencil state off. PushTempRenderTarget makes its pooled target and pushes it through here.
		 */
		void PushRenderTarget(void* a_renderer, const float* a_frame, void* a_target)
		{
			if (!Capturing() || !a_frame || !a_target)
				return Original<PushTargetFn>(kPushRenderTarget)(a_renderer, a_frame, a_target);
			auto& capture = Current();
			Flush(a_renderer);
			Original<PushTargetFn>(kPushRenderTarget)(a_renderer, a_frame, a_target);

			Capture::TargetFrame frame{};
			std::memcpy(frame.viewport, capture.display.viewport, sizeof(frame.viewport));
			std::memcpy(frame.viewMatrix, capture.viewMatrix, sizeof(frame.viewMatrix));
			frame.scratch = capture.display.scratch;
			frame.inMask = capture.inMask;
			frame.maskLevel = capture.maskLevel;
			frame.stencil = capture.stencil;
			frame.stencilRef = capture.stencilRef;
			capture.targets.push_back(frame);

			auto* target = static_cast<std::byte*>(a_target);
			auto* texture = *reinterpret_cast<RE::GTexture**>(target + Layout::kRenderTargetTexture);
			const std::uint32_t width = texture ? Field<std::uint32_t>(target, Layout::kRenderTargetWidth) : 64;
			const std::uint32_t height = texture ? Field<std::uint32_t>(target, Layout::kRenderTargetHeight) : 64;
			const std::int32_t scratch = AcquireScratch(texture, width, height);
			if (scratch == Display::kLostScratch)
				NoteLeftOut("a render target of GFx's that no scratch image could take (its draws are left out)");

			ClearSegment();
			capture.display.scratch = scratch;
			capture.display.viewport[0] = capture.display.viewport[1] = 0.0f;
			capture.display.viewport[2] = static_cast<float>(width);
			capture.display.viewport[3] = static_cast<float>(height);
			const float frameWidth = (std::max)(a_frame[2] - a_frame[0], 1.0f);
			const float frameHeight = (std::max)(a_frame[3] - a_frame[1], 1.0f);
			capture.viewMatrix[0] = 2.0f / frameWidth;
			capture.viewMatrix[1] = 0.0f;
			capture.viewMatrix[2] = -1.0f - capture.viewMatrix[0] * a_frame[0];
			capture.viewMatrix[3] = 0.0f;
			capture.viewMatrix[4] = -2.0f / frameHeight;
			capture.viewMatrix[5] = 1.0f - capture.viewMatrix[4] * a_frame[1];
			capture.stencil = Stencil::Off;
			capture.stencilRef = 0;
			StartSegment(a_renderer);
			capture.display.clearTarget = true;
		}

		RE::GTexture* PushTempRenderTarget(void* a_renderer, const float* a_frame, std::uint32_t a_width, std::uint32_t a_height, bool a_stencil)
		{
			// The renderer pushes its pooled target through the vtable's PushRenderTarget (above).
			return Original<PushTempTargetFn>(kPushTempRenderTarget)(a_renderer, a_frame, a_width, a_height, a_stencil);
		}

		/** @brief PopRenderTarget (SE 82882 / AE 85001) of a target pushed in the capture: its segment drawn, the interrupted one goes on. */
		void PopRenderTarget(void* a_renderer)
		{
			auto& capture = Current();
			if (!Capturing() || capture.targets.empty())
				return Original<VoidFn>(kPopRenderTarget)(a_renderer);
			Flush(a_renderer);
			Original<VoidFn>(kPopRenderTarget)(a_renderer);
			const auto frame = capture.targets.back();
			capture.targets.pop_back();
			ClearSegment();
			capture.display.scratch = frame.scratch;
			std::memcpy(capture.display.viewport, frame.viewport, sizeof(frame.viewport));
			std::memcpy(capture.viewMatrix, frame.viewMatrix, sizeof(frame.viewMatrix));
			capture.inMask = frame.inMask;
			capture.maskLevel = frame.maskLevel;
			capture.stencil = frame.stencil;
			capture.stencilRef = frame.stencilRef;
			StartSegment(a_renderer);
		}

		bool CaptureBlur(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params);

		void DrawBlurRect(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params)
		{
			if (!Capturing())
				return Original<BlurFn>(kDrawBlurRect)(a_renderer, a_source, a_sourceRect, a_destRect, a_params);
			if (!a_source || !a_sourceRect || !a_destRect || !a_params || !CaptureBlur(a_renderer, a_source, a_sourceRect, a_destRect, a_params))
				NoteLeftOut("a blur, shadow, glow or bevel filter of a texture the graph cannot read");
		}

		/**
		 * @brief DrawColorMatrixRect as a graph draw: FilterVS.hlsl and FilterPS.hlsl's FILTER 23 (24 for Multiply and Darken), with
		 * the constants the D3D11 renderer writes (SE 0x140f5e840): the current matrix, the source rectangle over the texture's size,
		 * the filters' blend array, the source bound clamped and linear. A source the graph cannot read leaves it out.
		 */
		void CaptureColorMatrix(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const float* a_matrix)
		{
			auto& capture = Current();
			// The source rectangle, the destination rectangle and the matrix, copied one after the other.
			std::array<float, 28> arguments;
			std::memcpy(arguments.data(), a_sourceRect, sizeof(float) * 4);
			std::memcpy(arguments.data() + 4, a_destRect, sizeof(float) * 4);
			std::memcpy(arguments.data() + 8, a_matrix, sizeof(float) * 20);
			const float* source = arguments.data();
			const float* dest = arguments.data() + 4;
			const float* matrix = arguments.data() + 8;
			if (!FindRedirect(a_source) && (!IsPlainTexture(a_source) || !Field<void*>(a_source, Layout::kTextureResource))) {
				NoteLeftOut("a colour matrix filter of a texture the graph cannot read");
				return;
			}
			const auto index = TextureIndex(a_renderer, a_source);
			if (index == kNoTexture)
				return;

			FilterRecord record{};
			const auto* redirect = FindRedirect(a_source);
			const float width = static_cast<float>((std::max)(Field<std::uint32_t>(a_source, Layout::kTextureWidth), 1u)) / (redirect ? redirect->scaleU : 1.0f);
			const float height = static_cast<float>((std::max)(Field<std::uint32_t>(a_source, Layout::kTextureHeight), 1u)) / (redirect ? redirect->scaleV : 1.0f);
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
			DrawMatrix(a_renderer, &Field<float>(a_renderer, Layout::kMatrix), matrixRecord);
			std::memcpy(record.mvp, matrixRecord.mvp, sizeof(record.mvp));
			std::memcpy(record.cxmul, matrix, sizeof(record.cxmul));
			std::memcpy(record.cxadd, matrix + 16, sizeof(record.cxadd));

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

		/** @brief GRenderer::BlurFilterParams (17 dwords; DrawBlurRect SE 82836 reads them from its fifth argument). */
		struct BlurParams
		{
			std::uint32_t flags;
			float blurX, blurY;
			std::uint32_t passes;
			float offsetX, offsetY;
			std::uint8_t color[4];   // B, G, R, A
			std::uint8_t color2[4];  // the same
			float strength;
			float cxform[8];  // rMul, rAdd, gMul, gAdd, bMul, bAdd, aMul, aAdd
		};
		static_assert(sizeof(BlurParams) == 0x44);

		// The filter pixel shader table's slot (PS-B, 1 to 26) to FilterPS.hlsl's FILTER.
		constexpr std::array<std::uint8_t, 27> kFilterOfSlot{ 0, 1, 2, 4, 5, 3, 2, 6, 5, 7, 8, 11, 12, 10, 9, 14, 13, 15, 15, 17, 17, 19, 21, 20, 22, 23, 24 };

		/**
		 * @brief Where a slot's constants go in its 32-float PSConstants (the renderer's offset table, SE 0x141e0ac70), -1 for none, and
		 * which textures it reads: the original source (srctex) and the pass's source (tex), by stage.
		 */
		struct FilterLayout
		{
			std::int32_t offset, scolor, scolor2, srctexscale, texscale;
			std::int32_t srctex, tex;
		};

		FilterLayout LayoutOfSlot(std::uint32_t a_slot)
		{
			if (a_slot <= 16)
				return { 12, 16, 20, 24, 26, 0, 1 };
			if (a_slot <= 20)
				return { 12, 16, -1, -1, 20, -1, 0 };  // srctexscale's index is -1: see BlurConstants
			return { -1, -1, -1, -1, 12, -1, 0 };
		}

		/**
		 * @brief One pass's PSConstants as DrawBlurRect writes them (docs/development/p5/scaleform.md, "The blur family"), for the
		 * pass's source of a_width by a_height and the original source of a_originalWidth by a_originalHeight (the sizes the draws sample
		 * in: a scratch image's for a texture drawn into one).
		 */
		void BlurConstants(const BlurParams& a_params, std::uint32_t a_slot, bool a_last, float a_width, float a_height, float a_originalWidth,
			float a_originalHeight, float (&a_out)[32])
		{
			std::memset(a_out, 0, sizeof(a_out));
			const FilterLayout layout = LayoutOfSlot(a_slot);
			const float* cx = a_params.cxform;
			if (a_last) {
				const float alpha = cx[6];
				const float mul[4] = { cx[0] * alpha, cx[2] * alpha, cx[4] * alpha, alpha };
				const float add[4] = { cx[1] * alpha, cx[3] * alpha, cx[5] * alpha, cx[7] };
				for (int i = 0; i < 4; ++i) {
					a_out[4 + i] = mul[i];
					a_out[i] = add[i] * kByteToUnit;
				}
			} else {
				for (int i = 0; i < 4; ++i)
					a_out[4 + i] = 1.0f;
			}
			const float sx = static_cast<float>(static_cast<std::uint32_t>((std::max)(a_params.blurX - 1.0f, 0.0f))) * 0.5f;
			const float sy = static_cast<float>(static_cast<std::uint32_t>((std::max)(a_params.blurY - 1.0f, 0.0f))) * 0.5f;
			const float weight = 1.0f / ((2.0f * sy + 1.0f) * (2.0f * sx + 1.0f));
			if (a_slot == 21 || a_slot == 23) {
				a_out[8] = a_params.blurX > 1.0f ? sx : sy;
				a_out[11] = weight;
				a_out[layout.texscale] = a_params.blurX > 1.0f ? 1.0f / a_width : 0.0f;
				a_out[layout.texscale + 1] = a_params.blurY > 1.0f ? 1.0f / a_height : 0.0f;
			} else {
				a_out[8] = sx;
				a_out[9] = sy;
				a_out[11] = weight;
				a_out[layout.texscale] = 1.0f / a_width;
				a_out[layout.texscale + 1] = 1.0f / a_height;
			}
			if (layout.offset >= 0) {
				a_out[layout.offset] = -a_params.offsetX;
				a_out[layout.offset + 1] = -a_params.offsetY;
				// srctexscale: for slots 17-20 its index is -1, so the renderer writes the width ratio before the buffer and the height
				// ratio over cxadd.r; the shadow-only shaders see that.
				const float ratioU = a_width / a_originalWidth, ratioV = a_height / a_originalHeight;
				if (layout.srctexscale >= 0) {
					a_out[layout.srctexscale] = ratioU;
					a_out[layout.srctexscale + 1] = ratioV;
				} else {
					a_out[0] = ratioV;
				}
			}
			const auto colour = [&](const std::uint8_t (&a_bgra)[4], std::int32_t a_at) {
				if (a_at < 0)
					return;
				a_out[a_at] = static_cast<float>(a_bgra[2]) * kByteToUnit;
				a_out[a_at + 1] = static_cast<float>(a_bgra[1]) * kByteToUnit;
				a_out[a_at + 2] = static_cast<float>(a_bgra[0]) * kByteToUnit;
				a_out[a_at + 3] = static_cast<float>(a_bgra[3]) * kByteToUnit;
			};
			colour(a_params.color, layout.scolor);
			colour(a_params.color2, layout.scolor2);
		}

		/** @brief A filter draw's bytes with the blur family's 128-byte constants (Scaleform/Filter.hlsli reads the same offsets). */
		struct BlurRecord
		{
			float vertices[6][6];
			float mvp[4][4];
			float constants[32];
		};
		static_assert(offsetof(BlurRecord, mvp) == kFilterMatrixOffset && offsetof(BlurRecord, constants) == kFilterConstantsOffset);

		/**
		 * @brief One blur pass's draw: the rectangle a_dest (in the matrix's space) sampling a_rect of the source over a_width by a_height,
		 * with a_slot's shader and constants, the textures by stage, clamped and linear.
		 */
		void EmitBlurPass(void* a_renderer, const float* a_matrix, const float* a_dest, const float* a_rect, float a_width, float a_height, std::uint32_t a_slot,
			const float (&a_constants)[32], std::int32_t a_srctex, std::int32_t a_tex, std::uint8_t a_blend)
		{
			auto& capture = Current();
			BlurRecord record{};
			const float u0 = a_rect[0] / a_width, v0 = a_rect[1] / a_height, u1 = a_rect[2] / a_width, v1 = a_rect[3] / a_height;
			const float corners[4][6] = {
				{ a_dest[0], a_dest[1], 0.0f, 1.0f, u0, v0 },
				{ a_dest[2], a_dest[1], 0.0f, 1.0f, u1, v0 },
				{ a_dest[0], a_dest[3], 0.0f, 1.0f, u0, v1 },
				{ a_dest[2], a_dest[3], 0.0f, 1.0f, u1, v1 },
			};
			constexpr int kOrder[6] = { 0, 1, 2, 2, 1, 3 };
			for (int i = 0; i < 6; ++i)
				std::memcpy(record.vertices[i], corners[kOrder[i]], sizeof(record.vertices[i]));
			DrawRecord matrixRecord{};
			DrawMatrix(a_renderer, a_matrix, matrixRecord);
			std::memcpy(record.mvp, matrixRecord.mvp, sizeof(record.mvp));
			std::memcpy(record.constants, a_constants, sizeof(record.constants));

			const FilterLayout layout = LayoutOfSlot(a_slot);
			Command command{};
			command.kind = Command::Kind::Draw;
			command.state.vertexKind = 0;
			command.state.pixelKind = 0;
			command.state.filter = kFilterOfSlot[a_slot];
			command.state.blend = a_blend;
			command.state.stencil = capture.stencil;
			command.stencilRef = capture.stencilRef;
			if (layout.srctex >= 0)
				command.textures[layout.srctex] = a_srctex;
			command.textures[layout.tex] = a_tex;
			command.samplers[0] = command.samplers[1] = 3;  // Bind(stage, clamp, linear)
			command.record = capture.display.Append(&record, sizeof(record), 16);
			command.vertexCount = 6;
			capture.display.commands.push_back(command);
		}

		/**
		 * @brief DrawBlurRect (SE 82836 / AE 84957) on the graph: its passes before the last drawn into scratch images (each cleared, as
		 * the renderer's PushTempRenderTarget clears its pooled target, onto {-1, -1, 1, 1} with the identity matrix), the last onto the
		 * current target with the current matrix and a_destRect. The shaders, constants, textures and blends are the renderer's
		 * (docs/development/p5/scaleform.md, "The blur family"). False, nothing captured, for a source the graph cannot read.
		 */
		bool CaptureBlur(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params)
		{
			auto& capture = Current();
			const auto* redirect = FindRedirect(a_source);
			if (!redirect) {
				// A source the graph cannot read: the filter is left out (the caller counts it).
				if (!IsPlainTexture(a_source) || !Field<void*>(a_source, Layout::kTextureResource))
					return false;
				auto* view = Field<ID3D11ShaderResourceView*>(a_source, Layout::kTextureView);
				if (!view || !Tripwire::Unwrap(view))
					return false;
			}
			std::uint32_t scratchWidth = 0, scratchHeight = 0;
			Passes::ScratchSize(scratchWidth, scratchHeight);
			const auto params = *static_cast<const BlurParams*>(a_params);
			const float sourceRect[4] = { a_sourceRect[0], a_sourceRect[1], a_sourceRect[2], a_sourceRect[3] };
			const float destRect[4] = { a_destRect[0], a_destRect[1], a_destRect[2], a_destRect[3] };
			const std::int32_t original = TextureIndex(a_renderer, a_source);
			if (original == kNoTexture)
				return false;
			const float originalWidth = static_cast<float>((std::max)(Field<std::uint32_t>(a_source, Layout::kTextureWidth), 1u)) / (redirect ? redirect->scaleU : 1.0f);
			const float originalHeight = static_cast<float>((std::max)(Field<std::uint32_t>(a_source, Layout::kTextureHeight), 1u)) / (redirect ? redirect->scaleV : 1.0f);

			// The renderer's three parameter sets: passes alternate the first two, the last uses the third. A blur of more than 32 taps
			// blurs one axis a pass, in twice the passes.
			const bool split = params.blurX * params.blurY > 32.0f;
			BlurParams sets[3] = { params, params, params };
			if (split) {
				sets[0].blurY = 1.0f;
				sets[1].blurX = 1.0f;
				sets[2].blurX = 1.0f;
			}
			const std::uint32_t passes = (std::max)(params.passes, 1u) * (split ? 2u : 1u);
			const bool multiply = Multiply();
			std::uint32_t lastSlot = split ? (multiply ? 23 : 21) : (multiply ? 24 : 22);
			if (params.flags & 0x2) {
				if (!(params.flags & 0x400))
					lastSlot = ((~(params.flags >> 6) & 8) | 1) + ((params.flags & 0x100) ? 4 : 0) + ((params.flags & 0x4) ? 1 : 0);
				else
					lastSlot = (params.flags & 0x4) ? 18 : 17;
				if (multiply)
					lastSlot += 2;
			}

			static constexpr float kIdentity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
			static constexpr float kUnitRect[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
			std::int32_t source = original;  // the pass's source
			float sourceWidth = originalWidth, sourceHeight = originalHeight;
			std::int32_t held = -1;  // the scratch image the last intermediate pass drew into
			const std::uint32_t first = (params.flags & 0x20000) ? passes - 1 : 0;
			for (std::uint32_t pass = first; pass + 1 < passes; ++pass) {
				const auto& set = sets[pass & 1];
				const std::uint32_t slot = split ? 21 : 22;
				const auto width = static_cast<std::uint32_t>(std::ceil(sourceRect[2] - sourceRect[0]));
				const auto height = static_cast<std::uint32_t>(std::ceil(sourceRect[3] - sourceRect[1]));
				const std::int32_t scratch = AcquireScratch(nullptr, (std::max)(width, 1u), (std::max)(height, 1u));
				if (scratch == Display::kLostScratch) {
					NoteLeftOut("a filter pass that no scratch image could take (the filter is left out)");
					if (held >= 0)
						capture.pinnedScratch &= ~(1u << held);
					return true;
				}
				float constants[32];
				BlurConstants(set, slot, false, sourceWidth, sourceHeight, originalWidth, originalHeight, constants);
				// The pass as its own segment into the scratch image; the interrupted segment's target and state come back after.
				Flush(a_renderer);
				const std::int32_t interrupted = capture.display.scratch;
				float viewport[4], viewMatrix[6];
				std::memcpy(viewport, capture.display.viewport, sizeof(viewport));
				std::memcpy(viewMatrix, capture.viewMatrix, sizeof(viewMatrix));
				const Stencil stencil = capture.stencil;
				const std::uint32_t stencilRef = capture.stencilRef;
				ClearSegment();
				capture.display.scratch = scratch;
				capture.display.viewport[0] = capture.display.viewport[1] = 0.0f;
				capture.display.viewport[2] = static_cast<float>((std::max)(width, 1u));
				capture.display.viewport[3] = static_cast<float>((std::max)(height, 1u));
				// PushRenderTarget's view matrix for the frame {-1, -1, 1, 1}.
				const float unitView[6] = { 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f };
				std::memcpy(capture.viewMatrix, unitView, sizeof(unitView));
				capture.stencil = Stencil::Off;
				capture.stencilRef = 0;
				StartSegment(a_renderer);
				capture.display.clearTarget = true;
				EmitBlurPass(a_renderer, kIdentity, kUnitRect, sourceRect, sourceWidth, sourceHeight, slot, constants, original, source,
					capture.inMask ? 7 : 9);
				Flush(a_renderer);
				ClearSegment();
				capture.display.scratch = interrupted;
				std::memcpy(capture.display.viewport, viewport, sizeof(viewport));
				std::memcpy(capture.viewMatrix, viewMatrix, sizeof(viewMatrix));
				capture.stencil = stencil;
				capture.stencilRef = stencilRef;
				StartSegment(a_renderer);
				// The previous intermediate is read; this one is held for the next pass.
				if (held >= 0)
					capture.pinnedScratch &= ~(1u << held);
				held = scratch;
				capture.pinnedScratch |= 1u << scratch;
				source = ScratchTexture(scratch);
				sourceWidth = static_cast<float>(scratchWidth);
				sourceHeight = static_cast<float>(scratchHeight);
			}
			// The last pass onto the current target: the filters' blend array for the mode, nothing while a mask is drawn.
			float constants[32];
			BlurConstants(sets[2], lastSlot, true, sourceWidth, sourceHeight, originalWidth, originalHeight, constants);
			const std::uint8_t blend = capture.inMask ? 7 : static_cast<std::uint8_t>(8 + kBlendStates[capture.blendMode > 14 ? 0 : capture.blendMode]);
			EmitBlurPass(a_renderer, &Field<float>(a_renderer, Layout::kMatrix), destRect, sourceRect, sourceWidth, sourceHeight, lastSlot, constants, original,
				source, blend);
			if (held >= 0)
				capture.pinnedScratch &= ~(1u << held);
			return true;
		}

		void DrawColorMatrixRect(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const float* a_matrix)
		{
			if (!Capturing())
				return Original<ColorMatrixFn>(kDrawColorMatrixRect)(a_renderer, a_source, a_sourceRect, a_destRect, a_matrix);
			if (a_source && a_sourceRect && a_destRect && a_matrix)
				CaptureColorMatrix(a_renderer, a_source, a_sourceRect, a_destRect, a_matrix);
		}

		void PushBlendMode(void* a_renderer, std::uint32_t a_mode)
		{
			if (!Capturing())
				return Original<SetBlendFn>(kPushBlendMode)(a_renderer, a_mode);
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

		void DrawIndexedTriList(void* a_renderer, std::int32_t a_base, std::int32_t a_min, std::int32_t a_count, std::int32_t a_start, std::int32_t a_triangles)
		{
			if (!Capturing())
				return Original<DrawTrianglesFn>(kDrawIndexedTriList)(a_renderer, a_base, a_min, a_count, a_start, a_triangles);
			// The renderer's own early outs: nothing drawn.
			if (!VideoModeSet(a_renderer) || !Field<const void*>(a_renderer, Layout::kVertexData) || !Field<const void*>(a_renderer, Layout::kIndexData) ||
				!Field<std::uint32_t>(a_renderer, Layout::kIndexFormat) || a_triangles <= 0)
				return;
			const auto& fill = Field<FillStyle>(a_renderer, Layout::kFillStyle);
			const std::int32_t format = Field<std::int32_t>(a_renderer, Layout::kVertexFormat);
			auto& capture = Current();
			Command command{};
			// DrawIndexedTriList's vertex shader (SE 82838): gouraud fills by the vertex format (5 for type 3 of XY16iCF32), every
			// other fill 1, which reads the position as R16G16_SINT whatever the format (XY32f too). A gouraud fill of another
			// format sets nothing: the shader bound before stays.
			if (fill.mode == 3) {
				if (format == Layout::kVertexXY16iC32)
					command.state.vertexKind = 3;
				else if (format == Layout::kVertexXY16iCF32)
					command.state.vertexKind = fill.gouraudType == 3 ? 5 : 4;
				else
					command.state.vertexKind = capture.boundVertexKind;
			} else {
				command.state.vertexKind = 1;
			}
			DrawRecord record{};
			if (!ApplyFill(a_renderer, fill, format, command, record))
				return;
			FinishVertexKind(a_renderer, command);
			DrawMatrix(a_renderer, &Field<float>(a_renderer, Layout::kMatrix), record);
			std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
			// The draws read the arrays from the segment's staging bytes, copied at their first draw there.
			const std::uint32_t indexSize = IndexSize(Field<std::uint32_t>(a_renderer, Layout::kIndexFormat));
			record.vertexOffset = StageVertices(a_renderer);
			record.vertexStride = Field<std::uint32_t>(a_renderer, Layout::kVertexStride);
			record.indexOffset = StageIndices(a_renderer) + static_cast<std::uint32_t>(a_start) * indexSize;
			record.indexSize = indexSize;
			record.baseVertex = a_base;
			command.vertexCount = static_cast<std::uint32_t>(a_triangles) * 3;
			Emit(command, record);
		}

		void DrawLineStrip(void* a_renderer, std::int32_t a_base, std::int32_t a_lines)
		{
			if (!Capturing())
				return Original<DrawLinesFn>(kDrawLineStrip)(a_renderer, a_base, a_lines);
			if (!VideoModeSet(a_renderer) || !Field<const void*>(a_renderer, Layout::kVertexData) || a_lines <= 0)
				return;
			// DrawLineStrip (SE 82839): vertex shader 1 (7 in 3D), the line style through ApplyFillStyle, the current matrix, and a
			// strip of the lines + 1 vertices from a_base.
			auto& capture = Current();
			Command command{};
			command.state.vertexKind = 1;
			command.state.lines = 1;
			DrawRecord record{};
			if (!ApplyFill(a_renderer, Field<FillStyle>(a_renderer, Layout::kLineStyle), Field<std::int32_t>(a_renderer, Layout::kVertexFormat), command, record))
				return;
			FinishVertexKind(a_renderer, command);
			DrawMatrix(a_renderer, &Field<float>(a_renderer, Layout::kMatrix), record);
			std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
			record.vertexOffset = StageVertices(a_renderer);
			record.vertexStride = Field<std::uint32_t>(a_renderer, Layout::kVertexStride);
			record.indexSize = 0;
			record.baseVertex = a_base;
			command.vertexCount = static_cast<std::uint32_t>(a_lines) + 1;
			Emit(command, record);
		}

		void DrawBitmaps(void* a_renderer, const BitmapDesc* a_bitmaps, std::int32_t a_listSize, std::int32_t a_start, std::int32_t a_count, RE::GTexture* a_texture,
			const float* a_matrix, void* a_cache)
		{
			if (!Capturing())
				return Original<DrawBitmapsFn>(kDrawBitmaps)(a_renderer, a_bitmaps, a_listSize, a_start, a_count, a_texture, a_matrix, a_cache);
			if (!VideoModeSet(a_renderer) || !a_bitmaps || !a_texture || a_count <= 0)
				return;  // the renderer draws nothing (and its state changes are the next draw's anyway)
			const auto* bitmaps = a_bitmaps + a_start;
			const bool redirected = FindRedirect(a_texture) != nullptr;
			if (!redirected && !IsPlainTexture(a_texture)) {
				NoteLeftOut("bitmaps of a texture that is not a plain GTextureD3D1x (video)");
				return;
			}
			if (!redirected && !Field<void*>(a_texture, Layout::kTextureResource)) {
				// Its data was lost (the device was reset): GFx makes it again before it is drawn again.
				NoteLeftOut("bitmaps of a texture whose data is lost");
				return;
			}
			Command command{};
			command.state.vertexKind = 2;
			FinishVertexKind(a_renderer, command);
			command.state.pixelKind = Field<std::uint32_t>(a_texture, Layout::kTextureFormat) == DXGI_FORMAT_A8_UNORM ? 4 : (Multiply() ? 6 : 5);
			const auto index = TextureIndex(a_renderer, a_texture);
			if (index == kNoTexture)
				return;
			command.textures[0] = index;
			command.samplers[0] = 3;  // Bind(0, clamp, linear)
			const auto* redirect = FindRedirect(a_texture);
			const float scaleU = redirect ? redirect->scaleU : 1.0f;
			const float scaleV = redirect ? redirect->scaleV : 1.0f;

			auto& capture = Current();
			DrawRecord record{};
			DrawMatrix(a_renderer, a_matrix, record);
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
				const float u0 = b.texCoords[0] * scaleU, v0 = b.texCoords[1] * scaleV, u1 = b.texCoords[2] * scaleU, v1 = b.texCoords[3] * scaleV;
				const GlyphVertex lt{ b.coords[0], b.coords[1], u0, v0, b.color };
				const GlyphVertex rt{ b.coords[2], b.coords[1], u1, v0, b.color };
				const GlyphVertex lb{ b.coords[0], b.coords[3], u0, v1, b.color };
				const GlyphVertex rb{ b.coords[2], b.coords[3], u1, v1, b.color };
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
			set(kPushBlendMode, &PushBlendMode);
			set(kPopBlendMode, &PopBlendMode);
			set(kDrawIndexedTriList, &DrawIndexedTriList);
			set(kDrawLineStrip, &DrawLineStrip);
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

	void Setup()
	{
		if (Requested())
			Passes::Setup();
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
