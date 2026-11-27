#include "RenderGraph/Engine/Scaleform/Scaleform.h"

#include "Snapshot/EngineReads.h"

#include "Features/HDRDisplay.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Scaleform/Display.h"
#include "RenderGraph/Engine/Scaleform/GRendererLayout.h"
#include "RenderGraph/Engine/Scaleform/GfxTextures.h"
#include "RenderGraph/Engine/Scaleform/Passes.h"
#include "RenderGraph/Engine/Scaleform/UiResources.h"
#include "RenderGraph/Engine/Scaleform/UiStream.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "Snapshot/Publication.h"
#include "Snapshot/Retire.h"
#include "Snapshot/UiCommandStream.h"
#include "Utils/Game.h"

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

		// Vtable slots CS takes. CommonLib numbers 0-45 the same way.
		enum Slot : std::size_t
		{
			kDestructor = 0,
			kGetRenderCaps = 1,
			kCreateTexture = 2,
			kCreateTextureYUV = 3,
			kCreateRenderTarget = 6,
			kSetDisplayRenderTarget = 7,
			kPushRenderTarget = 8,
			kPopRenderTarget = 9,
			kPushTempRenderTarget = 10,
			kReleaseTempRenderTargets = 11,
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
			kMakeViewAndPersp3D = 24,
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
			kCheckFilterSupport = 43,
			kDrawBlurRect = 44,
			kDrawColorMatrixRect = 45,
			kGetRenderStats = 48,
			kReleaseResources = 50,
			kSetDependentVideoMode = 53,
			kResetVideoMode = 54,
			kGetDisplayStatus = 55,
			kGetDevice = 56,
			kGetContext = 57,
		};

		// The renderer's 1/255 (DAT_141540648), used as it uses it so results match bit for bit.
		constexpr float kByteToUnit = 0.003921568859368563f;
		// GRenderer blend modes 0-14 to the renderer's blend states, from table 0x1417d4468.
		constexpr std::array<std::uint8_t, 15> kBlendStates{ 1, 1, 1, 4, 1, 5, 6, 1, 2, 3, 1, 7, 7, 1, 1 };
		constexpr std::uint32_t kBlendMultiply = 3;
		constexpr std::uint32_t kBlendDarken = 6;
		constexpr std::uint32_t kNoOffset = ~0u;
		constexpr std::uint32_t kViewportRenderTexture = 1;
		constexpr std::uint32_t kViewportScissor = 4;
		constexpr std::uint64_t kReportEvery = 1000;

		/** @brief GRenderer::BitmapDesc. */
		struct BitmapDesc
		{
			float coords[4];     // left, top, right, bottom
			float texCoords[4];  // same order
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

		/** @brief Set once native mode's vtable owns every D3D1x slot but the destructor (decision 16's condition). */
		std::atomic<bool> g_ownsRenderer{ false };

		/** @brief The D3D11 renderer's own slots. Written once, before the renderer is used. */
		std::array<void*, kSlots + 1> g_original{};
		/** @brief CS's vtable. Element 0 is the word before the original's, so the object points at element 1. */
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
		 * @brief The census of what became of each display, with left-out draws and segments counted by reason. A refused display goes
		 * to the D3D11 renderer, which draws nothing in native mode or under the flush latch. Any thread.
		 */
		struct Counters
		{
			std::mutex mutex;
			std::uint64_t graph = 0;
			std::uint64_t mixed = 0;
			std::uint64_t lost = 0;
			std::uint64_t segments = 0;
			std::uint64_t scratchSegments = 0;             // into a scratch image, for a GFx render target or a filter pass
			std::map<std::string, std::uint64_t> reasons;  // a whole display counts once
			// Seen counts: translated 3D draws, filter draws, pushed targets and display target switches inside displays.
			std::atomic<std::uint64_t> draws3D{ 0 }, blurs{ 0 }, colorMatrices{ 0 }, pushes{ 0 }, displayTargets{ 0 };
		};

		Counters& Count()
		{
			static Counters s_counters;
			return s_counters;
		}

		void ReportNativeUi();

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
				"render targets and filter passes); left out: {}; seen: {} 3D draws, {} blur filters, {} colour matrices, {} pushed targets, {} display target switches",
				total, a_counters.graph, a_counters.mixed, a_counters.lost, a_counters.segments, a_counters.scratchSegments, reasons.empty() ? "none" : reasons,
				a_counters.draws3D.load(), a_counters.blurs.load(), a_counters.colorMatrices.load(), a_counters.pushes.load(), a_counters.displayTargets.load());
			if (Native::Active())
				ReportNativeUi();
		}

		/** @brief Counts something the graph does not draw. Each reason is logged the first time. */
		void NoteLost(const char* a_reason)
		{
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			if (counters.reasons[a_reason]++ == 0)
				logger::warn("[ORG] Scaleform: not drawn on the graph: {}", a_reason);
		}

		/**
		 * @brief A display without a video mode draws nothing either way, since the D3D11 renderer tests +0xC0 first (SE 82819).
		 * Counted as on the graph.
		 */
		void CountEmpty()
		{
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			++counters.graph;
			Report(counters);
		}

		void CountLost(const char* a_reason)
		{
			NoteLost(a_reason);
			auto& counters = Count();
			std::scoped_lock lock(counters.mutex);
			++counters.lost;
			Report(counters);
		}

		/** @brief A vertex or index array a segment sets. GFx's memory is valid only while set, so it is copied at its first draw. */
		struct ArrayUse
		{
			const void* source = nullptr;
			std::int32_t count = 0;
			std::uint32_t elementSize = 0;
			std::uint32_t staging = kNoOffset;
		};

		struct Capture
		{
			bool active = false;
			Display display;
			std::vector<ArrayUse> arrays;
			std::int32_t vertexArray = -1;  // the ArrayUse the renderer's vertex fields hold now
			std::int32_t indexArray = -1;   // and its index fields
			std::uint32_t blendMode = 0;
			// The renderer's blend stack at +0x728, holding the mode before each push.
			std::vector<std::uint32_t> blendStack;
			bool inMask = false;
			std::int32_t maskLevel = 0;
			Stencil stencil = Stencil::Off;
			std::uint32_t stencilRef = 0;
			float viewMatrix[6]{};
			float texgen[4][4]{};  // the renderer keeps these between draws and displays
			std::unordered_map<ID3D11ShaderResourceView*, std::int32_t> textureIndex;
			/** @brief A render target GFx pushed inside the display, drawn into a scratch image. Holds what PopRenderTarget restores. */
			struct TargetFrame
			{
				float viewport[4];
				float viewMatrix[6];
				std::int32_t scratch;  // a scratch image, or -1 for the display's target
				bool inMask;
				std::int32_t maskLevel;
				Stencil stencil;
				std::uint32_t stencilRef;
			};
			std::vector<TargetFrame> targets;
			/** @brief A GFx texture drawn into a scratch image. Reads go to the scratch image with coordinates rescaled to its size. */
			struct Redirect
			{
				RE::GTexture* texture;
				std::int32_t scratch;
				float scaleU, scaleV;
				std::uint64_t serial;  // the oldest is reused first
			};
			std::vector<Redirect> redirects;
			std::uint64_t redirectSerial = 0;
			std::uint32_t pinnedScratch = 0;  // one bit per scratch image a filter holds between its passes
			std::uint32_t leftOut = 0;
			std::uint32_t graphSegments = 0;
			// The pixel stage last bound, for a draw with the fill off, since ApplyFillStyle sets nothing then.
			// pixelKind 0 means none known yet.
			struct BoundFill
			{
				std::uint8_t pixelKind = 0;
				bool nullShader = false;
				RE::GTexture* textures[2] = { nullptr, nullptr };
				std::uint8_t samplers[2] = { 0, 0 };
				float cxmul[4]{};
				float cxadd[4]{};
			} boundFill;
			std::uint8_t boundVertexKind = 1;  // DrawIndexedTriList keeps it for some draws
		};

		/** @brief The display being captured: guest's field path, or native's UI stream replay. */
		Capture& Current()
		{
			static Capture s_capture;
			return s_capture;
		}

		bool Capturing()
		{
			return Current().active && FrameAnchors::IsRenderOwner();
		}

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

		/** @brief Starts a new segment: after a flush, a render target push or pop, or a filter pass. The texgen rows carry over, as in D3D11. */
		void ClearSegment()
		{
			auto& capture = Current();
			capture.arrays.clear();
			capture.vertexArray = capture.indexArray = -1;
			capture.display.ResetCommands();
			capture.textureIndex.clear();
		}

		void NoteLeftOut(const char* a_reason)
		{
			++Current().leftOut;
			NoteLost(a_reason);
		}

		/** @brief Whether the UI stage holds the displays. Then no D3D11 renderer draws one in the graph's place. */
		bool StageDrawsDisplays()
		{
			return FrameAnchors::HeldByStage(FrameAnchors::Anchor::Interface);
		}

		/** @brief A display refused while a stage holds displays is a recipe mismatch. Fatal in development builds. */
		void NoteRefused(const char* a_reason)
		{
			if (StageDrawsDisplays())
				Frame::Driver::NoteTapError(Frame::Driver::TapError::Refused, Frame::SegmentId::Count, fmt::format("a Scaleform display: {}", a_reason));
		}

		/** @brief Draws the segment on the graph, or counts it left out. A refusal not caused by schedule lag is a mismatch in native mode. */
		void Flush()
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

		/** @brief ApplyColor's constant as RGBA floats. Multiply and Darken move towards white by alpha. */
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

		/** @brief ApplyCxform's constants, the multiplies then the adds over 255. */
		void CxformConstants(const float (&a_cxform)[8], DrawRecord& a_record)
		{
			for (int i = 0; i < 4; ++i) {
				a_record.cxmul[i] = a_cxform[i * 2];
				a_record.cxadd[i] = static_cast<float>(a_cxform[i * 2 + 1]) * kByteToUnit;
			}
		}

		/** @brief ApplyMatrix in 2D, computed as GMatrix2D's Prepend does. */
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

		/** @brief A texture's info: read live in guest, from the UI stream's texture table in the replay. */
		TextureInfo InfoOf(const RendererState& a_state, const RE::GTexture* a_texture)
		{
			if (!a_state.stream)
				return ReadTextureInfo(a_texture);
			if (!a_texture)
				return {};
			if (const auto found = a_state.textures->find(a_texture); found != a_state.textures->end())
				return a_state.stream->textures[found->second];
			NoteLost("a texture the UI stream did not name");
			return {};
		}

		/** @brief Whether a_texture is a plain GTextureD3D1x, whose fields are the ones read. */
		bool IsPlainTexture(const RendererState& a_state, const RE::GTexture* a_texture)
		{
			return a_texture && InfoOf(a_state, a_texture).plain;
		}

		const Capture::Redirect* FindRedirect(const RE::GTexture* a_texture)
		{
			for (const auto& redirect : Current().redirects)
				if (redirect.texture == a_texture)
					return &redirect;
			return nullptr;
		}

		/** @brief The display's index for a texture, or for its scratch image. kNoTexture leaves the draw out. */
		std::int32_t TextureIndex(const RendererState& a_state, RE::GTexture* a_texture)
		{
			auto& capture = Current();
			if (const auto* redirect = FindRedirect(a_texture))
				return ScratchTexture(redirect->scratch);
			const TextureInfo info = InfoOf(a_state, a_texture);
			auto* raw = info.view.get();
			if (!raw) {
				NoteLeftOut(info.hasView ? "a draw of a texture view the graph cannot read (a native handle)" : "a draw of a texture without a view");
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
		 * @brief ApplyFillTexture for one stage. False when the texture is not a plain GTextureD3D1x.
		 * That would be a YUV texture, which only GFx's video player makes, and Skyrim does not link it.
		 */
		bool BindTexture(const RendererState& a_state, const FillTexture& a_fill, std::uint32_t a_stage, Command& a_command)
		{
			if (!IsPlainTexture(a_state, a_fill.texture)) {
				NoteLeftOut("a draw of a fill texture that is not a plain GTextureD3D1x (video)");
				return false;
			}
			const auto index = TextureIndex(a_state, a_fill.texture);
			if (index == kNoTexture)
				return false;
			a_command.textures[a_stage] = index;
			a_command.samplers[a_stage] = static_cast<std::uint8_t>((a_fill.wrapMode * 2 + a_fill.sampleMode) & 3);
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
		 * @brief A two-texture gouraud fill without its second texture keeps whatever stage 1 held last. This is inferred from the
		 * fill-off rule, not read in Ghidra. Without one in this display, stage 0's texture is used and logged once.
		 */
		bool BindStaleSecond(const RendererState& a_state, Command& a_command)
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
			const auto index = TextureIndex(a_state, texture);
			if (index == kNoTexture)
				return false;
			a_command.textures[1] = index;
			a_command.samplers[1] = sampler;
			return true;
		}

		void NoteBoundFill(const FillStyle& a_fill, const Command& a_command, const DrawRecord& a_record)
		{
			auto& bound = Current().boundFill;
			bound.pixelKind = a_command.state.pixelKind;
			bound.nullShader = a_command.noColour;
			for (std::uint32_t stage = 0; stage < 2; ++stage) {
				// A stage read from the stale binding keeps that binding.
				if (a_command.textures[stage] != kNoTexture && !a_fill.textures[stage].texture)
					continue;
				bound.textures[stage] = a_command.textures[stage] != kNoTexture ? a_fill.textures[stage].texture : nullptr;
				bound.samplers[stage] = a_command.samplers[stage];
			}
			std::memcpy(bound.cxmul, a_record.cxmul, sizeof(bound.cxmul));
			std::memcpy(bound.cxadd, a_record.cxadd, sizeof(bound.cxadd));
		}

		/** @brief A fill-off draw reuses the pixel stage this display last bound. False and logged once when there is none. */
		bool ReuseBoundFill(const RendererState& a_state, Command& a_command, DrawRecord& a_record)
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
				const auto index = TextureIndex(a_state, bound.textures[stage]);
				if (index == kNoTexture)
					return false;
				a_command.textures[stage] = index;
				a_command.samplers[stage] = bound.samplers[stage];
			}
			std::memcpy(a_record.cxmul, bound.cxmul, sizeof(a_record.cxmul));
			std::memcpy(a_record.cxadd, bound.cxadd, sizeof(a_record.cxadd));
			return true;
		}

		/** @brief ApplyFillStyle. False when nothing is drawn. */
		bool ApplyFill(const RendererState& a_state, const FillStyle& a_fill, std::int32_t a_vertexFormat, Command& a_command, DrawRecord& a_record)
		{
			switch (a_fill.mode) {
			case 1:
				a_command.state.pixelKind = 1;
				ColorConstant(a_fill.color, a_record.cxmul);
				break;
			case 2:
				if (!a_fill.textures[0].texture) {
					// ApplyFillStyle binds a null pixel shader here. The draw writes no colour, but its stencil still applies.
					a_command.state.pixelKind = 1;
					a_command.noColour = true;
					break;
				}
				if (!BindTexture(a_state, a_fill.textures[0], 0, a_command))
					return false;
				a_command.state.pixelKind = Multiply() ? 3 : 2;
				CxformConstants(a_fill.cxform, a_record);
				break;
			case 3:
				{
					std::uint8_t kind = a_vertexFormat == Layout::kVertexXY16iC32 ? 12 : 11;
					if (a_fill.textures[0].texture) {
						if (!BindTexture(a_state, a_fill.textures[0], 0, a_command))
							return false;
						if (static_cast<std::uint32_t>(a_fill.gouraudType) - 1u < 2u) {
							kind = 13;
						} else {
							kind = 14;
							if (a_fill.textures[1].texture) {
								if (!BindTexture(a_state, a_fill.textures[1], 1, a_command))
									return false;
							} else if (!BindStaleSecond(a_state, a_command)) {
								return false;
							}
						}
					}
					a_command.state.pixelKind = Multiply() ? kind + 4 : kind;
					CxformConstants(a_fill.cxform, a_record);
					break;
				}
			default:
				return ReuseBoundFill(a_state, a_command, a_record);
			}
			NoteBoundFill(a_fill, a_command, a_record);
			return true;
		}

		/** @brief The vertex shader the draw binds. In 3D it is the 3D twin, at the renderer's index + 6. */
		void FinishVertexKind(const RendererState& a_state, Command& a_command)
		{
			Current().boundVertexKind = a_command.state.vertexKind;
			if (a_state.world3D)
				a_command.state.vertexKind = static_cast<std::uint8_t>(a_command.state.vertexKind + 6);
		}

		/**
		 * @brief ApplyMatrix (SE 82817, AE 84940). In 2D it uses the capture's view, since the D3D11 BeginDisplay did not run.
		 * In 3D the replay runs CS's port of it. Guest runs the renderer's own, which writes only the CPU copy of the vertex
		 * constants, and reads the result back.
		 */
		void DrawMatrix(RendererState& a_state, const float* a_matrix, DrawRecord& a_record)
		{
			const auto& matrix = *reinterpret_cast<const float (*)[6]>(a_matrix);
			if (!a_state.world3D) {
				MatrixConstants(matrix, a_record);
				return;
			}
			Count().draws3D.fetch_add(1, std::memory_order_relaxed);
			if (!a_state.renderer) {
				ApplyMatrix3D(a_state, matrix, a_record.mvp);
				return;
			}
			using ApplyMatrixFn = void (*)(void*, const float*);
			static REL::Relocation<ApplyMatrixFn> applyMatrix{ REL::RelocationID(82817, 84940) };
			applyMatrix(a_state.renderer, a_matrix);
			std::memcpy(a_record.mvp, &Field<float>(a_state.renderer, Layout::kVertexConstants), sizeof(a_record.mvp));
		}

		/** @brief Copies the array into the segment's staging bytes at its first draw, and returns its offset. */
		std::uint32_t StageArray(std::int32_t& a_use, const void* a_source, std::int32_t a_count, std::uint32_t a_elementSize)
		{
			auto& capture = Current();
			if (a_use < 0 || capture.arrays[a_use].source != a_source || capture.arrays[a_use].count != a_count || capture.arrays[a_use].elementSize != a_elementSize)
				a_use = UseArray(a_source, a_count, a_elementSize);  // GFx set another array since the last draw
			auto& use = capture.arrays[a_use];
			if (use.staging == kNoOffset)
				use.staging = capture.display.Append(a_source, static_cast<std::size_t>(std::max(a_count, 0)) * a_elementSize, 4);
			return use.staging;
		}

		std::uint32_t StageVertices(const RendererState& a_state)
		{
			return StageArray(Current().vertexArray, a_state.vertexData, a_state.vertexCount, a_state.vertexStride);
		}

		std::uint32_t StageIndices(const RendererState& a_state)
		{
			return StageArray(Current().indexArray, a_state.indexData, a_state.indexCount, IndexSize(a_state.indexFormat));
		}

		/** @brief Appends a draw's 192-byte record and its command. */
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

		/** @brief BeginDisplay's viewport clipped to the buffer, and the matrix mapping it to clip space, times the user matrix. */
		void ComputeView(const RendererState& a_state, const Viewport& a_viewport, float a_x0, float a_x1, float a_y0, float a_y1)
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
			const auto& u = a_state.userMatrix;
			const float m0 = m[0], m1 = m[1], m2 = m[2];
			auto& out = capture.viewMatrix;
			out[0] = m0 * u[0] + m[3] * u[1];
			out[3] = m[3] * u[4] + m0 * u[3];
			out[1] = m1 * u[0] + m[4] * u[1];
			out[4] = m[4] * u[4] + m1 * u[3];
			out[2] = m2 * u[0] + m[5] * u[1] + u[2];
			out[5] = m[5] * u[4] + m2 * u[3] + u[5];
		}

		/** @brief The texture and format behind an RTV, and the depth texture behind a DSV. False without a 2D texture. */
		bool ViewTargets(ID3D11RenderTargetView* a_rtv, ID3D11DepthStencilView* a_dsv, DisplayTargets& a_out)
		{
			if (!a_rtv)
				return false;
			auto* rtv = Tripwire::Unwrap(a_rtv);
			winrt::com_ptr<ID3D11Resource> resource;
			rtv->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return false;
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtv->GetDesc(&rtvDesc);
			a_out.target = std::move(texture);
			a_out.targetFormat = rtvDesc.Format;
			if (a_dsv) {
				winrt::com_ptr<ID3D11Resource> depthResource;
				Tripwire::Unwrap(a_dsv)->GetResource(depthResource.put());
				if (auto depth = depthResource.try_as<ID3D11Texture2D>()) {
					D3D11_TEXTURE2D_DESC desc{};
					depth->GetDesc(&desc);
					a_out.depthFormat = desc.Format;
					a_out.depth = std::move(depth);
				}
			}
			return true;
		}

		/**
		 * @brief A pushed target's frame as CS keeps it in native mode: the D3D1x push saves the target and the 3D matrices (SE 82887),
		 * and the pop sets them back (SE 82882).
		 */
		struct PushedFrame
		{
			void* target = nullptr;
			float view[16]{};
			float perspective[16]{};
			const float* world = nullptr;
		};

		/** @brief GFx's render target stack in native mode, which the D3D1x renderer keeps at +0x740 and +0x748. Any thread GFx renders on. */
		struct NativeTargetStack
		{
			void* current = nullptr;
			std::vector<PushedFrame> pushed;
		};

		NativeTargetStack& NativeTargets()
		{
			static NativeTargetStack s_targets;
			return s_targets;
		}

		/** @brief GFx's current render target: CS's in native mode, the renderer's +0x740 in guest. */
		void* CurrentTarget(void* a_renderer)
		{
			return Native::Active() ? NativeTargets().current : Field<void*>(a_renderer, Layout::kRenderTarget);
		}

		/** @brief A GFx render target's views: the RTV at +0x50 and the DSV at +0x58. */
		bool ReadTargetViews(void* a_target, DisplayTargets& a_out)
		{
			auto* target = static_cast<std::byte*>(a_target);
			if (!target)
				return false;
			return ViewTargets(*reinterpret_cast<ID3D11RenderTargetView**>(target + Layout::kRenderTargetRtv),
				*reinterpret_cast<ID3D11DepthStencilView**>(target + Layout::kRenderTargetDsv), a_out);
		}

		/** @brief The renderer's current render target's views. */
		bool ReadRendererTarget(void* a_renderer, DisplayTargets& a_out)
		{
			return ReadTargetViews(CurrentTarget(a_renderer), a_out);
		}

		/** @brief What ReadEngineTargets found. */
		enum class EngineTargets : std::uint8_t
		{
			Read,      // the shadow state's target and depth
			None,      // no renderer, or a target without a texture: the display has no targets
			Renderer,  // the shadow state names no target with a view: GFx's current target stands in
		};

		/**
		 * @brief The targets UI::DrawInterface bound, read from the engine's shadow state because the context's state is stale under
		 * the flush latch. At record time DrawInterface is a game-side producer (decision 16), so those are capture-point reads. A
		 * record-only stream reads them at its replay instead (a_atReplay), on the render owner.
		 */
		EngineTargets ReadEngineTargets(DisplayTargets& a_out, bool a_atReplay)
		{
			auto* renderer = a_atReplay ? CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer) : CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer);
			auto* shadow = a_atReplay ? CS_ENGINE_READ(ShadowState, RecordTime, globals::game::shadowState) : CS_ENGINE_READ(ShadowState, CapturePoint, globals::game::shadowState);
			if (!renderer || !shadow)
				return EngineTargets::None;
			const auto& rss = shadow->GetRuntimeData();
			const auto index = static_cast<std::uint32_t>(rss.renderTargets[0]);
			if (index >= static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL))
				return EngineTargets::Renderer;
			auto* rtv = renderer->GetRuntimeData().renderTargets[index].RTV;
			if (!rtv)
				return EngineTargets::Renderer;
			if (!ViewTargets(rtv, nullptr, a_out))
				return EngineTargets::None;
			if (rss.depthStencil < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL)) {
				const auto& depth = renderer->GetDepthStencilData().depthStencils[rss.depthStencil];
				if (depth.texture) {
					a_out.depth.copy_from(Tripwire::Unwrap(depth.texture));
					D3D11_TEXTURE2D_DESC desc{};
					a_out.depth->GetDesc(&desc);
					a_out.depthFormat = desc.Format;
				}
			}
			return EngineTargets::Read;
		}

		/** @brief A display's targets at record time: the engine's, or GFx's current target for a render texture display. */
		bool ReadTargets(void* a_renderer, DisplayTargets& a_out, bool a_renderTexture)
		{
			if (a_renderTexture)
				return ReadRendererTarget(a_renderer, a_out);
			switch (ReadEngineTargets(a_out, false)) {
			case EngineTargets::Read:
				return true;
			case EngineTargets::Renderer:
				return ReadRendererTarget(a_renderer, a_out);
			case EngineTargets::None:
				break;
			}
			return false;
		}

		/** @brief Makes a_targets the display's target and depth, each with a reference of its own. */
		void SetDisplayTargets(Display& a_display, const DisplayTargets& a_targets)
		{
			a_display.target = a_targets.target.get();
			a_display.target->AddRef();
			a_display.targetFormat = a_targets.targetFormat;
			if (a_targets.depth) {
				a_display.depth = a_targets.depth.get();
				a_display.depth->AddRef();
				a_display.depthFormat = a_targets.depthFormat;
			}
		}

		void ClearDisplay()
		{
			auto& capture = Current();
			ClearSegment();
			capture.display.Reset();
			capture.targets.clear();
			capture.redirects.clear();
			capture.pinnedScratch = 0;
		}

		/** @brief Ends the display in progress. Guest writes the blend mode back, so the D3D11 renderer agrees with GFx. */
		void FinishCapture(RendererState& a_state)
		{
			auto& capture = Current();
			Flush();
			capture.active = false;
			if (a_state.renderer)
				Field<std::uint32_t>(a_state.renderer, Layout::kBlendMode) = capture.blendMode;
			FinishDisplay();
		}

		enum class Begun : std::uint8_t
		{
			Captured,
			Empty,    // no video mode: the D3D11 renderer draws nothing either
			Refused,  // counted; guest's D3D11 renderer draws it unless a stage holds displays
		};

		/**
		 * @brief BeginDisplay (SE 82819 / AE 84942) on the render owner. a_targets is null when no target is bound. Draws the
		 * background, which leaves the identity matrix set.
		 */
		Begun TranslateBeginDisplay(RendererState& a_state, std::uint32_t a_background, const Viewport& a_viewport, float a_x0, float a_x1, float a_y0, float a_y1,
			const DisplayTargets* a_targets)
		{
			auto& capture = Current();
			if (capture.active) {
				// A display without EndDisplay: its last segment is drawn now.
				FinishCapture(a_state);
				ClearDisplay();
			}
			// The D3D11 BeginDisplay resets the world matrix whenever a video mode is set.
			if (a_state.videoModeSet)
				a_state.world3D = false;
			if (!Passes::Ready()) {
				CountLost("a whole display while the graph path is not set up");
				NoteRefused("the graph path is not set up");
				return Begun::Refused;
			}
			if (!a_state.videoModeSet) {
				// The D3D11 renderer draws nothing without a video mode, so this display's calls skip too.
				CountEmpty();
				return Begun::Empty;
			}

			ClearDisplay();
			if (!a_targets) {
				CountLost("a whole display with no render target bound");
				NoteRefused("no render target bound");
				return Begun::Refused;
			}
			SetDisplayTargets(capture.display, *a_targets);
			capture.active = true;
			capture.leftOut = 0;
			capture.graphSegments = 0;

			// The D3D11 BeginDisplay clears the blend stack but keeps the mode. Guest reads the mode its renderer holds.
			if (a_state.renderer)
				capture.blendMode = Field<std::uint32_t>(a_state.renderer, Layout::kBlendMode);
			capture.blendStack.clear();
			capture.inMask = false;
			capture.maskLevel = 0;
			capture.stencil = Stencil::Off;
			capture.stencilRef = 0;
			// A fill-off draw reuses only what this display bound, since an older display's textures may be gone.
			capture.boundFill = {};
			capture.boundVertexKind = 1;
			a_state.world3D = false;
			ComputeView(a_state, a_viewport, a_x0, a_x1, a_y0, a_y1);

			// The background is the frame rectangle in its colour, with the identity matrix left set.
			if (reinterpret_cast<const std::uint8_t*>(&a_background)[3] != 0) {
				std::memcpy(a_state.matrix, kIdentityMatrix, sizeof(kIdentityMatrix));
				const std::int16_t x0 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_x0));
				const std::int16_t x1 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_x1));
				const std::int16_t y0 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_y0));
				const std::int16_t y1 = static_cast<std::int16_t>(static_cast<std::int32_t>(a_y1));
				const std::int16_t corners[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
				const std::uint16_t strip[6] = { 0, 1, 2, 1, 3, 2 };
				DrawRecord record{};
				MatrixConstants(kIdentityMatrix, record);
				ColorConstant(a_background, record.cxmul);
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
			return Begun::Captured;
		}

		/** @brief EndDisplay on the render owner: the last segment is drawn. */
		void TranslateEndDisplay(RendererState& a_state)
		{
			if (Current().active)
				FinishCapture(a_state);
			ClearDisplay();
		}

		/** @brief Whether SetDisplayRenderTarget splits the display: inside a captured display, outside any pushed target. */
		bool TakesDisplayTarget()
		{
			return Capturing() && Current().targets.empty();
		}

		/**
		 * @brief SetDisplayRenderTarget inside a display, after the segment so far was drawn. The rest draws onto the new target,
		 * a_targets, or is left out when it has no view.
		 */
		void TranslateSetDisplayTarget(const DisplayTargets* a_targets)
		{
			Count().displayTargets.fetch_add(1, std::memory_order_relaxed);
			auto& capture = Current();
			ClearSegment();
			capture.display.Reset();
			if (a_targets) {
				SetDisplayTargets(capture.display, *a_targets);
			} else {
				// No target means the D3D11 renderer has none either, so the rest of the display is left out.
				capture.display.scratch = Display::kLostScratch;
				NoteLeftOut("SetDisplayRenderTarget inside a display to a target without a view (the rest of the display)");
			}
			ClearSegment();
		}

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
		 * @brief A scratch image for a pushed GFx render target. Takes a free one or the oldest redirect's.
		 * Returns kLostScratch when the target is too large or every image is being drawn into.
		 */
		std::int32_t AcquireScratch(const RendererState& a_state, RE::GTexture* a_texture, std::uint32_t a_width, std::uint32_t a_height)
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
			if (a_texture) {
				if (const TextureInfo info = InfoOf(a_state, a_texture); info.plain) {
					const float textureWidth = static_cast<float>((std::max)(info.width, 1u));
					const float textureHeight = static_cast<float>((std::max)(info.height, 1u));
					capture.redirects.push_back(
						{ a_texture, scratch, textureWidth / static_cast<float>(width), textureHeight / static_cast<float>(height), ++capture.redirectSerial });
				}
			}
			return scratch;
		}

		/** @brief A pushed render target: its texture and the size its pusher asked for (+0x38, +0x60, +0x64). */
		struct PushedTarget
		{
			RE::GTexture* texture = nullptr;
			std::uint32_t width = 64;
			std::uint32_t height = 64;
		};

		PushedTarget ReadPushedTarget(void* a_target)
		{
			auto* target = static_cast<std::byte*>(a_target);
			PushedTarget pushed;
			pushed.texture = *reinterpret_cast<RE::GTexture**>(target + Layout::kRenderTargetTexture);
			if (pushed.texture) {
				pushed.width = Field<std::uint32_t>(target, Layout::kRenderTargetWidth);
				pushed.height = Field<std::uint32_t>(target, Layout::kRenderTargetHeight);
			}
			return pushed;
		}

		/**
		 * @brief PushRenderTarget (SE 82887 / AE 85004) inside a captured display, after the segment so far was drawn. Draws until
		 * the matching pop go into a cleared scratch image on the graph.
		 */
		void TranslatePushRenderTarget(const RendererState& a_state, const float* a_frame, const PushedTarget& a_target)
		{
			Count().pushes.fetch_add(1, std::memory_order_relaxed);
			auto& capture = Current();
			Capture::TargetFrame frame{};
			std::memcpy(frame.viewport, capture.display.viewport, sizeof(frame.viewport));
			std::memcpy(frame.viewMatrix, capture.viewMatrix, sizeof(frame.viewMatrix));
			frame.scratch = capture.display.scratch;
			frame.inMask = capture.inMask;
			frame.maskLevel = capture.maskLevel;
			frame.stencil = capture.stencil;
			frame.stencilRef = capture.stencilRef;
			capture.targets.push_back(frame);

			const std::int32_t scratch = AcquireScratch(a_state, a_target.texture, a_target.width, a_target.height);
			if (scratch == Display::kLostScratch)
				NoteLeftOut("a render target of GFx's that no scratch image could take (its draws are left out)");

			ClearSegment();
			capture.display.scratch = scratch;
			capture.display.viewport[0] = capture.display.viewport[1] = 0.0f;
			capture.display.viewport[2] = static_cast<float>(a_target.width);
			capture.display.viewport[3] = static_cast<float>(a_target.height);
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
			ClearSegment();
			capture.display.clearTarget = true;
		}

		/** @brief Whether PopRenderTarget pops a target the capture pushed. */
		bool TakesPop()
		{
			return Capturing() && !Current().targets.empty();
		}

		/** @brief PopRenderTarget (SE 82882 / AE 85001), after the pushed segment was drawn. Resumes the interrupted one. */
		void TranslatePopRenderTarget()
		{
			auto& capture = Current();
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
			ClearSegment();
		}

		bool CaptureBlur(RendererState& a_state, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params);

		/** @brief DrawBlurRect inside a captured display. */
		void TranslateBlurRect(RendererState& a_state, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params)
		{
			Count().blurs.fetch_add(1, std::memory_order_relaxed);
			if (!a_source || !a_sourceRect || !a_destRect || !a_params || !CaptureBlur(a_state, a_source, a_sourceRect, a_destRect, a_params))
				NoteLeftOut("a blur, shadow, glow or bevel filter of a texture the graph cannot read");
		}

		/**
		 * @brief DrawColorMatrixRect as a graph draw with FILTER 23, or 24 for Multiply and Darken. The constants match what the
		 * D3D11 renderer writes at SE 0x140f5e840. A source the graph cannot read is left out.
		 */
		void CaptureColorMatrix(RendererState& a_state, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const float* a_matrix)
		{
			Count().colorMatrices.fetch_add(1, std::memory_order_relaxed);
			auto& capture = Current();
			std::array<float, 28> arguments;
			std::memcpy(arguments.data(), a_sourceRect, sizeof(float) * 4);
			std::memcpy(arguments.data() + 4, a_destRect, sizeof(float) * 4);
			std::memcpy(arguments.data() + 8, a_matrix, sizeof(float) * 20);
			const float* source = arguments.data();
			const float* dest = arguments.data() + 4;
			const float* matrix = arguments.data() + 8;
			const TextureInfo info = InfoOf(a_state, a_source);
			if (!FindRedirect(a_source) && (!info.plain || !info.hasData)) {
				NoteLeftOut("a colour matrix filter of a texture the graph cannot read");
				return;
			}
			const auto index = TextureIndex(a_state, a_source);
			if (index == kNoTexture)
				return;

			FilterRecord record{};
			const auto* redirect = FindRedirect(a_source);
			const float width = static_cast<float>((std::max)(info.width, 1u)) / (redirect ? redirect->scaleU : 1.0f);
			const float height = static_cast<float>((std::max)(info.height, 1u)) / (redirect ? redirect->scaleV : 1.0f);
			const float u0 = source[0] / width, v0 = source[1] / height, u1 = source[2] / width, v1 = source[3] / height;
			const float corners[4][6] = {
				{ dest[0], dest[1], 0.0f, 1.0f, u0, v0 },
				{ dest[2], dest[1], 0.0f, 1.0f, u1, v0 },
				{ dest[0], dest[3], 0.0f, 1.0f, u0, v1 },
				{ dest[2], dest[3], 0.0f, 1.0f, u1, v1 },
			};
			constexpr int kOrder[6] = { 0, 1, 2, 2, 1, 3 };
			for (int i = 0; i < 6; ++i)
				std::memcpy(record.vertices[i], corners[kOrder[i]], sizeof(record.vertices[i]));
			DrawRecord matrixRecord{};
			DrawMatrix(a_state, a_state.matrix, matrixRecord);
			std::memcpy(record.mvp, matrixRecord.mvp, sizeof(record.mvp));
			std::memcpy(record.cxmul, matrix, sizeof(record.cxmul));
			std::memcpy(record.cxadd, matrix + 16, sizeof(record.cxadd));

			Command command{};
			command.kind = Command::Kind::Draw;
			command.state.vertexKind = 0;
			command.state.pixelKind = 0;
			command.state.filter = Multiply() ? 24 : 23;
			// No blend while a mask is drawn, since the renderer leaves colour writes off then.
			command.state.blend = capture.inMask ? 7 : static_cast<std::uint8_t>(8 + kBlendStates[capture.blendMode > 14 ? 0 : capture.blendMode]);
			command.state.stencil = capture.stencil;
			command.stencilRef = capture.stencilRef;
			command.textures[0] = index;
			command.samplers[0] = command.samplers[1] = 3;  // clamp, linear
			command.record = capture.display.Append(&record, sizeof(record), 16);
			command.vertexCount = 6;
			capture.display.commands.push_back(command);
		}

		/** @brief GRenderer::BlurFilterParams, 17 dwords. DrawBlurRect (SE 82836) reads them from its fifth argument. */
		struct BlurParams
		{
			std::uint32_t flags;
			float blurX, blurY;
			std::uint32_t passes;
			float offsetX, offsetY;
			std::uint8_t color[4];   // B, G, R, A
			std::uint8_t color2[4];  // B, G, R, A
			float strength;
			float cxform[8];  // rMul, rAdd, gMul, gAdd, bMul, bAdd, aMul, aAdd
		};
		static_assert(sizeof(BlurParams) == 0x44);

		// Maps the filter pixel shader table's slot to FilterPS.hlsl's FILTER.
		constexpr std::array<std::uint8_t, 27> kFilterOfSlot{ 0, 1, 2, 4, 5, 3, 2, 6, 5, 7, 8, 11, 12, 10, 9, 14, 13, 15, 15, 17, 17, 19, 21, 20, 22, 23, 24 };

		/** @brief Where a slot's constants go in its 32-float PSConstants, from the renderer's offset table at SE 0x141e0ac70. -1 for none. */
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
				return { 12, 16, -1, -1, 20, -1, 0 };  // srctexscale is -1, see BlurConstants
			return { -1, -1, -1, -1, 12, -1, 0 };
		}

		/** @brief One pass's PSConstants as DrawBlurRect writes them. Sizes are those the draws sample, a scratch image's when redirected. */
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
				// For slots 17-20 the srctexscale index is -1, so the renderer writes the width ratio before the buffer and the height
				// ratio over cxadd.r. The shadow-only shaders read those.
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

		/** @brief A filter draw's bytes. Scaleform/Filter.hlsli reads the same offsets. */
		struct BlurRecord
		{
			float vertices[6][6];
			float mvp[4][4];
			float constants[32];
		};
		static_assert(offsetof(BlurRecord, mvp) == kFilterMatrixOffset && offsetof(BlurRecord, constants) == kFilterConstantsOffset);

		/** @brief One blur pass's draw with a_slot's shader and constants, sampled clamped and linear. */
		void EmitBlurPass(RendererState& a_state, const float* a_matrix, const float* a_dest, const float* a_rect, float a_width, float a_height, std::uint32_t a_slot,
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
			DrawMatrix(a_state, a_matrix, matrixRecord);
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
			command.samplers[0] = command.samplers[1] = 3;  // clamp, linear
			command.record = capture.display.Append(&record, sizeof(record), 16);
			command.vertexCount = 6;
			capture.display.commands.push_back(command);
		}

		/**
		 * @brief DrawBlurRect (SE 82836 / AE 84957) on the graph. Passes before the last draw into cleared scratch images, and the last
		 * draws onto the current target. False when the graph cannot read the source.
		 */
		bool CaptureBlur(RendererState& a_state, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params)
		{
			auto& capture = Current();
			const auto* redirect = FindRedirect(a_source);
			const TextureInfo info = InfoOf(a_state, a_source);
			if (!redirect && (!info.plain || !info.hasData || !info.view))
				return false;
			std::uint32_t scratchWidth = 0, scratchHeight = 0;
			Passes::ScratchSize(scratchWidth, scratchHeight);
			const auto params = *static_cast<const BlurParams*>(a_params);
			const float sourceRect[4] = { a_sourceRect[0], a_sourceRect[1], a_sourceRect[2], a_sourceRect[3] };
			const float destRect[4] = { a_destRect[0], a_destRect[1], a_destRect[2], a_destRect[3] };
			const std::int32_t original = TextureIndex(a_state, a_source);
			if (original == kNoTexture)
				return false;
			const float originalWidth = static_cast<float>((std::max)(info.width, 1u)) / (redirect ? redirect->scaleU : 1.0f);
			const float originalHeight = static_cast<float>((std::max)(info.height, 1u)) / (redirect ? redirect->scaleV : 1.0f);

			// Passes alternate the first two parameter sets and the last uses the third.
			// A blur of more than 32 taps does one axis per pass, in twice the passes.
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

			static constexpr float kUnitRect[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
			std::int32_t source = original;
			float sourceWidth = originalWidth, sourceHeight = originalHeight;
			std::int32_t held = -1;  // the scratch image the last intermediate pass drew into
			const std::uint32_t first = (params.flags & 0x20000) ? passes - 1 : 0;
			for (std::uint32_t pass = first; pass + 1 < passes; ++pass) {
				const auto& set = sets[pass & 1];
				const std::uint32_t slot = split ? 21 : 22;
				const auto width = static_cast<std::uint32_t>(std::ceil(sourceRect[2] - sourceRect[0]));
				const auto height = static_cast<std::uint32_t>(std::ceil(sourceRect[3] - sourceRect[1]));
				const std::int32_t scratch = AcquireScratch(a_state, nullptr, (std::max)(width, 1u), (std::max)(height, 1u));
				if (scratch == Display::kLostScratch) {
					NoteLeftOut("a filter pass that no scratch image could take (the filter is left out)");
					if (held >= 0)
						capture.pinnedScratch &= ~(1u << held);
					return true;
				}
				float constants[32];
				BlurConstants(set, slot, false, sourceWidth, sourceHeight, originalWidth, originalHeight, constants);
				// Each pass is its own segment. The interrupted segment's target and state come back after.
				Flush();
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
				const float unitView[6] = { 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f };
				std::memcpy(capture.viewMatrix, unitView, sizeof(unitView));
				capture.stencil = Stencil::Off;
				capture.stencilRef = 0;
				ClearSegment();
				capture.display.clearTarget = true;
				EmitBlurPass(a_state, kIdentityMatrix, kUnitRect, sourceRect, sourceWidth, sourceHeight, slot, constants, original, source,
					capture.inMask ? 7 : 9);
				Flush();
				ClearSegment();
				capture.display.scratch = interrupted;
				std::memcpy(capture.display.viewport, viewport, sizeof(viewport));
				std::memcpy(capture.viewMatrix, viewMatrix, sizeof(viewMatrix));
				capture.stencil = stencil;
				capture.stencilRef = stencilRef;
				ClearSegment();
				if (held >= 0)
					capture.pinnedScratch &= ~(1u << held);
				held = scratch;
				capture.pinnedScratch |= 1u << scratch;
				source = ScratchTexture(scratch);
				sourceWidth = static_cast<float>(scratchWidth);
				sourceHeight = static_cast<float>(scratchHeight);
			}
			float constants[32];
			BlurConstants(sets[2], lastSlot, true, sourceWidth, sourceHeight, originalWidth, originalHeight, constants);
			const std::uint8_t blend = capture.inMask ? 7 : static_cast<std::uint8_t>(8 + kBlendStates[capture.blendMode > 14 ? 0 : capture.blendMode]);
			EmitBlurPass(a_state, a_state.matrix, destRect, sourceRect, sourceWidth, sourceHeight, lastSlot, constants, original,
				source, blend);
			if (held >= 0)
				capture.pinnedScratch &= ~(1u << held);
			return true;
		}

		/** @brief PushBlendMode inside a captured display (BlendStackPush). */
		void TranslatePushBlendMode(std::uint32_t a_mode)
		{
			auto& capture = Current();
			BlendStackPush(capture.blendMode, capture.blendStack, a_mode);
		}

		/** @brief PopBlendMode inside a captured display (BlendStackPop). */
		void TranslatePopBlendMode()
		{
			auto& capture = Current();
			BlendStackPop(capture.blendMode, capture.blendStack);
		}

		void TranslateDrawIndexedTriList(RendererState& a_state, std::int32_t a_base, std::int32_t a_start, std::int32_t a_triangles)
		{
			// The renderer's own early outs.
			if (!a_state.videoModeSet || !a_state.vertexData || !a_state.indexData || !a_state.indexFormat || a_triangles <= 0)
				return;
			const auto& fill = a_state.fill;
			const std::int32_t format = a_state.vertexFormat;
			auto& capture = Current();
			Command command{};
			// DrawIndexedTriList (SE 82838) picks the vertex shader by fill. Non-gouraud fills use 1, which reads positions as R16G16_SINT
			// for any format. A gouraud fill of an unexpected format keeps the shader bound before.
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
			if (!ApplyFill(a_state, fill, format, command, record))
				return;
			FinishVertexKind(a_state, command);
			DrawMatrix(a_state, a_state.matrix, record);
			std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
			const std::uint32_t indexSize = IndexSize(a_state.indexFormat);
			record.vertexOffset = StageVertices(a_state);
			record.vertexStride = a_state.vertexStride;
			record.indexOffset = StageIndices(a_state) + static_cast<std::uint32_t>(a_start) * indexSize;
			record.indexSize = indexSize;
			record.baseVertex = a_base;
			command.vertexCount = static_cast<std::uint32_t>(a_triangles) * 3;
			Emit(command, record);
		}

		void TranslateDrawLineStrip(RendererState& a_state, std::int32_t a_base, std::int32_t a_lines)
		{
			if (!a_state.videoModeSet || !a_state.vertexData || a_lines <= 0)
				return;
			// DrawLineStrip (SE 82839) uses vertex shader 1, or 7 in 3D, and draws lines + 1 vertices from a_base.
			auto& capture = Current();
			Command command{};
			command.state.vertexKind = 1;
			command.state.lines = 1;
			DrawRecord record{};
			if (!ApplyFill(a_state, a_state.line, a_state.vertexFormat, command, record))
				return;
			FinishVertexKind(a_state, command);
			DrawMatrix(a_state, a_state.matrix, record);
			std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
			record.vertexOffset = StageVertices(a_state);
			record.vertexStride = a_state.vertexStride;
			record.indexSize = 0;
			record.baseVertex = a_base;
			command.vertexCount = static_cast<std::uint32_t>(a_lines) + 1;
			Emit(command, record);
		}

		/** @brief DrawBitmaps (SE 82835 / AE 84956). a_bitmaps starts at the first bitmap drawn. */
		void TranslateDrawBitmaps(RendererState& a_state, const BitmapDesc* a_bitmaps, std::int32_t a_count, RE::GTexture* a_texture, const float* a_matrix)
		{
			if (!a_state.videoModeSet || !a_bitmaps || !a_texture || a_count <= 0)
				return;  // the renderer draws nothing, and its state changes belong to the next draw anyway
			const bool redirected = FindRedirect(a_texture) != nullptr;
			const TextureInfo info = InfoOf(a_state, a_texture);
			if (!redirected && !info.plain) {
				NoteLeftOut("bitmaps of a texture that is not a plain GTextureD3D1x (video)");
				return;
			}
			if (!redirected && !info.hasData) {
				// The device was reset. GFx remakes the data before drawing it again.
				NoteLeftOut("bitmaps of a texture whose data is lost");
				return;
			}
			Command command{};
			command.state.vertexKind = 2;
			FinishVertexKind(a_state, command);
			command.state.pixelKind = info.format == DXGI_FORMAT_A8_UNORM ? 4 : (Multiply() ? 6 : 5);
			const auto index = TextureIndex(a_state, a_texture);
			if (index == kNoTexture)
				return;
			command.textures[0] = index;
			command.samplers[0] = 3;  // clamp, linear
			const auto* redirect = FindRedirect(a_texture);
			const float scaleU = redirect ? redirect->scaleU : 1.0f;
			const float scaleV = redirect ? redirect->scaleV : 1.0f;

			auto& capture = Current();
			DrawRecord record{};
			DrawMatrix(a_state, a_matrix, record);
			CxformConstants(a_state.cxform, record);
			std::memcpy(record.texgen, capture.texgen, sizeof(record.texgen));
			// Six vertices per bitmap, as two triangles.
			struct GlyphVertex
			{
				float x, y, u, v;
				std::uint32_t color;
			};
			static_assert(sizeof(GlyphVertex) == 20);
			std::vector<GlyphVertex> vertices;
			vertices.reserve(static_cast<std::size_t>(a_count) * 6);
			for (std::int32_t i = 0; i < a_count; ++i) {
				const BitmapDesc& b = a_bitmaps[i];
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

		void TranslateBeginSubmitMask(const RendererState& a_state, std::int32_t a_mode)
		{
			if (!a_state.videoModeSet)
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

		void TranslateEndSubmitMask(const RendererState& a_state)
		{
			if (!a_state.videoModeSet)
				return;
			auto& capture = Current();
			capture.inMask = false;
			capture.stencil = Stencil::Test;
			capture.stencilRef = static_cast<std::uint32_t>(capture.maskLevel);
		}

		void TranslateDisableMask(const RendererState& a_state)
		{
			if (!a_state.videoModeSet)
				return;
			auto& capture = Current();
			capture.inMask = false;
			capture.maskLevel = 0;
			capture.stencil = Stencil::Off;
			capture.stencilRef = 0;
		}

		// ---------------------------------------------------------------------------------------------------------------------
		// The replay: applies a UI stream's commands to the renderer state CS keeps and draws them through the translation above.

		struct Replay
		{
			RendererState state;
			std::unordered_map<const RE::GTexture*, std::uint32_t> textures;
			std::shared_ptr<const UiStream> stream;
			std::size_t cursor = 0;
		};

		Replay& Replayer()
		{
			static Replay s_replay;
			return s_replay;
		}

		template <class T>
		const T& PayloadAs(const UiStream& a_stream, const UiCommand& a_command, std::size_t a_offset = 0)
		{
			return *reinterpret_cast<const T*>(a_stream.At(a_command) + a_offset);
		}

		void ReplayCommand(Replay& a_replay, const UiStream& a_stream, const UiCommand& a_command)
		{
			auto& state = a_replay.state;
			const auto& c = a_command;
			switch (c.op) {
			case UiOp::Texture:
				a_replay.textures[c.key] = static_cast<std::uint32_t>(c.a);
				break;
			case UiOp::BeginDisplay:
				{
					const auto& args = PayloadAs<BeginDisplayArgs>(a_stream, c);
					Viewport viewport;
					std::memcpy(&viewport, args.viewport, sizeof(viewport));
					state.videoModeSet = c.b != 0;
					const DisplayTargets* targets = c.a >= 0 ? &a_stream.targets[c.a] : nullptr;
					// A record-only stream reads the engine's targets here (c): its recorded entry is GFx's current target, for a
					// shadow state that names none.
					DisplayTargets engine;
					if (c.c) {
						switch (ReadEngineTargets(engine, true)) {
						case EngineTargets::Read:
							targets = &engine;
							break;
						case EngineTargets::None:
							targets = nullptr;
							break;
						case EngineTargets::Renderer:
							break;
						}
					}
					(void)TranslateBeginDisplay(state, args.background, viewport, args.x0, args.x1, args.y0, args.y1, targets);
					break;
				}
			case UiOp::EndDisplay:
				TranslateEndDisplay(state);
				break;
			case UiOp::SetMatrix:
				std::memcpy(state.matrix, a_stream.At(c), sizeof(state.matrix));
				break;
			case UiOp::SetUserMatrix:
				std::memcpy(state.userMatrix, a_stream.At(c), sizeof(state.userMatrix));
				break;
			case UiOp::SetCxform:
				std::memcpy(state.cxform, a_stream.At(c), sizeof(state.cxform));
				break;
			case UiOp::PushBlendMode:
				TranslatePushBlendMode(static_cast<std::uint32_t>(c.a));
				break;
			case UiOp::PopBlendMode:
				TranslatePopBlendMode();
				break;
			case UiOp::SetPerspective3D:
				state.SetPerspective3D(PayloadAs<float[16]>(a_stream, c));
				break;
			case UiOp::SetView3D:
				state.SetView3D(PayloadAs<float[16]>(a_stream, c));
				break;
			case UiOp::SetWorld3D:
				state.world3D = c.size != 0;
				if (state.world3D)
					std::memcpy(state.worldMatrix, a_stream.At(c), sizeof(state.worldMatrix));
				break;
			case UiOp::SetVertexData:
				state.SetVertexData(c.c ? a_stream.At(c) : nullptr, c.a, c.b);
				break;
			case UiOp::SetIndexData:
				state.SetIndexData(c.c ? a_stream.At(c) : nullptr, c.a, c.b);
				break;
			case UiOp::DrawIndexedTriList:
				if (Capturing())
					TranslateDrawIndexedTriList(state, c.a, c.d, c.e);
				break;
			case UiOp::DrawLineStrip:
				if (Capturing())
					TranslateDrawLineStrip(state, c.a, c.b);
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
				state.FillStyleBitmap(PayloadAs<FillTexture>(a_stream, c));
				break;
			case UiOp::FillStyleGouraud:
				{
					const auto* textures = &PayloadAs<FillTexture>(a_stream, c);
					state.FillStyleGouraud(c.a, (c.b & 1) ? &textures[0] : nullptr, (c.b & 2) ? &textures[1] : nullptr, (c.b & 4) != 0);
					break;
				}
			case UiOp::DrawBitmaps:
				if (Capturing()) {
					const auto* bitmaps = c.a > 0 ? &PayloadAs<BitmapDesc>(a_stream, c) : nullptr;
					const auto* matrix = &PayloadAs<float>(a_stream, c, sizeof(BitmapDesc) * static_cast<std::size_t>((std::max)(c.a, 0)));
					TranslateDrawBitmaps(state, bitmaps, c.a, const_cast<RE::GTexture*>(c.key), matrix);
				}
				break;
			case UiOp::BeginSubmitMask:
				if (Capturing())
					TranslateBeginSubmitMask(state, c.a);
				break;
			case UiOp::EndSubmitMask:
				if (Capturing())
					TranslateEndSubmitMask(state);
				break;
			case UiOp::DisableMask:
				if (Capturing())
					TranslateDisableMask(state);
				break;
			case UiOp::DrawBlurRect:
				if (Capturing()) {
					const auto* rects = c.size ? &PayloadAs<float>(a_stream, c) : nullptr;
					TranslateBlurRect(state, const_cast<RE::GTexture*>(c.key), rects, rects ? rects + 4 : nullptr, rects ? rects + 8 : nullptr);
				}
				break;
			case UiOp::DrawColorMatrixRect:
				if (Capturing() && c.size && c.key) {
					const auto* floats = &PayloadAs<float>(a_stream, c);
					CaptureColorMatrix(state, const_cast<RE::GTexture*>(c.key), floats, floats + 4, floats + 8);
				}
				break;
			case UiOp::PushRenderTarget:
				if (Capturing() && c.size) {
					Flush();
					TranslatePushRenderTarget(state, &PayloadAs<float>(a_stream, c),
						PushedTarget{ const_cast<RE::GTexture*>(c.key), static_cast<std::uint32_t>(c.a), static_cast<std::uint32_t>(c.b) });
				}
				break;
			case UiOp::PopRenderTarget:
				if (TakesPop()) {
					Flush();
					TranslatePopRenderTarget();
				}
				break;
			case UiOp::SetDisplayRenderTarget:
				if (TakesDisplayTarget()) {
					Flush();
					TranslateSetDisplayTarget(c.a >= 0 ? &a_stream.targets[c.a] : nullptr);
				}
				break;
			}
		}

		/**
		 * @brief Starts replaying a_stream from its header, so the replay reads nothing the streams before it left (decision 19). A
		 * display the stream before left open is drawn first.
		 */
		void StartReplay(Replay& a_replay, const std::shared_ptr<const UiStream>& a_stream)
		{
			auto& capture = Current();
			if (capture.active) {
				FinishCapture(a_replay.state);
				ClearDisplay();
			}
			// The stream replayed before goes to the retire queue: the GPU may still read the views it holds.
			Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Render, std::exchange(a_replay.stream, a_stream));
			a_replay.cursor = 0;
			a_replay.textures.clear();
			a_replay.state = a_stream->header.state;
			capture.blendMode = a_stream->header.blendMode;
			capture.blendStack = a_stream->header.blendStack;
			std::memset(capture.texgen, 0, sizeof(capture.texgen));
		}

		/**
		 * @brief Replays a_stream's commands not replayed yet, through the next EndDisplay when a_oneDisplay. The uploads of this stream
		 * and the ones before it, replayed or not, are applied first. Render owner.
		 */
		void RunReplay(const std::shared_ptr<const UiStream>& a_stream, bool a_oneDisplay = false)
		{
			auto& replay = Replayer();
			if (replay.stream != a_stream)
				StartReplay(replay, a_stream);
			UiResources::ApplyUploads(a_stream->sequence);
			replay.state.stream = a_stream.get();
			replay.state.textures = &replay.textures;
			while (replay.cursor < a_stream->commands.size()) {
				const auto& command = a_stream->commands[replay.cursor++];
				ReplayCommand(replay, *a_stream, command);
				if (a_oneDisplay && command.op == UiOp::EndDisplay)
					break;
			}
		}

		// ---------------------------------------------------------------------------------------------------------------------
		// The recorder (native mode): every GRenderer call on the thread that produces the UI becomes a UI stream command, with what
		// it points at copied. Vertex and index arrays and the world matrix are copied at the first draw that reads them, since GFx's
		// memory is valid while set and the D3D11 renderer reads it at the draw.

		struct Recorder
		{
			std::shared_ptr<UiStream> stream;
			StreamHeader next;  // the header the next stream starts with
			std::uint64_t sequence = 0;
			RendererState mirror;  // what GFx set: arrays, styles, matrices, the colour transform
			bool vertexPending = false;
			bool indexPending = false;
			const float* world = nullptr;
			bool worldPending = false;
			float worldValues[16]{};
			// A repeated render's stream (cadence repeat): GFx's calls are not recorded, and this stream is replayed in their place.
			std::shared_ptr<const UiStream> repeat;
		};

		Recorder& Rec()
		{
			static Recorder s_recorder;
			return s_recorder;
		}

		/** @brief How the DrawInterface this thread runs is drawn (native mode). */
		enum class UiMode : std::uint8_t
		{
			None,        // this thread produces no UI: GFx's calls are dropped
			PerDisplay,  // the render owner records and replays each display at its EndDisplay
			RecordOnly,  // recorded only: the UI point replays the whole stream (ReplaySnapshotUi)
		};

		thread_local UiMode t_uiMode = UiMode::None;

		struct UiModeScope
		{
			explicit UiModeScope(UiMode a_mode) :
				previous(std::exchange(t_uiMode, a_mode)) {}
			~UiModeScope() { t_uiMode = previous; }
			UiModeScope(const UiModeScope&) = delete;
			UiModeScope& operator=(const UiModeScope&) = delete;
			UiMode previous;
		};

		std::atomic<std::uint64_t> g_strayCalls{ 0 };  // GFx calls on the render owner outside DrawInterface
		std::atomic<std::uint64_t> g_otherWork{ 0 };   // engine render work inside a record-only DrawInterface
		std::atomic<std::uint64_t> g_recordOnly{ 0 }, g_perDisplay{ 0 };

		/** @brief Whether GFx's setter calls update the recorder's mirror: native mode, inside a DrawInterface this thread produces. */
		bool Mirroring()
		{
			if (!Native::Active())
				return false;
			if (t_uiMode != UiMode::None)
				return true;
			// The render owner's GFx calls all come through DrawInterface. One outside it would be lost.
			if (FrameAnchors::IsRenderOwner() && g_strayCalls.fetch_add(1, std::memory_order_relaxed) == 0)
				logger::warn("[ORG] Scaleform: GFx called the renderer on the render owner outside DrawInterface; the call is not drawn (counted in the census line)");
			return false;
		}

		/** @brief Whether GFx's calls are recorded: mirroring, outside a repeated render. */
		bool Recording()
		{
			return Mirroring() && !Rec().repeat;
		}

		/**
		 * @brief The open stream, opened with the header the last one left. Streams are per DrawInterface: its end finishes the one it
		 * opened. UI producer.
		 */
		UiStream& Stream()
		{
			auto& recorder = Rec();
			if (!recorder.stream) {
				recorder.stream = std::make_shared<UiStream>();
				recorder.stream->sequence = ++recorder.sequence;
				recorder.stream->header = recorder.next;
				UiResources::OpenStream(recorder.stream->sequence);
				// The new stream carries its own copies.
				recorder.vertexPending = recorder.mirror.vertexData != nullptr;
				recorder.indexPending = recorder.mirror.indexData != nullptr;
				recorder.worldPending = recorder.world != nullptr;
			}
			return *recorder.stream;
		}

		/** @brief Finishes the open stream: it is immutable from here. Null when none is open. UI producer; nothing is replayed. */
		std::shared_ptr<const UiStream> FinishStream()
		{
			auto& recorder = Rec();
			if (!recorder.stream)
				return nullptr;
			recorder.stream->named.clear();
			recorder.next = recorder.stream->EndState();
			UiResources::CloseStream(recorder.stream->sequence);
			std::shared_ptr<const UiStream> finished = std::move(recorder.stream);
			recorder.stream.reset();
			return finished;
		}

		void RecordArrays(UiStream& a_stream, bool a_indices)
		{
			auto& recorder = Rec();
			const auto& mirror = recorder.mirror;
			if (recorder.vertexPending) {
				recorder.vertexPending = false;
				const std::size_t size = mirror.vertexData ? static_cast<std::size_t>((std::max)(mirror.vertexCount, 0)) * mirror.vertexStride : 0;
				auto& command = a_stream.Add(UiOp::SetVertexData, mirror.vertexData, size);
				command.a = mirror.vertexCount;
				command.b = mirror.vertexFormat;
				command.c = mirror.vertexData != nullptr;
			}
			if (a_indices && recorder.indexPending) {
				recorder.indexPending = false;
				const std::size_t size = mirror.indexData ? static_cast<std::size_t>((std::max)(mirror.indexCount, 0)) * IndexSize(mirror.indexFormat) : 0;
				auto& command = a_stream.Add(UiOp::SetIndexData, mirror.indexData, size);
				command.a = mirror.indexCount;
				command.b = mirror.indexFormat == DXGI_FORMAT_R16_UINT ? 1 : mirror.indexFormat == DXGI_FORMAT_R32_UINT ? 2 :
				                                                                                                          0;
				command.c = mirror.indexData != nullptr;
			}
		}

		/** @brief The world matrix GFx points at, when it changed since the stream last held it. */
		void RecordWorld(UiStream& a_stream)
		{
			auto& recorder = Rec();
			if (!recorder.world || (!recorder.worldPending && std::memcmp(recorder.worldValues, recorder.world, sizeof(recorder.worldValues)) == 0))
				return;
			recorder.worldPending = false;
			std::memcpy(recorder.worldValues, recorder.world, sizeof(recorder.worldValues));
			a_stream.Add(UiOp::SetWorld3D, recorder.worldValues, sizeof(recorder.worldValues));
		}

		/** @brief Names the textures a draw with a_style reads, by ApplyFillStyle's rules. */
		void RecordStyleTextures(UiStream& a_stream, const FillStyle& a_style)
		{
			if (a_style.mode == 2 || a_style.mode == 3)
				a_stream.NoteTexture(a_style.textures[0].texture);
			if (a_style.mode == 3 && a_style.textures[0].texture && static_cast<std::uint32_t>(a_style.gouraudType) - 1u >= 2u)
				a_stream.NoteTexture(a_style.textures[1].texture);
		}

		/** @brief The setter or call with no data. */
		UiCommand& RecordPlain(UiOp a_op)
		{
			return Stream().Add(a_op);
		}

		// ---------------------------------------------------------------------------------------------------------------------
		// The vtable slots. Guest's field path reads the D3D11 renderer's fields at each call and forwards the calls it does not
		// draw. Native mode takes every slot of the D3D1x renderer but its destructor: it records every call into the UI stream, draws
		// the stream's replay, and keeps GFx's textures, render targets, target stack and video mode itself (UiResources). The slots
		// that are GFx's base class code (frame and user-data no-ops, event handlers, MakeViewAndPersp3D) stay GFx's.

		bool NativeUi()
		{
			return Native::Active();
		}

		/** @brief Logs, with the census, how native mode's DrawInterface calls ran and CS's UI objects. */
		void ReportNativeUi()
		{
			logger::info(
				"[ORG] Scaleform: DrawInterface {} record-only (replayed at the UI point), {} per display; {} engine render calls inside a record-only "
				"DrawInterface; {} GFx calls on the render owner outside DrawInterface",
				g_recordOnly.load(std::memory_order_relaxed), g_perDisplay.load(std::memory_order_relaxed), g_otherWork.load(std::memory_order_relaxed),
				g_strayCalls.load(std::memory_order_relaxed));
			logger::info("[ORG] Scaleform: {}", UiResources::TakeStatsLine());
		}

		struct PushedTarget;
		void NativePushRenderTarget(void* a_renderer, const float* a_frame, void* a_target, const PushedTarget& a_pushed);
		void NativePopRenderTarget(void* a_renderer);

		RendererState& FieldState(void* a_renderer)
		{
			static RendererState s_state;
			RendererState::ReadFields(a_renderer, s_state);
			return s_state;
		}

		void BeginDisplay(void* a_renderer, const std::uint32_t* a_background, const Viewport* a_viewport, float a_x0, float a_x1, float a_y0, float a_y1)
		{
			const auto original = Original<BeginDisplayFn>(kBeginDisplay);
			const bool renderTexture = (a_viewport->flags & kViewportRenderTexture) != 0;
			if (NativeUi()) {
				if (!Mirroring())
					return;
				const bool videoMode = UiResources::VideoModeSet();
				if (videoMode)
					Rec().world = nullptr;  // BeginDisplay resets the world matrix
				if (!Recording())
					return;
				auto& stream = Stream();
				DisplayTargets targets;
				std::int32_t targetIndex = -1;
				// A record-only producer reads no engine state: the replay reads the engine's targets, and the stream keeps GFx's
				// current target for a shadow state that names none.
				const bool targetsAtReplay = !renderTexture && t_uiMode == UiMode::RecordOnly;
				if (targetsAtReplay ? ReadRendererTarget(a_renderer, targets) : ReadTargets(a_renderer, targets, renderTexture)) {
					targetIndex = static_cast<std::int32_t>(stream.targets.size());
					stream.targets.push_back(std::move(targets));
				}
				BeginDisplayArgs args{ *a_background, {}, a_x0, a_x1, a_y0, a_y1 };
				std::memcpy(args.viewport, a_viewport, sizeof(args.viewport));
				auto& command = stream.Add(UiOp::BeginDisplay, &args, sizeof(args));
				command.a = targetIndex;
				command.b = videoMode;
				command.c = targetsAtReplay;
				return;
			}
			// Only the render owner captures. Another thread's display neither reads nor ends it.
			if (!FrameAnchors::IsRenderOwner()) {
				CountLost("a whole display off the render owner (a loading screen)");
				original(a_renderer, a_background, a_viewport, a_x0, a_x1, a_y0, a_y1);
				return;
			}
			auto& state = FieldState(a_renderer);
			DisplayTargets targets;
			const bool haveTargets = ReadTargets(a_renderer, targets, renderTexture);
			switch (TranslateBeginDisplay(state, *a_background, *a_viewport, a_x0, a_x1, a_y0, a_y1, haveTargets ? &targets : nullptr)) {
			case Begun::Captured:
				Field<const float*>(a_renderer, Layout::kWorld3D) = nullptr;
				if (reinterpret_cast<const std::uint8_t*>(a_background)[3] != 0)
					Original<SetMatrixFn>(kSetMatrix)(a_renderer, state.matrix);
				break;
			case Begun::Refused:
				if (!StageDrawsDisplays())
					original(a_renderer, a_background, a_viewport, a_x0, a_x1, a_y0, a_y1);
				break;
			case Begun::Empty:
				break;
			}
		}

		void EndDisplay(void* a_renderer)
		{
			if (NativeUi()) {
				// CS's textures need no release queue: GFx's references and the streams' own keep them.
				if (!Mirroring())
					return;
				auto& recorder = Rec();
				const bool perDisplay = t_uiMode == UiMode::PerDisplay;
				if (recorder.repeat) {
					// A repeated render draws the snapshot's display where GFx ends its own.
					if (perDisplay)
						RunReplay(recorder.repeat, true);
					return;
				}
				RecordPlain(UiOp::EndDisplay);
				if (perDisplay)
					RunReplay(recorder.stream);
				return;
			}
			// Only the render owner captures. Another thread's display neither ends nor resets it.
			if (FrameAnchors::IsRenderOwner())
				TranslateEndDisplay(FieldState(a_renderer));
			// Always called, since it releases the textures GFx queued and the targets a D3D11 BeginDisplay took.
			Original<VoidFn>(kEndDisplay)(a_renderer);
		}

		void SetDisplayRenderTarget(void* a_renderer, void* a_target, bool a_setState)
		{
			if (NativeUi()) {
				// SE 82919 sets +0x740 and binds the target's views. CS keeps the target; the replay draws onto it.
				NativeTargets().current = a_target;
				if (!Recording())
					return;
				auto& stream = Stream();
				DisplayTargets targets;
				std::int32_t targetIndex = -1;
				if (ReadTargetViews(a_target, targets)) {
					targetIndex = static_cast<std::int32_t>(stream.targets.size());
					stream.targets.push_back(std::move(targets));
				}
				stream.Add(UiOp::SetDisplayRenderTarget).a = targetIndex;
				return;
			}
			if (!TakesDisplayTarget())
				return Original<DisplayTargetFn>(kSetDisplayRenderTarget)(a_renderer, a_target, a_setState);
			Flush();
			Original<DisplayTargetFn>(kSetDisplayRenderTarget)(a_renderer, a_target, a_setState);
			DisplayTargets targets;
			TranslateSetDisplayTarget(ReadRendererTarget(a_renderer, targets) ? &targets : nullptr);
		}

		void PushRenderTarget(void* a_renderer, const float* a_frame, void* a_target)
		{
			const bool valid = a_frame && a_target;
			const PushedTarget pushed = a_target ? ReadPushedTarget(a_target) : PushedTarget{};
			if (NativeUi())
				return NativePushRenderTarget(a_renderer, a_frame, a_target, pushed);
			if (!Capturing() || !valid)
				return Original<PushTargetFn>(kPushRenderTarget)(a_renderer, a_frame, a_target);
			Flush();
			Original<PushTargetFn>(kPushRenderTarget)(a_renderer, a_frame, a_target);
			TranslatePushRenderTarget(FieldState(a_renderer), a_frame, pushed);
		}

		RE::GTexture* PushTempRenderTarget(void* a_renderer, const float* a_frame, std::uint32_t a_width, std::uint32_t a_height, bool a_stencil)
		{
			if (NativeUi()) {
				// CS's port of the D3D1x pool, then the push.
				auto* target = UiResources::AcquireTempTarget(a_renderer, a_width, a_height, a_stencil);
				PushRenderTarget(a_renderer, a_frame, target);
				return target->target;
			}
			// The renderer's pool makes GFx's target objects and pushes its target through the vtable's PushRenderTarget above.
			return Original<PushTempTargetFn>(kPushTempRenderTarget)(a_renderer, a_frame, a_width, a_height, a_stencil);
		}

		void PopRenderTarget(void* a_renderer)
		{
			if (NativeUi())
				return NativePopRenderTarget(a_renderer);
			if (!TakesPop())
				return Original<VoidFn>(kPopRenderTarget)(a_renderer);
			Flush();
			Original<VoidFn>(kPopRenderTarget)(a_renderer);
			TranslatePopRenderTarget();
		}

		void DrawBlurRect(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const void* a_params)
		{
			if (NativeUi()) {
				if (!Recording())
					return;
				auto& stream = Stream();
				RecordWorld(stream);
				stream.NoteTexture(a_source);
				const bool valid = a_sourceRect && a_destRect && a_params;
				std::array<std::byte, sizeof(float) * 8 + sizeof(BlurParams)> bytes{};
				if (valid) {
					std::memcpy(bytes.data(), a_sourceRect, sizeof(float) * 4);
					std::memcpy(bytes.data() + sizeof(float) * 4, a_destRect, sizeof(float) * 4);
					std::memcpy(bytes.data() + sizeof(float) * 8, a_params, sizeof(BlurParams));
				}
				stream.Add(UiOp::DrawBlurRect, bytes.data(), valid ? bytes.size() : 0).key = a_source;
				return;
			}
			if (!Capturing())
				return Original<BlurFn>(kDrawBlurRect)(a_renderer, a_source, a_sourceRect, a_destRect, a_params);
			TranslateBlurRect(FieldState(a_renderer), a_source, a_sourceRect, a_destRect, a_params);
		}

		void DrawColorMatrixRect(void* a_renderer, RE::GTexture* a_source, const float* a_sourceRect, const float* a_destRect, const float* a_matrix)
		{
			const bool valid = a_source && a_sourceRect && a_destRect && a_matrix;
			if (NativeUi()) {
				if (!Recording())
					return;
				auto& stream = Stream();
				RecordWorld(stream);
				stream.NoteTexture(a_source);
				std::array<float, 28> floats{};
				if (valid) {
					std::memcpy(floats.data(), a_sourceRect, sizeof(float) * 4);
					std::memcpy(floats.data() + 4, a_destRect, sizeof(float) * 4);
					std::memcpy(floats.data() + 8, a_matrix, sizeof(float) * 20);
				}
				stream.Add(UiOp::DrawColorMatrixRect, floats.data(), valid ? sizeof(floats) : 0).key = a_source;
				return;
			}
			if (!Capturing())
				return Original<ColorMatrixFn>(kDrawColorMatrixRect)(a_renderer, a_source, a_sourceRect, a_destRect, a_matrix);
			if (valid)
				CaptureColorMatrix(FieldState(a_renderer), a_source, a_sourceRect, a_destRect, a_matrix);
		}

		void PushBlendMode(void* a_renderer, std::uint32_t a_mode)
		{
			if (NativeUi()) {
				if (Recording())
					RecordPlain(UiOp::PushBlendMode).a = static_cast<std::int32_t>(a_mode);
				return;
			}
			if (!Capturing())
				return Original<SetBlendFn>(kPushBlendMode)(a_renderer, a_mode);
			TranslatePushBlendMode(a_mode);
		}

		void PopBlendMode(void* a_renderer)
		{
			if (NativeUi()) {
				if (Recording())
					RecordPlain(UiOp::PopBlendMode);
				return;
			}
			if (!Capturing())
				return Original<VoidFn>(kPopBlendMode)(a_renderer);
			TranslatePopBlendMode();
		}

		void DrawIndexedTriList(void* a_renderer, std::int32_t a_base, std::int32_t a_min, std::int32_t a_count, std::int32_t a_start, std::int32_t a_triangles)
		{
			if (NativeUi()) {
				if (!Recording())
					return;
				auto& stream = Stream();
				RecordArrays(stream, true);
				RecordWorld(stream);
				RecordStyleTextures(stream, Rec().mirror.fill);
				auto& command = stream.Add(UiOp::DrawIndexedTriList);
				command.a = a_base;
				command.b = a_min;
				command.c = a_count;
				command.d = a_start;
				command.e = a_triangles;
				return;
			}
			if (!Capturing())
				return Original<DrawTrianglesFn>(kDrawIndexedTriList)(a_renderer, a_base, a_min, a_count, a_start, a_triangles);
			TranslateDrawIndexedTriList(FieldState(a_renderer), a_base, a_start, a_triangles);
		}

		void DrawLineStrip(void* a_renderer, std::int32_t a_base, std::int32_t a_lines)
		{
			if (NativeUi()) {
				if (!Recording())
					return;
				auto& stream = Stream();
				RecordArrays(stream, false);
				RecordWorld(stream);
				RecordStyleTextures(stream, Rec().mirror.line);
				auto& command = stream.Add(UiOp::DrawLineStrip);
				command.a = a_base;
				command.b = a_lines;
				return;
			}
			if (!Capturing())
				return Original<DrawLinesFn>(kDrawLineStrip)(a_renderer, a_base, a_lines);
			TranslateDrawLineStrip(FieldState(a_renderer), a_base, a_lines);
		}

		void DrawBitmaps(void* a_renderer, const BitmapDesc* a_bitmaps, std::int32_t a_listSize, std::int32_t a_start, std::int32_t a_count, RE::GTexture* a_texture,
			const float* a_matrix, void* a_cache)
		{
			if (NativeUi()) {
				if (!Recording())
					return;
				auto& stream = Stream();
				RecordWorld(stream);
				stream.NoteTexture(a_texture);
				const std::int32_t count = a_bitmaps && a_matrix ? (std::max)(a_count, 0) : 0;
				std::vector<std::byte> bytes(sizeof(BitmapDesc) * static_cast<std::size_t>(count) + sizeof(float) * 6);
				if (count)
					std::memcpy(bytes.data(), a_bitmaps + a_start, sizeof(BitmapDesc) * static_cast<std::size_t>(count));
				if (a_matrix)
					std::memcpy(bytes.data() + sizeof(BitmapDesc) * static_cast<std::size_t>(count), a_matrix, sizeof(float) * 6);
				auto& command = stream.Add(UiOp::DrawBitmaps, bytes.data(), bytes.size());
				command.a = count;
				command.key = a_texture;
				return;
			}
			if (!Capturing())
				return Original<DrawBitmapsFn>(kDrawBitmaps)(a_renderer, a_bitmaps, a_listSize, a_start, a_count, a_texture, a_matrix, a_cache);
			TranslateDrawBitmaps(FieldState(a_renderer), a_bitmaps ? a_bitmaps + a_start : nullptr, a_count, a_texture, a_matrix);
		}

		void BeginSubmitMask(void* a_renderer, std::int32_t a_mode)
		{
			if (NativeUi()) {
				if (Recording())
					RecordPlain(UiOp::BeginSubmitMask).a = a_mode;
				return;
			}
			if (!Capturing())
				return Original<MaskFn>(kBeginSubmitMask)(a_renderer, a_mode);
			TranslateBeginSubmitMask(FieldState(a_renderer), a_mode);
		}

		void EndSubmitMask(void* a_renderer)
		{
			if (NativeUi()) {
				if (Recording())
					RecordPlain(UiOp::EndSubmitMask);
				return;
			}
			if (!Capturing())
				return Original<VoidFn>(kEndSubmitMask)(a_renderer);
			TranslateEndSubmitMask(FieldState(a_renderer));
		}

		void DisableMask(void* a_renderer)
		{
			if (NativeUi()) {
				if (Recording())
					RecordPlain(UiOp::DisableMask);
				return;
			}
			if (!Capturing())
				return Original<VoidFn>(kDisableMask)(a_renderer);
			TranslateDisableMask(FieldState(a_renderer));
		}

		// The pure setters, taken in native mode only: the recorder's mirror follows GFx (repeated renders too) and the calls are
		// recorded. Nothing is forwarded.

		template <UiOp Op, std::size_t Floats>
		void SetFloats(void*, const float* a_values)
		{
			if (!Mirroring())
				return;
			auto& mirror = Rec().mirror;
			if constexpr (Op == UiOp::SetMatrix)
				std::memcpy(mirror.matrix, a_values, sizeof(mirror.matrix));
			else if constexpr (Op == UiOp::SetUserMatrix)
				std::memcpy(mirror.userMatrix, a_values, sizeof(mirror.userMatrix));
			else if constexpr (Op == UiOp::SetPerspective3D)
				mirror.SetPerspective3D(*reinterpret_cast<const float (*)[16]>(a_values));
			else if constexpr (Op == UiOp::SetView3D)
				mirror.SetView3D(*reinterpret_cast<const float (*)[16]>(a_values));
			if (Recording())
				Stream().Add(Op, a_values, sizeof(float) * Floats);
		}

		void SetCxform(void*, const float* a_cxform)
		{
			if (!Mirroring())
				return;
			std::memcpy(Rec().mirror.cxform, a_cxform, sizeof(Rec().mirror.cxform));
			if (Recording())
				Stream().Add(UiOp::SetCxform, a_cxform, sizeof(float) * 8);
		}

		void SetWorld3D(void*, const float* a_world)
		{
			if (!Mirroring())
				return;
			auto& recorder = Rec();
			recorder.world = a_world;
			if (a_world)
				recorder.worldPending = true;
			else if (Recording())
				RecordPlain(UiOp::SetWorld3D);
		}

		void SetVertexData(void*, const void* a_data, std::int32_t a_count, std::int32_t a_format, void*)
		{
			if (!Mirroring())
				return;
			Rec().mirror.SetVertexData(a_data, a_count, a_format);
			Rec().vertexPending = true;
		}

		void SetIndexData(void*, const void* a_data, std::int32_t a_count, std::int32_t a_format, void*)
		{
			if (!Mirroring())
				return;
			Rec().mirror.SetIndexData(a_data, a_count, a_format);
			Rec().indexPending = true;
		}

		void LineStyleDisable(void*)
		{
			if (!Mirroring())
				return;
			Rec().mirror.LineStyleDisable();
			if (Recording())
				RecordPlain(UiOp::LineStyleDisable);
		}

		void LineStyleColor(void*, const std::uint32_t* a_color)
		{
			if (!Mirroring())
				return;
			Rec().mirror.LineStyleColor(*a_color);
			if (Recording())
				RecordPlain(UiOp::LineStyleColor).a = static_cast<std::int32_t>(*a_color);
		}

		void FillStyleDisable(void*)
		{
			if (!Mirroring())
				return;
			Rec().mirror.FillStyleDisable();
			if (Recording())
				RecordPlain(UiOp::FillStyleDisable);
		}

		void FillStyleColor(void*, const std::uint32_t* a_color)
		{
			if (!Mirroring())
				return;
			Rec().mirror.FillStyleColor(*a_color);
			if (Recording())
				RecordPlain(UiOp::FillStyleColor).a = static_cast<std::int32_t>(*a_color);
		}

		void FillStyleBitmap(void*, const FillTexture* a_texture)
		{
			if (!Mirroring())
				return;
			Rec().mirror.FillStyleBitmap(*a_texture);
			if (Recording())
				Stream().Add(UiOp::FillStyleBitmap, a_texture, sizeof(FillTexture));
		}

		void FillStyleGouraud(void*, std::int32_t a_type, const FillTexture* a_texture0, const FillTexture* a_texture1, const FillTexture* a_texture2)
		{
			if (!Mirroring())
				return;
			Rec().mirror.FillStyleGouraud(a_type, a_texture0, a_texture1, a_texture2 != nullptr);
			if (!Recording())
				return;
			FillTexture textures[2]{};
			if (a_texture0)
				textures[0] = *a_texture0;
			if (a_texture1)
				textures[1] = *a_texture1;
			auto& command = Stream().Add(UiOp::FillStyleGouraud, textures, sizeof(textures));
			command.a = a_type;
			command.b = (a_texture0 ? 1 : 0) | (a_texture1 ? 2 : 0) | (a_texture2 ? 4 : 0);
		}

		// The D3D1x renderer's own slots past drawing, CS's in native mode.

		/** @brief CreateTexture (SE 82832 / AE 84953). */
		RE::GTexture* CreateTexture(void* a_renderer)
		{
			return UiResources::CreateTexture(a_renderer);
		}

		/** @brief CreateTextureYUV (SE 82833 / AE 84954): only GFx's video player asks, and Skyrim does not link it. */
		RE::GTexture* CreateTextureYUV(void*)
		{
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::warn("[ORG] Scaleform: GFx asked for a YUV texture, which CS does not make (the D3D1x renderer's is for GFx's video player)");
			return nullptr;
		}

		/** @brief CreateRenderTarget (SE 82830 / AE 84951). */
		void* CreateRenderTarget(void* a_renderer)
		{
			return UiResources::CreateRenderTarget(a_renderer);
		}

		/** @brief ReleaseTempRenderTargets (SE 82895 / AE 85012). */
		void ReleaseTempRenderTargets(void*, std::uint32_t a_keepPixels)
		{
			UiResources::ReleaseTempTargets(a_keepPixels);
		}

		/** @brief GetRenderCaps (SE 82852 / AE 84973): the D3D1x renderer's caps once a video mode is set. */
		bool GetRenderCaps(void*, std::uint32_t* a_caps)
		{
			if (!UiResources::VideoModeSet())
				return false;
			a_caps[0] = 0xC8F384;  // cap bits
			a_caps[1] = 0x1B;      // blend modes
			a_caps[2] = 0x36B;     // vertex formats
			a_caps[3] = 0x800;     // max texture size
			return true;
		}

		/** @brief CheckFilterSupport (SE 82825 / AE 84948): multi-pass (3) for large or multi-pass blurs, else one pass (1). */
		std::uint32_t CheckFilterSupport(void*, const std::uint8_t* a_params)
		{
			constexpr float kOnePassArea = 32.0f;  // DAT_14158142c
			float blurX = 0.0f, blurY = 0.0f;
			std::uint32_t passes = 0;
			std::memcpy(&blurX, a_params + 4, sizeof(blurX));
			std::memcpy(&blurY, a_params + 8, sizeof(blurY));
			std::memcpy(&passes, a_params + 0xC, sizeof(passes));
			return (passes > 1 || kOnePassArea < blurY * blurX) ? 3u : 1u;
		}

		/** @brief GetRenderStats (SE 0x140f5f460): CS keeps no D3D1x statistics, so they read zero. */
		void GetRenderStats(void*, std::uint8_t* a_stats, bool)
		{
			if (a_stats)
				std::memset(a_stats, 0, 0x14);
		}

		/** @brief ReleaseResources (SE 0x140f62240): CS's objects need no device reset, so nothing is let go. */
		void ReleaseResources(void*)
		{
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::info("[ORG] Scaleform: GFx asked the renderer to release its resources; CS's UI objects stay valid (no device reset in native mode)");
		}

		/** @brief SetDependentVideoMode (SE 82918 / AE 85033): keeps the engine's device and context, and the video mode is set. */
		void SetDependentVideoMode(void*, ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::uint32_t)
		{
			UiResources::SetVideoMode(a_device, a_context);
		}

		/** @brief ResetVideoMode (SE 0x140f62ec0). */
		bool ResetVideoMode(void*)
		{
			UiResources::ResetVideoMode();
			return true;
		}

		/** @brief GetDisplayStatus (SE 0x140f5ce60): 1 (no video mode) until one is set, then 0. */
		std::uint32_t GetDisplayStatus(void*)
		{
			return UiResources::VideoModeSet() ? 0u : 1u;
		}

		ID3D11Device* GetDevice(void*)
		{
			return UiResources::Device();
		}

		ID3D11DeviceContext* GetContext(void*)
		{
			return UiResources::Context();
		}

		/** @brief The destructor stays the D3D1x renderer's (it runs at shutdown). */
		void* Destroy(void* a_renderer, std::uint32_t a_flags)
		{
			using DestroyFn = void* (*)(void*, std::uint32_t);
			return Original<DestroyFn>(kDestructor)(a_renderer, a_flags);
		}

		/**
		 * @brief PushRenderTarget in native mode, CS's port of SE 82887: the frame is saved and the target becomes current, then the
		 * D3D1x push's vtable calls are made the same way (a camera for the target's frame through GFx's MakeViewAndPersp3D, then
		 * SetView3D, SetPerspective3D and no world matrix), so the stream gets the same commands.
		 */
		void NativePushRenderTarget(void* a_renderer, const float* a_frame, void* a_target, const PushedTarget& a_pushed)
		{
			if (!a_frame || !a_target)
				RenderGraph::Fatal("Scaleform", "GFx pushed a render target without a frame or a target");
			auto& targets = NativeTargets();
			auto& recorder = Rec();
			PushedFrame& frame = targets.pushed.emplace_back();
			frame.target = targets.current;
			std::memcpy(frame.view, recorder.mirror.view3D, sizeof(frame.view));
			std::memcpy(frame.perspective, recorder.mirror.perspective3D, sizeof(frame.perspective));
			frame.world = recorder.world;
			targets.current = a_target;
			if (Recording()) {
				auto& stream = Stream();
				stream.NoteTexture(a_pushed.texture);
				auto& command = stream.Add(UiOp::PushRenderTarget, a_frame, sizeof(float) * 4);
				command.key = a_pushed.texture;
				command.a = static_cast<std::int32_t>(a_pushed.width);
				command.b = static_cast<std::int32_t>(a_pushed.height);
			}
			// The D3D1x push's field of view and Y flip (SE 0x1417be7b4 holds 55).
			constexpr float kPushFov = 55.0f;
			float view[16]{}, perspective[16]{};
			for (int i = 0; i < 4; ++i)
				view[i * 5] = perspective[i * 5] = 1.0f;
			using MakeViewAndPerspFn = void (*)(void*, const float*, float*, float*, float, bool);
			Original<MakeViewAndPerspFn>(kMakeViewAndPersp3D)(a_renderer, a_frame, view, perspective, kPushFov, true);
			SetFloats<UiOp::SetView3D, 16>(a_renderer, view);
			SetFloats<UiOp::SetPerspective3D, 16>(a_renderer, perspective);
			SetWorld3D(a_renderer, nullptr);
		}

		/**
		 * @brief PopRenderTarget in native mode, CS's port of SE 82882: a pooled target lets its depth-stencil go (the flag
		 * PushTempRenderTarget set), the saved target becomes current again and its 3D matrices are set back through the slots.
		 */
		void NativePopRenderTarget(void* a_renderer)
		{
			auto& targets = NativeTargets();
			if (targets.pushed.empty())
				RenderGraph::Fatal("Scaleform", "GFx popped a render target it never pushed");
			if (UiResources::IsRenderTarget(targets.current)) {
				auto* popped = static_cast<UiResources::RenderTarget*>(targets.current);
				if (popped->pooled && popped->depth) {
					UiResources::Release(popped->depth);
					popped->depth = nullptr;
				}
			}
			const PushedFrame frame = targets.pushed.back();
			targets.pushed.pop_back();
			targets.current = frame.target;
			if (Recording())
				RecordPlain(UiOp::PopRenderTarget);
			SetFloats<UiOp::SetView3D, 16>(a_renderer, frame.view);
			SetFloats<UiOp::SetPerspective3D, 16>(a_renderer, frame.perspective);
			SetWorld3D(a_renderer, frame.world);
		}

		/**
		* @brief An offscreen menu's PreDisplay. Main::DrawFrame calls it before the world, through the offscreen menus' loop (SE 79948 /
		* AE 82085). In graph mode, native and guest, it runs at DrawInterface's start instead, inside the UI stage, where its draws are
		* CS's steps. Nothing in the world reads the targets they draw, so the move changes no image.
		* - BookMenu::PreDisplay (SE 50121 / AE 51052): the page copies into kBOOK_TEXT_n and the pages' displays.
		* - MapMenu::PreDisplay (SE 52216 / AE 53103, vtable slot 7): the local map's render while the local map shows (LocalMapRenderer::
		*   Render, SE 16094 / AE 16335): its land and objects and its fog of war into kLOCAL_MAP_SWAP, and ISLocalMap into kLOCAL_MAP.
		*/
		template <int Menu>
		struct OffscreenPreDisplay
		{
			static void thunk(void* a_menu)
			{
				if (!FrameAnchors::IsRenderOwner() || running)
					return func(a_menu);
				due = a_menu;
			}

			/** @brief DrawInterface's start: the call DrawFrame left, under the movie lock its loop takes, with the target and depth put back. */
			static void RunDue()
			{
				auto* menu = std::exchange(due, nullptr);
				if (!menu)
					return;
				// DrawInterface keeps the bound target and depth when one is set, so the pages' targets must not stay bound.
				auto& shadow = CS_ENGINE_READ(ShadowState, RecordTime, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
				const auto target = shadow.renderTargets[0];
				const auto depth = shadow.depthStencil;
				const auto slice = shadow.depthStencilSlice;
				static const REL::Relocation<void (*)(void*)> lock{ RELOCATION_ID(80605, 82732) };
				static const REL::Relocation<void (*)(void*)> unlock{ RELOCATION_ID(80606, 82733) };
				auto* movies = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(RE::BSScaleformManager::GetSingleton()) + 0x10);
				running = true;
				lock(movies);
				func(menu);
				unlock(movies);
				running = false;
				if (shadow.renderTargets[0] != target || shadow.depthStencil != depth || shadow.depthStencilSlice != slice) {
					shadow.renderTargets[0] = target;
					shadow.setRenderTargetMode[0] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
					shadow.depthStencil = depth;
					shadow.depthStencilSlice = slice;
					shadow.setDepthStencilMode = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
					shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
				}
			}

			static inline void* due = nullptr;
			static inline bool running = false;
			static inline REL::Relocation<decltype(thunk)> func;
		};
		using BookPreDisplay = OffscreenPreDisplay<0>;
		using MapPreDisplay = OffscreenPreDisplay<1>;

		/**
		 * @brief The UI point's render-side work, which the features' DrawInterface detours do outside native mode (in this order:
		 * Upscaling's or HDR's detour, then Hooks.cpp's): Upscaling's PostDisplay (the jitter and dynamic resolution undone, the full
		 * resolution camera), the clean-scene copy, HDR's UI buffer, then temporal off until the scope ends. Render owner.
		 */
		struct UiPoint
		{
			UiPoint()
			{
				auto& upscaling = globals::features::upscaling;
				auto& hdr = globals::features::hdrDisplay;
				if (upscaling.loaded) {
					upscaling.PostDisplay();
					globals::features::screenshotFeature.SnapshotCleanScene();
					if (hdr.loaded)
						hdr.SetUIBuffer();
				} else if (hdr.loaded) {
					globals::features::screenshotFeature.SnapshotCleanScene();
					hdr.SetUIBuffer();
				}
				temporal = Util::GetTemporal();
				Util::SetTemporal(false);
			}
			~UiPoint() { Util::SetTemporal(temporal); }
			UiPoint(const UiPoint&) = delete;
			UiPoint& operator=(const UiPoint&) = delete;
			bool temporal = false;
		};

		/**
		 * @brief Whether the render owner's DrawInterface for a_snapshot records only and the UI point replays after it: a game update's
		 * unpaused world frame without the map, by the snapshot and by the UI at DrawInterface's start, and no offscreen PreDisplay due.
		 * In those only GFx records inside DrawInterface. Other
		 * frames (menus that pause, the map, the book, loading screens, frame-only snapshots) have menu scenes, ImageSpace effects or
		 * PreDisplay draws recorded inline, so each display is replayed at its EndDisplay to keep their order.
		 */
		bool RecordOnlyFrame(const Snapshot::FrameSnapshot& a_snapshot);

		struct DrawInterface
		{
			/**
			 * @brief MenuManager::DrawInterface (SE 79947 / AE 82084), native mode. Its UI stream goes into the snapshot being built in a
			 * game update (Snapshot::Building), else into the frame-only snapshot the frame adopted (a loading screen's on the serving
			 * thread, a main-thread frame outside Main::Update).
			 */
			static void thunk(void* a_manager)
			{
				if (!NativeUi()) {
					if (FrameAnchors::IsRenderOwner()) {
						BookPreDisplay::RunDue();
						MapPreDisplay::RunDue();
					}
					return func(a_manager);
				}
				if (!FrameAnchors::IsRenderOwner())
					return func(a_manager);
				const auto snapshot = Snapshot::InGameUpdate() ? Snapshot::Building() : Snapshot::Current();
				if (snapshot && RecordOnlyFrame(*snapshot)) {
					RecordOnly(a_manager, *snapshot);
					ReplaySnapshotUi(*snapshot);
					return;
				}
				PerDisplay(a_manager, snapshot.get());
			}

			/**
			 * @brief Records GFx's calls into a_snapshot's holder and replays nothing. A snapshot whose stream is set already is a
			 * repeated render (cadence repeat): GFx draws again and nothing is recorded. UI producer.
			 */
			static void RecordOnly(void* a_manager, const Snapshot::FrameSnapshot& a_snapshot, void (*a_body)(void*) = nullptr)
			{
				const auto body = a_body ? a_body : func.get();
				const auto& holder = a_snapshot.ui;
				if (!holder)
					RenderGraph::Fatal("Scaleform", "a record-only DrawInterface for a snapshot without a UI stream holder");
				if (g_recordOnly.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[ORG] Scaleform: first record-only DrawInterface (snapshot {}): GFx's calls are recorded, the UI point replays them after it",
						a_snapshot.id);
				auto& recorder = Rec();
				const UiModeScope mode(UiMode::RecordOnly);
				if (auto repeat = holder->Get()) {
					recorder.repeat = std::move(repeat);
					body(a_manager);
					recorder.repeat.reset();
					return;
				}
				body(a_manager);
				if (auto finished = FinishStream())
					holder->Set(std::move(finished));
			}

			/**
			 * @brief The render owner records and replays each display at its EndDisplay, the tail at the end, inside the UI point. A
			 * repeated render (the snapshot's stream is set) records nothing and draws each display from the snapshot's stream where GFx
			 * ends its own. Menu scenes and the interface's ImageSpace effects run where DrawInterface calls them, between the displays.
			 */
			static void PerDisplay(void* a_manager, const Snapshot::FrameSnapshot* a_snapshot)
			{
				g_perDisplay.fetch_add(1, std::memory_order_relaxed);
				const UiPoint point;
				auto& recorder = Rec();
				auto& replay = Replayer();
				const Snapshot::UiCommandStream* holder = a_snapshot ? a_snapshot->ui.get() : nullptr;
				const auto repeat = holder ? holder->Get() : nullptr;
				if (repeat) {
					recorder.repeat = repeat;
					// The replay starts again from the stream's header.
					Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Render, std::exchange(replay.stream, nullptr));
				}
				{
					const UiModeScope mode(UiMode::PerDisplay);
					BookPreDisplay::RunDue();
					MapPreDisplay::RunDue();
					func(a_manager);
				}
				if (repeat) {
					RunReplay(repeat);
					recorder.repeat.reset();
					return;
				}
				if (recorder.stream)
					RunReplay(recorder.stream);
				auto finished = FinishStream();
				if (finished && holder)
					holder->Set(std::exchange(finished, nullptr));
				// A stream no snapshot took still went to the GPU through its replay.
				Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Render, std::move(finished));
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool RecordOnlyFrame(const Snapshot::FrameSnapshot& a_snapshot)
		{
			if (!Snapshot::InGameUpdate() || a_snapshot.kind != Snapshot::FrameKind::World || a_snapshot.menus.paused || a_snapshot.menus.map ||
				BookPreDisplay::due || MapPreDisplay::due)
				return false;
			// A menu the update's UI jobs opened after the capture draws in this DrawInterface already (its first frame), so the menus
			// are asked again here. The UI is game state and DrawInterface's start is a capture point.
			auto* ui = CS_ENGINE_READ(UI, CapturePoint, RE::UI::GetSingleton());
			return ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
		}

		/** @brief Whether the object's vtable is GRendererD3D1x's, so the slots CS replaces are the expected functions. */
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

		/** @brief Puts CS's vtable on the renderer, a copy of its own with CS's slots. */
		void Wrap(void* a_renderer)
		{
			auto** vtable = *static_cast<void***>(a_renderer);
			if (!IsD3D1xRenderer(vtable))
				RenderGraph::Fatal("Scaleform", "the renderer is not the GRendererD3D1x CS knows");
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
			if (NativeUi()) {
				// Native mode records every call into the UI stream, the pure setters too, and draws the stream's replay.
				set(kSetMatrix, &SetFloats<UiOp::SetMatrix, 6>);
				set(kSetUserMatrix, &SetFloats<UiOp::SetUserMatrix, 6>);
				set(kSetCxform, &SetCxform);
				set(kSetPerspective3D, &SetFloats<UiOp::SetPerspective3D, 16>);
				set(kSetView3D, &SetFloats<UiOp::SetView3D, 16>);
				set(kSetWorld3D, &SetWorld3D);
				set(kSetVertexData, &SetVertexData);
				set(kSetIndexData, &SetIndexData);
				set(kLineStyleDisable, &LineStyleDisable);
				set(kLineStyleColor, &LineStyleColor);
				set(kFillStyleDisable, &FillStyleDisable);
				set(kFillStyleColor, &FillStyleColor);
				set(kFillStyleBitmap, &FillStyleBitmap);
				set(kFillStyleGouraud, &FillStyleGouraud);
				// And the renderer's own slots: textures, targets and the video mode are CS's.
				UiResources::Install();
				set(kDestructor, &Destroy);
				set(kGetRenderCaps, &GetRenderCaps);
				set(kCreateTexture, &CreateTexture);
				set(kCreateTextureYUV, &CreateTextureYUV);
				set(kCreateRenderTarget, &CreateRenderTarget);
				set(kReleaseTempRenderTargets, &ReleaseTempRenderTargets);
				set(kCheckFilterSupport, &CheckFilterSupport);
				set(kGetRenderStats, &GetRenderStats);
				set(kReleaseResources, &ReleaseResources);
				set(kSetDependentVideoMode, &SetDependentVideoMode);
				set(kResetVideoMode, &ResetVideoMode);
				set(kGetDisplayStatus, &GetDisplayStatus);
				set(kGetDevice, &GetDevice);
				set(kGetContext, &GetContext);
				g_ownsRenderer.store(true, std::memory_order_release);
			}
			*static_cast<void***>(a_renderer) = g_vtable.data() + 1;
			logger::info("[ORG] Scaleform: the renderer has CS's vtable; displays are drawn on the render graph once it is up{}",
				NativeUi() ? "; native mode records the UI stream and draws its replay" : "");
		}

		/** @brief The renderer's factory, SE 82831 / AE 84952, called by BSScaleformManager::InitRenderer (SE 80603 / AE 82730). */
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

	void Install()
	{
		if (!Engine::GraphMode())
			return;
		logger::info("[ORG] Scaleform: hooking the renderer's factory");
		stl::detour_thunk<CreateRenderer>(REL::RelocationID(82831, 84952));
		GfxTextures::Install();
		if (!REL::Module::IsVR()) {
			stl::detour_thunk<DrawInterface>(REL::RelocationID(79947, 82084));
			stl::detour_thunk<BookPreDisplay>(REL::RelocationID(50121, 51052));
			// MapMenu::PreDisplay is a jump into its local map menu's, so the vtable slot is replaced.
			REL::Relocation<std::uintptr_t> mapMenu{ RE::VTABLE_MapMenu[0] };
			MapPreDisplay::func = mapMenu.write_vfunc(7, MapPreDisplay::thunk);
			if (!DrawInterface::func.address() || !BookPreDisplay::func.address())
				RenderGraph::Fatal("Scaleform", "DrawInterface or BookMenu::PreDisplay could not be hooked");
		}
	}

	bool OwnsRenderer()
	{
		return g_ownsRenderer.load(std::memory_order_acquire);
	}

	namespace
	{
		/**
		* @brief MenuManager::DrawInterface's display loop (AE 0x140fa4f00) without its render state. The engine's loop: the TLS memory
		* context 0x46, the movie lock, the menu stack lock (SE 525557 / AE 412033), then from the topmost menu that hides the ones below
		* it (flag 0x200) or the bottom up to the top, each menu's PostDisplay (vfunc +0x30), then the render pass free list's returns
		* (SE 100722 / AE 107502). Left out: the per-frame buffers, the target and depth binds, ApplyState, each menu's viewport, VParams
		* at 1, and the ISCopy into the framebuffer when the bound target is not it (ReplaySnapshotUi ports the binds' rule).
		*/
		void DisplayMenus(void* a_ui)
		{
			auto* ui = static_cast<RE::UI*>(a_ui);
			if (!*reinterpret_cast<const bool*>(reinterpret_cast<const std::byte*>(ui) + 0x1C0))
				return;
			static const REL::Relocation<std::uint32_t*> tlsIndex{ RELOCATION_ID(528600, 415542) };
			auto* tls = reinterpret_cast<std::byte**>(__readgsqword(0x58));
			auto* context = reinterpret_cast<std::uint32_t*>(tls[*tlsIndex.get()] + 0x768);
			const auto savedContext = std::exchange(*context, 0x46u);
			static const REL::Relocation<void (*)(void*)> lock{ RELOCATION_ID(80605, 82732) };
			static const REL::Relocation<void (*)(void*)> unlock{ RELOCATION_ID(80606, 82733) };
			static const REL::Relocation<RE::BSSpinLock*> stackLock{ RELOCATION_ID(525557, 412033) };
			auto* movies = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(RE::BSScaleformManager::GetSingleton()) + 0x10);
			lock(movies);
			{
				const RE::BSSpinLockGuard guard(*stackLock.get());
				auto& stack = ui->menuStack;
				if (!stack.empty()) {
					std::uint32_t first = stack.size() - 1;
					while (first > 0 && !(static_cast<std::uint32_t>(stack[first]->menuFlags.underlying()) & 0x200u))
						--first;
					for (std::uint32_t i = first; i < stack.size(); ++i)
						if (const auto& menu = stack[i])
							menu->PostDisplay();
				}
			}
			unlock(movies);
			static const REL::Relocation<void (*)()> returnPasses{ RELOCATION_ID(100722, 107502) };
			returnPasses();
			*context = savedContext;
		}
	}

	void ProduceUi(RE::UI* a_ui)
	{
		if (!Native::Active())
			RenderGraph::Fatal("Scaleform", "a game-side UI producer outside native mode");
		const auto& snapshot = Snapshot::Building();
		if (!snapshot)
			RenderGraph::Fatal("Scaleform", "a game-side UI producer before the first game update's snapshot");
		DrawInterface::RecordOnly(a_ui, *snapshot, &DisplayMenus);
	}

	bool RecordsUiOnly(const Snapshot::FrameSnapshot& a_snapshot)
	{
		return RecordOnlyFrame(a_snapshot);
	}

	void ReplaySnapshotUi(const Snapshot::FrameSnapshot& a_snapshot)
	{
		if (!FrameAnchors::IsRenderOwner())
			RenderGraph::Fatal("Scaleform", "the UI point's replay off the render owner");
		const UiPoint point;
		// DrawInterface's binds before its displays: no bound target means kMAIN. The record-only stream reads the targets here.
		if (auto* shadow = globals::game::shadowState) {
			auto& rss = shadow->GetRuntimeData();
			if (static_cast<std::int32_t>(rss.renderTargets[0]) == -1) {
				rss.renderTargets[0] = static_cast<std::remove_cvref_t<decltype(rss.renderTargets[0])>>(RE::RENDER_TARGET::kMAIN);
				rss.setRenderTargetMode[0] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
				rss.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
			}
		}
		const auto stream = a_snapshot.ui ? a_snapshot.ui->Get() : nullptr;
		if (!stream)
			return;
		auto& replay = Replayer();
		// The replay starts from the stream's header, a repeated render's too.
		Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Render, std::exchange(replay.stream, nullptr));
		RunReplay(stream);
	}

	bool OwnsUiPoint()
	{
		return Native::Active() && DrawInterface::func.address() != 0;
	}

	void NoteOtherUiWork(const char* a_what)
	{
		if (t_uiMode != UiMode::RecordOnly)
			return;
		if (g_otherWork.fetch_add(1, std::memory_order_relaxed) == 0)
			logger::warn("[ORG] Scaleform: {} inside a record-only DrawInterface: it draws before the UI point's displays instead of between them (counted in the census line)", a_what);
	}
}
