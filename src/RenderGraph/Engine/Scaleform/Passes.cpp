#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/Engine/Scaleform/Passes.h"

#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/HDRDisplay.h"
#	include "Features/HDRDisplay/ORGHDRDisplay.h"
#	include "Globals.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/Engine/Scaleform/Display.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/ProgramCache.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/UndeclaredAccess.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <bit>
#	include <cmath>
#	include <cstddef>
#	include <cstring>
#	include <optional>
#	include <set>
#	include <string>

namespace RenderGraph::Engine::Scaleform::Passes
{
	namespace
	{
		constexpr const char* kClientId = "cs.scaleform";
		constexpr const char* kVertexShader = "Scaleform/FillVS.hlsl";
		constexpr const char* kPixelShader = "Scaleform/FillPS.hlsl";
		constexpr const char* kFilterVertexShader = "Scaleform/FilterVS.hlsl";
		constexpr const char* kFilterPixelShader = "Scaleform/FilterPS.hlsl";
		// GpuTextures' diagnostic source tag for Scaleform's views (ImGui's is 256).
		constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::Scaleform);
		constexpr std::uint32_t kMinimumStaging = 256u << 10;
		// The most commands one Interface run draws: one run gate entry each. A longer segment is drawn in several runs.
		constexpr std::uint32_t kGateEntries = 1024;
		// The UI stages' fixed instances (docs/development/org-native-frame-graph.md, "Frame kinds and the UI stage"), each with its own gate
		// entries and run slots: the UI stage's Interface step makes instances 0 to 7 (the k-th Interface run of the frame is instance k); the
		// menu UI stage's step before its scene makes 8 to 15, its step after the scene 0 to 7. All are declared every frame; the ones the
		// engine does not use stay closed. More runs at one step in one frame are a mismatch (an instance not declared). The anchor's own
		// epoch (guest mode) uses instance 0.
		constexpr std::uint32_t kInstances = 16;
		// The scratch images GFx's render targets are drawn into on the graph (PushRenderTarget: a filter's source, its passes): a
		// blur's source and its two intermediates, and one more for a nested target.
		constexpr std::uint32_t kScratchTargets = 4;
		constexpr rhi::Format kScratchFormat = rhi::Format::R8G8B8A8_UNorm;

		using FrameAnchors::Anchor;

		// Must match the push block in Scaleform/Fill.hlsli.
		struct FillBlock
		{
			std::uint32_t geometry;
			std::uint32_t record;
			std::uint32_t textures[4];
			std::uint32_t samplers[4];
			std::uint32_t padding[2];
		};
		static_assert(sizeof(FillBlock) == 48);
		constexpr std::uint32_t kConstantWords = ShaderBindings::Words<FillBlock>();

		// Must match the push block in Scaleform/Filter.hlsli; padded to the fill block's size, so every program has one layout.
		struct FilterBlock
		{
			std::uint32_t geometry;
			std::uint32_t vertexOffset;
			std::uint32_t matrixOffset;
			std::uint32_t constantsOffset;
			std::uint32_t source;
			std::uint32_t second;
			std::uint32_t sampler0;
			std::uint32_t sampler1;
			std::uint32_t padding[4];
		};
		static_assert(sizeof(FilterBlock) == sizeof(FillBlock));

		/** @brief A pipeline: the draw's state and the formats of the pass it draws in (depth Unknown for a pass without masks). */
		struct ProgramKey
		{
			DrawState state;
			rhi::Format colour = rhi::Format::Unknown;
			rhi::Format depth = rhi::Format::Unknown;

			auto operator<=>(const ProgramKey&) const = default;
		};

		/** @brief One command as the pass records it. */
		struct FrameCommand
		{
			Command::Kind kind = Command::Kind::Draw;
			const RasterProgram* program = nullptr;
			std::uint32_t stencilRef = 0;
			bool lines = false;   // a line strip
			bool filter = false;  // the block is a FilterBlock
			// Per texture stage: the declared read (FrameAnchors::Texture::Key) whose view the pass binds in place of the block's raw
			// index, or null (an exempt texture, read untracked).
			std::array<const void*, 2> reads{};
			FillBlock block{};
		};

		/** @brief Native mode: a tracked texture the displays read, which the passes declare (PathState::reads). */
		struct Read
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;  // the display's view: its identity, kept while it is declared
			FrameAnchors::Texture texture;
			std::uint64_t lastCommit = 0;
		};

		/** @brief The tracked textures one build's passes declare. */
		using Reads = std::vector<FrameAnchors::Texture>;

		// A declared read no commit used for this many commits is dropped the next time the client registers.
		constexpr std::uint64_t kReadIdleCommits = 600;

		/** @brief What one run of a surface's pass records: immutable once the commit stored it. */
		struct Frame
		{
			std::shared_ptr<org::LatchBlock> staging;
			std::vector<FrameCommand> commands;
			rhi::Viewport viewport{};
			rhi::ScissorRect scissor{};
			std::uint32_t width = 0, height = 0;
			// The run starts by clearing the target to zero (a render target GFx pushed: PushRenderTarget clears it whole).
			bool clear = false;
			rhi::Format format = rhi::Format::Unknown;
			std::vector<std::shared_ptr<const void>> owners;  // programs, samplers, texture descriptors and views
		};

		using Slots = RunSlots<Frame>;

		/**
		 * @brief The targets UI::DrawInterface binds for the displays: the only ones the graph draws onto. The main menu and the
		 * loading menu draw their movies into kMAIN (the 2026-11-12 latch census: most of the Scaleform draws left in load windows
		 * and at the main menu), so kMAIN is a surface from setup too.
		 */
		enum class Role : std::uint8_t
		{
			Framebuffer,  // the back buffer (kFRAMEBUFFER's RTV, or HDR's saved one)
			HdrUi,        // HDR's UI texture, while HDR redirects the interface into it
			Main,         // kMAIN: the main menu's and the loading menu's movies
			// kBOOK_TEXT_0 to 3: the book menu's pages, rendered into a texture of GFx's (kViewportRenderTexture) that is one of these
			// engine targets, with kBOOK_TEXT's depth-stencil for masks.
			Book0,
			Book1,
			Book2,
			Book3,
		};
		constexpr std::array<Role, 7> kRoles{ Role::Framebuffer, Role::HdrUi, Role::Main, Role::Book0, Role::Book1, Role::Book2, Role::Book3 };

		bool IsBook(Role a_role) { return a_role >= Role::Book0 && a_role <= Role::Book3; }

		/**
		 * @brief A target the displays draw onto, with kMAIN's depth-stencil for masks. It has two passes: one with the target alone,
		 * for displays without masks, and one with the depth-stencil too, for displays with masks (when the depth can be imported).
		 * One surface per role, made at setup; made again only when the role's texture was made again (a resize, HDR's textures).
		 */
		struct Surface
		{
			Role role = Role::Framebuffer;
			// A scratch image's index (a graph texture of this client's that GFx's pushed render targets are drawn into), or -1 for a role.
			std::int32_t scratch = -1;
			winrt::com_ptr<ID3D11Texture2D> colourTexture;  // DXVK's: the surface's identity
			winrt::com_ptr<ID3D11Texture2D> depthTexture;
			FrameAnchors::Texture colour;  // an engine target's slot, or this client's own import
			FrameAnchors::Texture depth;
			std::string ownId;  // set when this client registers colour itself (HDR's twin; imports are the anchors')
			rhi::Format colourFormat = rhi::Format::Unknown;
			rhi::Format depthFormat = rhi::Format::Unknown;
			std::uint32_t width = 0, height = 0;
			// One per instance (kInstances): empty for a surface the graph cannot draw onto (no passes).
			std::vector<std::shared_ptr<const Slots>> colourSlots;
			std::vector<std::shared_ptr<const Slots>> depthSlots;  // empty without an importable depth-stencil

			bool Drawable() const { return !colourSlots.empty(); }
			bool Masks() const { return !depthSlots.empty(); }

			/** @brief Every slot emptied: a pass of an older build that still has this surface draws nothing more. */
			void Clear() const
			{
				for (const auto& slots : colourSlots)
					slots->Clear();
				for (const auto& slots : depthSlots)
					slots->Clear();
			}
		};

		/** @brief A surface pass's slots, one per instance. */
		std::vector<std::shared_ptr<const Slots>> MakeSlots()
		{
			std::vector<std::shared_ptr<const Slots>> slots;
			for (std::uint32_t i = 0; i < kInstances; ++i)
				slots.push_back(std::make_shared<const Slots>());
			return slots;
		}

		struct PathState
		{
			bool reserved = false;
			bool failed = false;
			bool setUp = false;
			std::array<org::OwnedDescriptorBinding, 4> samplers;  // the renderer's, by wrap * 2 + sample
			std::vector<std::shared_ptr<Surface>> surfaces;       // one per role that has a texture, then the scratch images
			std::uint32_t scratchWidth = 0, scratchHeight = 0;    // every scratch image's size (the largest role surface's)
			// Native mode: the tracked textures the displays read (graph-owned, or exempt ones the context wrote since). The passes
			// declare each, and a draw binds its declared view. A new one registers the client again after the run (readsChanged).
			std::vector<Read> reads;
			bool readsChanged = false;
			std::uint64_t commits = 0;
			std::uint32_t registrations = 0;
			// The registration that made the surfaces as they are now, and the latest one a built graph is known to hold. A registration
			// for new reads alone keeps the surfaces: the graph built before it still draws them (a new read keeps its raw index there).
			std::uint32_t surfacesRegistration = 0;
			std::uint32_t builtRegistration = 0;
			std::uint32_t largestSegment = 0;
			// Counts the times the surfaces were made (ClientDesc::generation): the stage's schedule draws a display only once it holds a
			// registration with the surfaces as they are.
			std::uint64_t surfacesGeneration = 0;
			std::uint32_t peakTaps = 0;  // the most Interface runs one world frame had so far (logged at each new peak)
			ProgramCache<RasterProgram, ProgramKey> programs{ "Scaleform" };
			std::set<ProgramKey> failedPrograms;  // each logged once (NoteFailedProgram)
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<FrameStaging> staging;
			// The run in progress (Submit to the end of its anchor run): the display, and the commands of it this run draws.
			Display* pending = nullptr;
			std::uint32_t pendingFirst = 0;
			std::uint32_t pendingCount = 0;
			Surface* pendingSurface = nullptr;
			bool pendingDepth = false;                                          // the pass with the depth-stencil draws it
			std::vector<std::shared_ptr<const RasterProgram>> pendingPrograms;  // per command; null for a clear
			bool committed = false;
			bool executed = false;
			const char* commitReason = nullptr;
		};

		PathState& Path()
		{
			static PathState s_state;
			return s_state;
		}

		/**
		 * @brief The renderer's blend state: CreateShaders' first array (indices 1 to 7), or the filters' array (9 to 15: the same with
		 * source colour factor One where the first has SrcAlpha, for premultiplied filter output; read from SE 0x1417c2588).
		 */
		rhi::BlendAttachment BlendOf(std::uint8_t a_index)
		{
			using F = rhi::BlendFactor;
			using O = rhi::BlendOp;
			if (a_index > 8) {
				rhi::BlendAttachment blend = BlendOf(static_cast<std::uint8_t>(a_index - 8));
				if (blend.srcColor == F::SrcAlpha)
					blend.srcColor = F::One;
				return blend;
			}
			rhi::BlendAttachment blend{};
			blend.enable = true;
			auto set = [&](F a_src, F a_dst, O a_op, F a_srcAlpha, F a_dstAlpha, O a_alphaOp) {
				blend.srcColor = a_src;
				blend.dstColor = a_dst;
				blend.colorOp = a_op;
				blend.srcAlpha = a_srcAlpha;
				blend.dstAlpha = a_dstAlpha;
				blend.alphaOp = a_alphaOp;
			};
			switch (a_index) {
			case 2:  // add
				set(F::SrcAlpha, F::One, O::Add, F::Zero, F::One, O::Add);
				break;
			case 3:  // subtract
				set(F::SrcAlpha, F::One, O::RevSub, F::Zero, F::One, O::RevSub);
				break;
			case 4:  // multiply
				set(F::DstColor, F::Zero, O::Add, F::DstAlpha, F::Zero, O::Add);
				break;
			case 5:  // lighten
				set(F::SrcAlpha, F::One, O::Max, F::One, F::One, O::Max);
				break;
			case 6:  // darken
				set(F::SrcAlpha, F::One, O::Min, F::One, F::One, O::Min);
				break;
			case 7:  // no colour write: masks, and the Alpha and Erase modes
				set(F::SrcAlpha, F::InvSrcAlpha, O::Add, F::SrcAlpha, F::InvSrcAlpha, O::Add);
				blend.enable = false;
				blend.writeMask = static_cast<rhi::ColorWriteEnable>(0);
				break;
			default:  // 1: normal
				set(F::SrcAlpha, F::InvSrcAlpha, O::Add, F::One, F::InvSrcAlpha, O::Add);
				break;
			}
			return blend;
		}

		/** @brief The renderer's depth-stencil state: depth never tested, the stencil as the mask needs it. */
		rhi::DepthStencilState DepthOf(Stencil a_stencil)
		{
			rhi::DepthStencilState depth{ .depthEnable = false, .depthWrite = false, .depthFunc = rhi::CompareOp::Always };
			if (a_stencil == Stencil::Off)
				return depth;
			depth.stencilEnable = true;
			depth.stencilReadMask = 0xFF;
			depth.stencilWriteMask = a_stencil == Stencil::Test ? 0 : 0xFF;
			rhi::StencilFaceState face{};
			switch (a_stencil) {
			case Stencil::Clear:
				face.passOp = rhi::StencilOp::Replace;
				face.func = rhi::CompareOp::Always;
				break;
			case Stencil::Increment:
				face.passOp = rhi::StencilOp::Incr;
				face.func = rhi::CompareOp::Equal;
				break;
			case Stencil::Decrement:
				face.passOp = rhi::StencilOp::Decr;
				face.func = rhi::CompareOp::Equal;
				break;
			default:  // Test
				face.passOp = rhi::StencilOp::Keep;
				face.func = rhi::CompareOp::Equal;
				break;
			}
			depth.front = face;
			depth.back = face;
			return depth;
		}

		/** @brief What a pipeline is built from (ProgramCache's describe, on the render thread). */
		RasterProgram::Desc DescOf(const ProgramKey& a_key)
		{
			RasterProgram::Desc desc{};
			desc.constantWords = kConstantWords;
			if (a_key.state.filter) {
				desc.vertexSource = kFilterVertexShader;
				desc.pixelSource = kFilterPixelShader;
				desc.defines = { { L"FILTER", std::to_wstring(a_key.state.filter) } };
			} else {
				desc.vertexSource = kVertexShader;
				desc.pixelSource = kPixelShader;
				// The renderer's vertex shader index: 1 to 5, and 7 to 11 for the same kinds in 3D (a world matrix set).
				const bool threeD = a_key.state.vertexKind > 6;
				desc.defines = { { L"VS_KIND", std::to_wstring(threeD ? a_key.state.vertexKind - 6 : a_key.state.vertexKind) },
					{ L"PS_KIND", std::to_wstring(a_key.state.pixelKind) } };
				if (threeD)
					desc.defines.emplace_back(L"VS_3D", L"1");
			}
			desc.topology = a_key.state.lines ? rhi::PrimitiveTopology::LineStrip : rhi::PrimitiveTopology::TriangleList;
			RasterProgram::Target target{ .format = a_key.colour };
			target.blend = BlendOf(a_key.state.blend);
			desc.targets = { target };
			desc.depthFormat = a_key.depth;
			desc.depth = DepthOf(a_key.state.stencil);
			return desc;
		}

		/**
		 * @brief Every draw state the capture can make (Renderer.cpp: the background, ApplyFill, DrawIndexedTriList's vertex kinds,
		 * DrawBitmaps, the colour matrix filter), for the pass with the depth-stencil or the one without. The set is finite:
		 * - vertex and pixel kinds: 1 with 1-3 (solid, bitmap), 2 with 4-6 (bitmaps), 3 with 12-14 and 16-18 (XY16iC32 gouraud),
		 *   4 with 11, 13, 14, 15, 17, 18 (XY16iCF32 gouraud), 5 with 11, 14, 15, 18 (gouraud type 3); filters 1 to 24; line strips
		 *   of kind 1 with 1 (the line style's colour);
		 * - blends: the Multiply and Darken kinds (3, 6, 15-18, filter 24) only with the multiply and darken states (4, 6), the other
		 *   kinds with 1, 2, 3, 5, the solid and glyph kinds (1, 4) with any, the filters from the filters' array (8 + the index);
		 *   every kind with 7 (a mask being drawn, the Alpha and Erase modes, a bitmap fill without a texture);
		 * - 3D draws (a world matrix set: vertex kinds 7-11) are not enumerated: they are built at their first draw (logged).
		 * - stencils: off in the pass without masks; off and test with any blend, clear, increment and decrement only with 7 (inside
		 *   BeginSubmitMask) in the pass with them.
		 */
		std::vector<DrawState> DrawStates(bool a_withDepth)
		{
			struct Kinds
			{
				std::uint8_t vertex;
				std::vector<std::uint8_t> pixels;
			};
			const std::array<Kinds, 5> fills{ { { 1, { 1, 2, 3 } }, { 2, { 4, 5, 6 } }, { 3, { 12, 13, 14, 16, 17, 18 } }, { 4, { 11, 13, 14, 15, 17, 18 } },
				{ 5, { 11, 14, 15, 18 } } } };
			const auto multiply = [](std::uint8_t a_pixel) { return a_pixel == 3 || a_pixel == 6 || a_pixel >= 15; };
			std::vector<DrawState> states;
			const auto add = [&](DrawState a_state, const std::vector<std::uint8_t>& a_blends) {
				for (const auto blend : a_blends) {
					a_state.blend = blend;
					a_state.stencil = Stencil::Off;
					states.push_back(a_state);
					if (a_withDepth) {
						a_state.stencil = Stencil::Test;
						states.push_back(a_state);
					}
				}
				if (a_withDepth) {
					a_state.blend = 7;
					for (const auto stencil : { Stencil::Clear, Stencil::Increment, Stencil::Decrement }) {
						a_state.stencil = stencil;
						states.push_back(a_state);
					}
				}
			};
			for (const auto& kinds : fills)
				for (const auto pixel : kinds.pixels) {
					// A solid fill in Multiply or Darken keeps kind 1 (its colour is moved towards white instead).
					const std::vector<std::uint8_t> blends = pixel == 4 || pixel == 1 ? std::vector<std::uint8_t>{ 1, 2, 3, 4, 5, 6, 7 } :
					                                         multiply(pixel) ? std::vector<std::uint8_t>{ 4, 6, 7 } :
					                                                           std::vector<std::uint8_t>{ 1, 2, 3, 5, 7 };
					add({ .vertexKind = kinds.vertex, .pixelKind = pixel }, blends);
				}
			add({ .vertexKind = 1, .pixelKind = 1, .lines = 1 }, { 1, 2, 3, 4, 5, 6, 7 });
			add({ .vertexKind = 0, .pixelKind = 0, .filter = 23 }, { 9, 10, 11, 13, 15, 7 });
			add({ .vertexKind = 0, .pixelKind = 0, .filter = 24 }, { 12, 14, 7 });
			// The blur family (DrawBlurRect): its last pass blends with the filters' array for the mode (the Multiply and Darken shaders
			// with those modes' states), its passes before with the normal one (9, among those listed).
			for (std::uint8_t filter = 1; filter <= 22; ++filter) {
				const bool multiplies = filter == 4 || filter == 5 || filter == 6 || (filter >= 11 && filter <= 14) || filter == 17 || filter == 18 || filter == 20 ||
				                        filter == 22;
				add({ .vertexKind = 0, .pixelKind = 0, .filter = filter }, multiplies ? std::vector<std::uint8_t>{ 12, 14, 7 } : std::vector<std::uint8_t>{ 9, 10, 11, 13, 15, 7 });
			}
			return states;
		}

		struct PassBindings
		{
			org::DeclaredViewToken colour;
			org::DeclaredViewToken depth;
			bool hasDepth = false;
			std::vector<std::pair<const void*, org::DeclaredViewToken>> reads;  // by FrameAnchors::Texture::Key
		};

		struct PassFrame
		{
			std::shared_ptr<const Slots> slots;
			std::shared_ptr<const RunGate> gate;
			std::uint32_t gateBase = 0;  // the instance's first gate entry
		};

		/**
		 * @brief One pass of a surface: the display a commit stored for this run, as one render pass.
		 *
		 * Each draw reads its vertex count from its command's entry of the client's run gate, which the anchors close at the start of
		 * every run and only this client's commit opens: a run without that commit (a frame left in a slot by a run that failed, a run
		 * kept by another client) draws nothing. Its Revision is FrameAnchors::CommitReadRevision: it is recorded at every
		 * run from the frame of that run, which names the display's pipelines, and its push blocks are that run's.
		 */
		class DisplayPass final : public org::TypedRenderGraphPass<DisplayPass, PassFrame, PassBindings>
		{
		public:
			DisplayPass(std::shared_ptr<const Surface> a_surface, std::shared_ptr<const Slots> a_slots, std::shared_ptr<const RunGate> a_gate, bool a_withDepth,
				std::shared_ptr<const Reads> a_reads, bool a_atExecution, std::uint32_t a_instance) :
				surface(std::move(a_surface)), slots(std::move(a_slots)), gate(std::move(a_gate)), withDepth(a_withDepth), reads(std::move(a_reads)),
				atExecution(a_atExecution), gateBase(a_instance * kGateEntries) {}

			PassBindings Declare(org::PassBuilder& a_builder)
			{
				PassBindings bindings{ a_builder.RenderTarget(surface->colour).View() };
				if (withDepth) {
					bindings.depth = a_builder.DepthReadWrite(surface->depth).View();
					bindings.hasDepth = true;
				}
				// The tracked textures the displays read: declared, so the graph orders the reads after the context's writes and moves
				// the images into the shader-read layout. The pass's own targets are left out.
				if (reads)
					for (const auto& read : *reads)
						if (read && read.Key() != surface->colour.Key() && (!withDepth || read.Key() != surface->depth.Key()))
							bindings.reads.emplace_back(read.Key(), a_builder.ShaderResource(read).View());
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { FrameAnchors::CommitReadRevision(atExecution, a_out); }

			PassFrame Prepare(const PassBindings&, const org::PassPrepareContext&) const { return { slots, gate, gateBase }; }

			// Recorded at execution: a surface no display of this instance drew on this run skips it, and its barriers fold into the next
			// pass that runs.
			static bool SkipsAtExecution(const PassBindings&, const PassFrame& a_pass, const org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots || !a_pass.gate)
					return true;
				const auto frame = a_pass.slots->Find(a_recording.Run());
				return !frame || !frame->staging || (frame->commands.empty() && !frame->clear);
			}

			static void Record(const PassBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots || !a_pass.gate)
					return;
				const org::RunRef run = a_recording.Run();
				const auto frame = a_pass.slots->Take(run);
				if (!frame || !frame->staging || (frame->commands.empty() && !frame->clear))
					return;
				a_recording.Retain(frame);
				a_recording.Retain(a_pass.gate);
				auto& commands = a_recording.Commands();

				rhi::ColorAttachment colour{};
				colour.rtv = a_recording.View(a_bindings.colour);
				colour.resource = a_recording.Resolve(a_bindings.colour).GetHandle();
				colour.loadOp = frame->clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
				colour.storeOp = rhi::StoreOp::Store;
				if (frame->clear) {
					colour.clear.type = rhi::ClearValueType::Color;
					colour.clear.format = frame->format;
					colour.clear.rgba[0] = colour.clear.rgba[1] = colour.clear.rgba[2] = colour.clear.rgba[3] = 0.0f;
				}
				rhi::DepthAttachment depth{};
				if (a_bindings.hasDepth) {
					depth.dsv = a_recording.View(a_bindings.depth);
					depth.resource = a_recording.Resolve(a_bindings.depth).GetHandle();
					depth.depthLoad = rhi::LoadOp::Load;
					depth.depthStore = rhi::StoreOp::Store;
					depth.stencilLoad = rhi::LoadOp::Load;
					depth.stencilStore = rhi::StoreOp::Store;
					depth.clear.type = rhi::ClearValueType::DepthStencil;
				}
				rhi::PassBeginInfo begin{};
				begin.colors = { &colour, 1 };
				begin.depth = a_bindings.hasDepth ? &depth : nullptr;
				begin.width = frame->width;
				begin.height = frame->height;
				begin.debugName = "CS Scaleform";
				commands.BeginPass(begin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.SetViewports({ &frame->viewport, 1 });
				commands.SetScissors({ &frame->scissor, 1 });
				// The run's gate region: its arena chunk and its offset there.
				const auto region = a_pass.gate->Latch().At(run);
				const auto latchBuffer = region.buffer->GetAPIResource().GetHandle();
				const std::uint64_t latchOffset = region.offset;
				const RasterProgram* bound = nullptr;
				std::uint32_t reference = 0;
				bool lines = false;
				for (std::uint32_t i = 0; i < frame->commands.size(); ++i) {
					const auto& command = frame->commands[i];
					if (command.kind == Command::Kind::ClearStencil) {
						// Inside the pass this is vkCmdClearAttachments over the render area: the whole target, as D3D11 clears it.
						if (a_bindings.hasDepth)
							commands.ClearDepthStencilView(depth.dsv, false, true, 0.0f, 0);
						continue;
					}
					if (!command.program || !command.program->drawSignature)
						continue;
					if (command.program != bound) {
						commands.BindLayout(command.program->layout->GetHandle());
						commands.BindPipeline(command.program->Pipeline());
						bound = command.program;
					}
					if (command.lines != lines) {
						commands.SetPrimitiveTopology(command.lines ? rhi::PrimitiveTopology::LineStrip : rhi::PrimitiveTopology::TriangleList);
						lines = command.lines;
					}
					if (command.stencilRef != reference) {
						commands.SetStencilRef(command.stencilRef);
						reference = command.stencilRef;
					}
					// A tracked texture's index comes from its declared view. A read this build does not declare yet (new since its build)
					// keeps the raw index until the build with it is attached.
					std::uint32_t words[sizeof(FillBlock) / 4];
					std::memcpy(words, &command.block, sizeof(words));
					// The stages' texture words: FillBlock::textures, or FilterBlock::source and second.
					const std::size_t first = (command.filter ? offsetof(FilterBlock, source) : offsetof(FillBlock, textures)) / 4;
					for (std::uint32_t stage = 0; stage < 2; ++stage) {
						if (!command.reads[stage])
							continue;
						const auto declared = std::ranges::find(a_bindings.reads, command.reads[stage], &std::pair<const void*, org::DeclaredViewToken>::first);
						if (declared != a_bindings.reads.end())
							words[first + stage] = a_recording.Index(declared->second);
					}
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kConstantWords, words);
					// The vertex count is the gate entry's: zero unless this run's commit opened it.
					commands.ExecuteIndirect(command.program->drawSignature->GetHandle(), latchBuffer, latchOffset + a_pass.gate->Offset(a_pass.gateBase + i), {}, 0, 1);
				}
				commands.EndPass();
			}

		private:
			std::shared_ptr<const Surface> surface;
			std::shared_ptr<const Slots> slots;
			std::shared_ptr<const RunGate> gate;
			bool withDepth = false;
			std::shared_ptr<const Reads> reads;
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
			std::uint32_t gateBase = 0;
		};

		/**
		 * @brief Native mode: the declared read of the display texture a_view, or null for one read untracked (exempt, or not native's).
		 * A tracked texture seen for the first time is added to the reads (the promoted exempt texture's own resource, or for any other
		 * tracked texture, graph-owned or host-made, its image's one graph resource through FrameAnchors::ShareImage), and the client
		 * registers again after the run, so the next build declares it. a_new says whether it was added now.
		 */
		const void* DeclaredRead(PathState& a_state, ID3D11ShaderResourceView* a_view, bool& a_new)
		{
			a_new = false;
			if (!a_view || !Engine::Native::OwnedResources())
				return nullptr;
			for (auto& read : a_state.reads)
				if (read.view.get() == a_view) {
					read.lastCommit = a_state.commits;
					return read.texture.Key();
				}
			FrameAnchors::Texture texture;
			if (auto promoted = Engine::Native::PromotedTextureOf(a_view)) {
				texture = FrameAnchors::Texture(std::move(promoted));
			} else if (Engine::Native::IsTrackedTexture(a_view)) {
				winrt::com_ptr<ID3D11Resource> resource;
				a_view->GetResource(resource.put());
				const auto texture2D = resource.try_as<ID3D11Texture2D>();
				DxvkOrgInteropResourceInfo info{};
				if (!texture2D || !RenderGraphRuntime::Get().DescribeResource(texture2D.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
					return nullptr;
				D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
				a_view->GetDesc(&viewDesc);
				org::TextureDescription desc{};
				desc.format = rhi::helpers::ToRHI(viewDesc.Format);
				desc.channels = 4;
				desc.hasSRV = true;
				desc.srvFormat = desc.format;
				texture = FrameAnchors::ShareImage(texture2D.get(), info.image, desc, "cs.scaleform.read");
			}
			if (!texture)
				return nullptr;
			Read read;
			read.view.copy_from(a_view);
			read.texture = texture;
			read.lastCommit = a_state.commits;
			a_state.reads.push_back(std::move(read));
			a_state.readsChanged = true;
			a_new = true;
			return texture.Key();
		}

		/** @brief The scratch image a_scratch's surface, or null. */
		const Surface* ScratchSurface(const PathState& a_state, std::int32_t a_scratch)
		{
			if (a_scratch < 0)
				return nullptr;
			for (const auto& surface : a_state.surfaces)
				if (surface->scratch == a_scratch)
					return surface.get();
			return nullptr;
		}

		/**
		 * @brief Inside the epoch: the pending display's frame, stored for this run in the slots of the pass that draws it, with its
		 * draws' gate entries opened. Every other pass's entry for this run is emptied first, so nothing older is left there.
		 */
		void Commit()
		{
			auto& state = Path();
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			const org::RunRef run = host->CurrentRun();
			// The tap's instance: the step's first instance plus its earlier runs this frame (0 on the anchor's own epoch).
			const std::uint32_t instance = FrameAnchors::TapInstance();
			if (instance >= kInstances) {
				state.commitReason = "an Interface run past the UI stage's instances";
				return;
			}
			for (const auto& surface : state.surfaces) {
				if (surface->Drawable())
					surface->colourSlots[instance]->Store(run, nullptr);
				if (surface->Masks())
					surface->depthSlots[instance]->Store(run, nullptr);
			}
			if (!state.pending || !state.pendingSurface)
				return;
			const Display& display = *state.pending;
			const Surface& surface = *state.pendingSurface;
			const auto& slotSet = state.pendingDepth ? surface.depthSlots : surface.colourSlots;
			if (slotSet.size() <= instance)
				return;
			const auto& slots = slotSet[instance];
			auto frame = std::make_shared<Frame>();

			++state.commits;
			std::vector<std::uint32_t> textures(display.textures.size(), 0);
			std::vector<const void*> reads(display.textures.size(), nullptr);
			for (std::size_t i = 0; i < display.textures.size(); ++i) {
				auto* view = display.textures[i];
				bool added = false;
				reads[i] = DeclaredRead(state, view, added);
				// The raw index: what an exempt texture is read through, and a tracked one until a build declares it. A read declared
				// before this commit binds its declared view in the pass, so its raw index is only the fallback until that build is
				// attached: the undeclared-access validator counts it as declared (interim).
				std::optional<RenderGraph::UndeclaredAccess::DeclaredViewScope> declared;
				if (reads[i] && !added)
					declared.emplace();
				auto binding = DCLF::GpuTextures::Get().ResolveBinding(view, kSourceTag);
				declared.reset();
				if (binding.index == DCLF::GpuTextures::kInvalid) {
					state.commitReason = "a texture's view cannot be resolved";
					return;
				}
				textures[i] = binding.index;
				frame->owners.push_back(std::move(binding.owner));
				// The view itself too, until the recording's GPU work retires: EndDisplay's queued releases may drop GFx's.
				view->AddRef();
				frame->owners.push_back(std::shared_ptr<const void>(view, [](const void* a_view) {
					static_cast<ID3D11ShaderResourceView*>(const_cast<void*>(a_view))->Release();
				}));
			}

			frame->staging = state.staging->Acquire(std::max<std::uint64_t>(display.staging.size(), 4));
			if (!frame->staging) {
				state.commitReason = "no staging buffer";
				return;
			}
			std::memcpy(frame->staging->Bytes().data(), display.staging.data(), display.staging.size());

			const std::size_t first = state.pendingFirst;
			const std::size_t last = std::min<std::size_t>(first + state.pendingCount, display.commands.size());
			frame->commands.reserve(last - first);
			for (std::size_t i = first; i < last; ++i) {
				const Command& command = display.commands[i];
				FrameCommand& out = frame->commands.emplace_back();
				out.kind = command.kind;
				if (command.kind != Command::Kind::Draw)
					continue;
				out.program = state.pendingPrograms[i].get();
				out.stencilRef = command.stencilRef;
				out.lines = command.state.lines != 0;
				const auto textureOf = [&](std::uint32_t a_stage) {
					const std::int32_t texture = command.textures[a_stage];
					return texture >= 0 ? textures[static_cast<std::size_t>(texture)] : 0u;
				};
				for (std::uint32_t stage = 0; stage < 2; ++stage) {
					const std::int32_t texture = command.textures[stage];
					// A scratch image is read through the view every pass declares (its index replaces the block's word in Record).
					const auto* scratch = ScratchSurface(state, ScratchOf(texture));
					out.reads[stage] = texture >= 0 ? reads[static_cast<std::size_t>(texture)] : (scratch ? scratch->colour.Key() : nullptr);
				}
				out.filter = command.state.filter != 0;
				if (command.state.filter) {
					FilterBlock filter{};
					filter.geometry = frame->staging->SrvIndex();
					filter.vertexOffset = command.record;
					filter.matrixOffset = command.record + kFilterMatrixOffset;
					filter.constantsOffset = command.record + kFilterConstantsOffset;
					filter.source = textureOf(0);
					filter.second = textureOf(1);
					filter.sampler0 = state.samplers[command.samplers[0] & 3].Index();
					filter.sampler1 = state.samplers[command.samplers[1] & 3].Index();
					std::memcpy(&out.block, &filter, sizeof(filter));
					continue;
				}
				out.block.geometry = frame->staging->SrvIndex();
				out.block.record = command.record;
				for (std::uint32_t stage = 0; stage < 2; ++stage) {
					out.block.textures[stage] = textureOf(stage);
					out.block.samplers[stage] = state.samplers[command.samplers[stage] & 3].Index();
				}
				out.block.samplers[2] = out.block.samplers[3] = out.block.samplers[0];
			}
			for (const auto& program : state.pendingPrograms)
				if (program)
					frame->owners.push_back(program);
			for (const auto& sampler : state.samplers)
				frame->owners.push_back(sampler.Owner());

			frame->width = surface.width;
			frame->height = surface.height;
			frame->clear = display.clearTarget && state.pendingFirst == 0;
			frame->format = surface.colourFormat;
			frame->viewport = { display.viewport[0], display.viewport[1], display.viewport[2], display.viewport[3], 0.0f, 1.0f };
			// D3D11 clips to the viewport (the renderer's rasterizer state has no scissor).
			const float left = std::clamp(display.viewport[0], 0.0f, static_cast<float>(surface.width));
			const float top = std::clamp(display.viewport[1], 0.0f, static_cast<float>(surface.height));
			const float right = std::clamp(display.viewport[0] + display.viewport[2], left, static_cast<float>(surface.width));
			const float bottom = std::clamp(display.viewport[1] + display.viewport[3], top, static_cast<float>(surface.height));
			frame->scissor = { static_cast<std::int32_t>(std::floor(left)), static_cast<std::int32_t>(std::floor(top)),
				static_cast<std::uint32_t>(std::ceil(right) - std::floor(left)), static_cast<std::uint32_t>(std::ceil(bottom) - std::floor(top)) };

			slots->Store(run, std::move(frame));
			// The run's commands use the instance's gate entries from its first, in order.
			for (std::size_t i = first; i < last; ++i)
				if (display.commands[i].kind == Command::Kind::Draw && state.pendingPrograms[i])
					state.gate->OpenDraw(instance * kGateEntries + static_cast<std::uint32_t>(i - first), display.commands[i].vertexCount);
			state.committed = true;
		}

		void RegisterReads(PathState& a_state);

		FrameAnchors::ClientDesc MakeClient(const PathState& a_state)
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			std::shared_ptr<const RunGate> gate = a_state.gate;
			auto declared = std::make_shared<Reads>();
			for (const auto& read : a_state.reads)
				declared->push_back(read.texture);
			// The scratch images, read by any pass but their own (DisplayPass::Declare leaves its own target out).
			for (const auto& surface : a_state.surfaces)
				if (surface->scratch >= 0 && surface->Drawable())
					declared->push_back(surface->colour);
			std::shared_ptr<const Reads> reads = std::move(declared);
			for (std::size_t i = 0; i < a_state.surfaces.size(); ++i) {
				std::shared_ptr<const Surface> surface = a_state.surfaces[i];
				if (!surface->Drawable())
					continue;
				const auto add = [&](const std::vector<std::shared_ptr<const Slots>>& a_slots, bool a_withDepth, const char* a_suffix, std::int32_t a_order) {
					FrameAnchors::PassDesc pass{ .name = fmt::format("cs.scaleform.draw.{}{}", i, a_suffix),
						.anchor = Anchor::Interface,
						.order = a_order,
						.kind = FrameAnchors::PassKind::Render,
						// It reads the run's slot, which the commit stores: recorded then, without a ticket prepared again.
						.atExecution = true,
						// Gated: a run whose commit stored no frame in the slot records nothing (DisplayPass::Record), its clear included.
						.gated = true };
					// One pass per instance (the k-th display segment of the frame), each reading its own slots and gate entries.
					pass.makeInstance = [surface, a_slots, gate, a_withDepth, reads](std::uint32_t a_instance) -> std::shared_ptr<org::RenderPass> {
						if (a_instance >= a_slots.size())
							return nullptr;
						// Recorded at execution in native mode (an anchor's own epoch) and in the UI stage's late batch alike.
						return std::make_shared<DisplayPass>(surface, a_slots[a_instance], gate, a_withDepth, reads, FrameAnchors::RecordsAtExecution(Anchor::Interface),
							a_instance);
					};
					client.passes.push_back(std::move(pass));
				};
				add(surface->colourSlots, false, "", 100 + 2 * static_cast<std::int32_t>(i));
				if (surface->Masks())
					add(surface->depthSlots, true, ".masks", 101 + 2 * static_cast<std::int32_t>(i));
			}
			// Fixed instances in the UI stage, all declared: how many displays the engine draws is its late decision. Made instance by
			// instance, so a later display's passes follow an earlier one's on every surface.
			client.maxInstances = kInstances;
			client.instances = [] { return kInstances; };
			client.instanceMajor = true;
			// A registration for new reads keeps the surfaces, slots and gate: the schedule's older registration draws the displays.
			client.matchesById = true;
			client.generation = a_state.surfacesGeneration;
			client.registerResources = [](org::RenderGraph& a_graph) {
				for (const auto& surface : Path().surfaces)
					if (!surface->ownId.empty() && surface->colour.Plain())
						a_graph.RegisterResource(org::ResourceIdentifier(surface->ownId), surface->colour.Plain());
			};
			client.commit = [](const FrameAnchors::Point&) { Commit(); };
			client.active = [] { return Path().pending != nullptr; };
			client.gates = { a_state.gate };
			// New reads register again once the display's last run is done (Submit), never between its runs.
			client.executed = [](const FrameAnchors::Point&) {
				auto& state = Path();
				if (state.pending)
					state.executed = true;
			};
			client.onFailed = [] {
				Path().failed = true;
				logger::error("[ORG] Scaleform: the render graph failed; the D3D11 renderer draws every display from now on");
			};
			return client;
		}

		/**
		 * @brief Registers the client with a pass pair for every surface. At setup, the setup's build point (Engine::PostSetup) builds
		 * the Interface graph with it before the first display; after a role's surface was made again, the next build point between
		 * frames does. False, and the path failed, when it is refused.
		 */
		bool RegisterClient(PathState& a_state)
		{
			if (!FrameAnchors::Register(MakeClient(a_state))) {
				a_state.failed = true;
				logger::error("[ORG] Scaleform: the Interface anchor's client could not be registered; D3D11 draws every display");
				return false;
			}
			++a_state.registrations;
			const auto drawable = std::ranges::count_if(a_state.surfaces, [](const auto& a_surface) { return a_surface->Drawable(); });
			logger::info("[ORG] Scaleform: registered with {} surfaces (registration {})", drawable, a_state.registrations);
			return true;
		}

		/**
		 * @brief Native mode: registers the client again with the reads as they are now, after a run that found a new tracked texture.
		 * Reads no commit used for kReadIdleCommits are dropped first. The Interface graph is built again off the render thread; until
		 * it is attached, the graph built before draws the displays, the new reads through their raw indices.
		 */
		void RegisterReads(PathState& a_state)
		{
			a_state.readsChanged = false;
			std::erase_if(a_state.reads, [&](const Read& a_read) { return a_read.lastCommit + kReadIdleCommits < a_state.commits; });
			logger::info("[ORG] Scaleform: {} tracked textures read by the displays; the Interface graph is built again to declare them", a_state.reads.size());
			RegisterClient(a_state);
		}

		/** @brief The engine render target whose texture is a_texture (DXVK's), or -1. */
		std::int32_t EngineTargetOf(ID3D11Texture2D* a_texture)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_texture)
				return -1;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL); ++i)
				if (targets[i].texture && Tripwire::Unwrap(targets[i].texture) == a_texture)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/** @brief The refusal for a display onto another target, naming the engine target when it is one (counted per name). */
		const char* OtherTargetReason(ID3D11Texture2D* a_texture)
		{
			static const std::array<std::string, static_cast<std::size_t>(RE::RENDER_TARGET::kTOTAL)> s_engine = [] {
				std::array<std::string, static_cast<std::size_t>(RE::RENDER_TARGET::kTOTAL)> names;
				for (std::size_t i = 0; i < names.size(); ++i)
					names[i] = fmt::format("a render target that is not a surface (engine target {})", i);
				return names;
			}();
			const auto target = EngineTargetOf(a_texture);
			return target >= 0 ? s_engine[static_cast<std::size_t>(target)].c_str() :
			                     "a render target that is not a surface (not an engine target)";
		}

		/** @brief The engine depth-stencil target whose texture is a_texture (DXVK's), or -1. */
		std::int32_t EngineDepthOf(ID3D11Texture2D* a_texture)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_texture)
				return -1;
			const auto& targets = renderer->GetDepthStencilData().depthStencils;
			for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
				if (targets[i].texture && Tripwire::Unwrap(targets[i].texture) == a_texture)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/** @brief The engine's framebuffer RTV (the back buffer's): HDR's saved one while HDR points kFRAMEBUFFER at its UI texture. */
		ID3D11RenderTargetView* FramebufferRtv()
		{
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return nullptr;
			auto* rtv = renderer->GetRuntimeData().renderTargets[static_cast<std::uint32_t>(RE::RENDER_TARGET::kFRAMEBUFFER)].RTV;
			const auto& hdr = globals::features::hdrDisplay;
			if (hdr.uiTexture && hdr.uiTexture->rtv && rtv == hdr.uiTexture->rtv.get() && hdr.savedFramebufferRTV)
				rtv = hdr.savedFramebufferRTV;
			return rtv;
		}

		/** @brief a_rtv's texture (DXVK's, referenced) and view format; null for a view that is not of a 2D texture. */
		winrt::com_ptr<ID3D11Texture2D> TextureOf(ID3D11RenderTargetView* a_rtv, DXGI_FORMAT& a_format)
		{
			a_format = DXGI_FORMAT_UNKNOWN;
			if (!a_rtv)
				return nullptr;
			auto* rtv = Tripwire::Unwrap(a_rtv);
			winrt::com_ptr<ID3D11Resource> resource;
			rtv->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return nullptr;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			rtv->GetDesc(&desc);
			a_format = desc.Format;
			return texture;
		}

		/** @brief The role's texture now (DXVK's, referenced) and its view format; null when the role has none (HDR off). */
		winrt::com_ptr<ID3D11Texture2D> RoleTexture(Role a_role, DXGI_FORMAT& a_format)
		{
			if (a_role == Role::Framebuffer)
				return TextureOf(FramebufferRtv(), a_format);
			if (a_role == Role::Main || IsBook(a_role)) {
				auto* renderer = globals::game::renderer;
				a_format = DXGI_FORMAT_UNKNOWN;
				const auto target = a_role == Role::Main ? static_cast<std::uint32_t>(RE::RENDER_TARGET::kMAIN) :
				                                           static_cast<std::uint32_t>(RE::RENDER_TARGET::kBOOK_TEXT_0) + (static_cast<std::uint32_t>(a_role) - static_cast<std::uint32_t>(Role::Book0));
				return renderer ? TextureOf(renderer->GetRuntimeData().renderTargets[target].RTV, a_format) : nullptr;
			}
			const auto& hdr = globals::features::hdrDisplay;
			a_format = DXGI_FORMAT_UNKNOWN;
			return hdr.loaded && hdr.uiTexture && hdr.uiTexture->rtv ? TextureOf(hdr.uiTexture->rtv.get(), a_format) : nullptr;
		}

		const char* RoleName(Role a_role)
		{
			switch (a_role) {
			case Role::Framebuffer:
				return "framebuffer";
			case Role::HdrUi:
				return "HDR UI";
			case Role::Book0:
				return "kBOOK_TEXT_0";
			case Role::Book1:
				return "kBOOK_TEXT_1";
			case Role::Book2:
				return "kBOOK_TEXT_2";
			case Role::Book3:
				return "kBOOK_TEXT_3";
			default:
				return "kMAIN";
			}
		}

		/** @brief kMAIN's depth-stencil texture (DXVK's), the one UI::DrawInterface binds; null without one. */
		ID3D11Texture2D* MainDepth()
		{
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return nullptr;
			const auto& depth = renderer->GetDepthStencilData().depthStencils[static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)];
			return depth.texture ? Tripwire::Unwrap(depth.texture) : nullptr;
		}

		/** @brief The depth-stencil a role's displays mask with (DXVK's): kBOOK_TEXT's for the book's pages, kMAIN's for the rest. */
		ID3D11Texture2D* RoleDepth(Role a_role)
		{
			if (!IsBook(a_role))
				return MainDepth();
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return nullptr;
			const auto& depth = renderer->GetDepthStencilData().depthStencils[static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kBOOK_TEXT)];
			return depth.texture ? Tripwire::Unwrap(depth.texture) : nullptr;
		}

		/**
		 * @brief a_texture (DXVK's), not an engine target, as a graph texture through the anchors' imports, so the image is one graph
		 * resource whoever else imports it. HDR's UI texture by the name and views Bink imports it with ("hdr.ui"), any other as a render
		 * target in a_format under a_id.
		 */
		FrameAnchors::Texture ImportTarget(ID3D11Texture2D* a_texture, DXGI_FORMAT a_format, const std::string& a_id)
		{
			const auto& hdr = globals::features::hdrDisplay;
			if (hdr.uiTexture && hdr.uiTexture->resource &&
				(hdr.uiTexture->resource.get() == a_texture || Tripwire::Unwrap(hdr.uiTexture->resource.get()) == a_texture)) {
				const auto& ui = *hdr.uiTexture;
				return FrameAnchors::D3D11Texture("hdr.ui", ui.resource.get(), ui.srv.get(), ui.uav.get(), ui.rtv.get());
			}
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_format);
			desc.channels = 4;
			desc.hasRTV = true;
			desc.rtvFormat = desc.format;
			return FrameAnchors::ImportTexture(a_id, a_texture, desc);
		}

		/**
		 * @brief The graph texture of the role's target a_colour (DXVK's): an engine target's slot, HDR's twin (a_ownId set to the id this
		 * client registers it under), or this client's import, one id per role (a texture made again for the role is that import's next
		 * version, or a new slot, never one more id).
		 */
		FrameAnchors::Texture ColourOf(Role a_role, ID3D11Texture2D* a_colour, DXGI_FORMAT a_format, std::string& a_ownId)
		{
			a_ownId.clear();
			if (const auto target = EngineTargetOf(a_colour); target >= 0)
				return FrameAnchors::EngineTarget(static_cast<std::uint32_t>(target));
			if (auto owned = ORGHDRDisplay::Get().GraphOf(a_colour)) {
				// HDR's UI texture as the graph owns it (CS_ORG_HDR_DISPLAY): the twin, which the composite reads.
				a_ownId = "cs.scaleform.hdr-ui";
				return owned;
			}
			return ImportTarget(a_colour, a_format, fmt::format("cs.scaleform.target.{}", static_cast<int>(a_role)));
		}

		/** @brief The graph texture of a depth-stencil a_depth (DXVK's): an engine depth target's slot, or none. */
		FrameAnchors::Texture DepthTextureOf(ID3D11Texture2D* a_depth)
		{
			if (!a_depth)
				return {};
			const auto depth = EngineDepthOf(a_depth);
			return depth >= 0 ? FrameAnchors::EngineDepth(static_cast<std::uint32_t>(depth)) : FrameAnchors::Texture{};
		}

		/** @brief The role's surface for a_colour with a_depth (DXVK's), imported; without passes when the target cannot be imported. */
		std::shared_ptr<Surface> MakeSurface(Role a_role, ID3D11Texture2D* a_colour, DXGI_FORMAT a_format, ID3D11Texture2D* a_depth)
		{
			auto surface = std::make_shared<Surface>();
			surface->role = a_role;
			surface->colourTexture.copy_from(a_colour);
			surface->depthTexture.copy_from(a_depth);
			D3D11_TEXTURE2D_DESC colourDesc{};
			a_colour->GetDesc(&colourDesc);
			surface->width = colourDesc.Width;
			surface->height = colourDesc.Height;
			surface->colourFormat = rhi::helpers::ToRHI(a_format);
			surface->colour = ColourOf(a_role, a_colour, a_format, surface->ownId);
			if (a_depth) {
				surface->depth = DepthTextureOf(a_depth);
				if (surface->depth) {
					D3D11_TEXTURE2D_DESC depthDesc{};
					a_depth->GetDesc(&depthDesc);
					surface->width = std::min(surface->width, depthDesc.Width);
					surface->height = std::min(surface->height, depthDesc.Height);
					surface->depthFormat = surface->depth.Format();
				}
			}
			logger::info("[ORG] Scaleform: the {} surface: {} ({}x{}, DXGI format {}), depth {}", RoleName(a_role),
				!surface->ownId.empty() ? surface->ownId : (surface->colour.Id().empty() ? std::string("an engine target") : surface->colour.Id()), colourDesc.Width, colourDesc.Height, static_cast<int>(a_format),
				surface->depth ? "an engine depth-stencil" : (a_depth ? "not importable" : "none"));
			if (surface->colour && surface->colourFormat != rhi::Format::Unknown) {
				surface->colourSlots = MakeSlots();
				if (surface->depth)
					surface->depthSlots = MakeSlots();
			}
			return surface;
		}

		/**
		 * @brief Every pipeline the surface's passes can draw with (DrawStates), queued on the program build thread: they build while
		 * the game loads, from shaders the build compiles into the shader store. A key a display needs before its build finished is
		 * built on the render thread (ProgramCache::Require, logged).
		 */
		void QueuePrograms(PathState& a_state, const Surface& a_surface)
		{
			if (!a_surface.Drawable())
				return;
			std::size_t queued = 0;
			const auto queue = [&](bool a_withDepth) {
				for (const auto& drawState : DrawStates(a_withDepth)) {
					const ProgramKey key{ drawState, a_surface.colourFormat, a_withDepth ? a_surface.depthFormat : rhi::Format::Unknown };
					(void)a_state.programs.Get(key, [&] { return DescOf(key); });
					++queued;
				}
			};
			queue(false);
			if (a_surface.Masks())
				queue(true);
			logger::info("[ORG] Scaleform: {} pipelines queued for the {} surface", queued, RoleName(a_surface.role));
		}

		/** @brief The surface whose colour is a_colour (DXVK's), or null. */
		Surface* Find(const PathState& a_state, ID3D11Texture2D* a_colour)
		{
			for (const auto& surface : a_state.surfaces)
				if (surface->scratch < 0 && a_colour && surface->colourTexture.get() == a_colour)
					return surface.get();
			return nullptr;
		}

		/**
		 * @brief The role's surface made from its texture now (with kMAIN's depth-stencil), its pipelines queued, replacing the role's
		 * old one; false when the role has no texture (HDR off).
		 */
		bool DeclareSurface(PathState& a_state, Role a_role)
		{
			DXGI_FORMAT format{};
			const auto texture = RoleTexture(a_role, format);
			if (!texture || format == DXGI_FORMAT_UNKNOWN)
				return false;
			auto surface = MakeSurface(a_role, texture.get(), format, RoleDepth(a_role));
			QueuePrograms(a_state, *surface);
			std::erase_if(a_state.surfaces, [&](const std::shared_ptr<Surface>& a_old) {
				if (a_old->scratch >= 0 || a_old->role != a_role)
					return false;
				// A pass of an older build that still holds it draws nothing more; its textures go once D3D11 and the graph are done.
				a_old->Clear();
				RenderGraphRuntime::Get().ReleaseAfterD3D11(a_old);
				return true;
			});
			a_state.surfaces.push_back(std::move(surface));
			return true;
		}

		/**
		 * @brief The role's surface kept for its texture made again (a resize, HDR's textures made again) when the texture is the same
		 * graph texture as before: the slot of an engine target or of this client's import takes the new texture as its next version, so
		 * the passes stay and nothing is built. Its D3D11 identity and size follow the new texture. Null when the graph texture changed
		 * (another slot, another format, another depth-stencil): the caller makes the surface again.
		 */
		Surface* RefreshSurface(PathState& a_state, Role a_role, const Display& a_display)
		{
			const auto found = std::ranges::find_if(a_state.surfaces, [&](const auto& a_surface) { return a_surface->scratch < 0 && a_surface->role == a_role; });
			if (found == a_state.surfaces.end() || !(*found)->Drawable())
				return nullptr;
			Surface& surface = **found;
			if (surface.colourFormat != rhi::helpers::ToRHI(a_display.targetFormat))
				return nullptr;
			std::string ownId;
			const auto colour = ColourOf(a_role, a_display.target, a_display.targetFormat, ownId);
			if (!colour || !(colour == surface.colour) || ownId != surface.ownId)
				return nullptr;
			auto* depthTexture = RoleDepth(a_role);
			const auto depth = DepthTextureOf(depthTexture);
			if (!(depth == surface.depth))
				return nullptr;
			surface.colourTexture.copy_from(a_display.target);
			surface.depthTexture.copy_from(depthTexture);
			D3D11_TEXTURE2D_DESC desc{};
			a_display.target->GetDesc(&desc);
			surface.width = desc.Width;
			surface.height = desc.Height;
			if (depthTexture && surface.depth) {
				D3D11_TEXTURE2D_DESC depthDesc{};
				depthTexture->GetDesc(&depthDesc);
				surface.width = std::min(surface.width, depthDesc.Width);
				surface.height = std::min(surface.height, depthDesc.Height);
			}
			logger::info("[ORG] Scaleform: the {} texture was made again ({}x{}); its surface draws it as the slot's next version, nothing is built",
				RoleName(a_role), desc.Width, desc.Height);
			return &surface;
		}

		/**
		 * @brief The surface a_display draws onto. Only the framebuffer's and HDR's UI texture's, declared at setup, are drawn on the
		 * graph. When one of those roles' textures was made again (a resize, HDR's textures made again), its surface is made again
		 * and the client registered again: the Interface graph is built with it at the next build point, between frames (a rebuild
		 * trigger, logged). Null, with a_reason, otherwise.
		 */
		Surface* SurfaceFor(PathState& a_state, const Display& a_display, bool a_needsDepth, const char*& a_reason)
		{
			if (a_display.scratch != -1) {
				// A render target GFx pushed: drawn into its scratch image.
				auto* scratch = const_cast<Surface*>(ScratchSurface(a_state, a_display.scratch));
				if (!scratch || !scratch->Drawable())
					a_reason = "a render target of GFx's that no scratch image could take (larger than the screen, or all in use)";
				return scratch && scratch->Drawable() ? scratch : nullptr;
			}
			if (auto* surface = Find(a_state, a_display.target)) {
				if (!surface->Drawable()) {
					a_reason = "a render target the graph cannot import";
					return nullptr;
				}
				if (surface->colourFormat != rhi::helpers::ToRHI(a_display.targetFormat)) {
					a_reason = "a render target view in another format than its surface's";
					return nullptr;
				}
				if (a_needsDepth && (!surface->Masks() || a_display.depth != surface->depthTexture.get())) {
					a_reason = "a mask on a depth-stencil other than kMAIN's, or one the graph cannot import";
					return nullptr;
				}
				return surface;
			}
			for (const auto role : kRoles) {
				DXGI_FORMAT format{};
				if (RoleTexture(role, format).get() != a_display.target)
					continue;
				if (auto* refreshed = RefreshSurface(a_state, role, a_display); refreshed)
					return refreshed;
				logger::info("[ORG] Scaleform: the {} texture was made again as another graph resource; the Interface graph is built again with it",
					RoleName(role));
				if (DeclareSurface(a_state, role)) {
					++a_state.surfacesGeneration;
					if (RegisterClient(a_state))
						a_state.surfacesRegistration = a_state.registrations;
				}
				a_reason = "a surface made again (drawn once the Interface graph is built with it)";
				return nullptr;
			}
			a_reason = OtherTargetReason(a_display.target);
			return nullptr;
		}

		/** @brief Logs each new power of two of the largest segment (more than kGateEntries commands take more than one run). */
		void NoteSegment(PathState& a_state, std::uint32_t a_commands)
		{
			if (a_commands <= a_state.largestSegment)
				return;
			const bool log = a_commands >= 64 && std::bit_floor(a_commands) > std::bit_floor(std::max(a_state.largestSegment, 1u));
			a_state.largestSegment = a_commands;
			if (log)
				logger::info("[ORG] Scaleform: the largest segment so far has {} commands (drawn in runs of at most {})", a_commands, kGateEntries);
		}

		/**
		 * @brief A draw whose pipeline could not be built: the ProgramCache logged the build's error once; this names the key, once.
		 * The draw is left out and the rest of its segment draws.
		 */
		void NoteFailedProgram(PathState& a_state, const ProgramKey& a_key)
		{
			if (!a_state.failedPrograms.insert(a_key).second)
				return;
			logger::error(
				"[ORG] Scaleform programs: the pipeline for vertex kind {}, pixel kind {}, filter {}, blend {}, stencil {}, {} (colour format {}, "
				"depth format {}) could not be built; its draws are left out",
				a_key.state.vertexKind, a_key.state.pixelKind, a_key.state.filter, a_key.state.blend, static_cast<int>(a_key.state.stencil),
				a_key.state.lines ? "line strip" : "triangle list", static_cast<int>(a_key.colour), static_cast<int>(a_key.depth));
		}

		/** @brief A run that did not draw: an error of the graph path, logged once per reason. Not a readiness state. */
		void Failure(const char* a_reason)
		{
			static std::vector<const char*> logged;
			if (std::ranges::find(logged, a_reason) != logged.end())
				return;
			logged.push_back(a_reason);
			logger::error("[ORG] Scaleform: a segment did not draw on the graph ({}); it is left out", a_reason);
		}

		/** @brief Submit's reason for a run that did not submit. */
		const char* ReasonOf(FrameAnchors::RunStatus a_status)
		{
			switch (a_status) {
			case FrameAnchors::RunStatus::Idle:
				return "the Interface epoch did not run (idle)";
			case FrameAnchors::RunStatus::Refused:
				return "the Interface epoch refused the run";
			default:
				return "the Interface epoch failed";
			}
		}
	}

	namespace
	{
		/**
		 * @brief The scratch images: kScratchTargets graph textures of this client's, the largest role surface's size, registered with
		 * the Interface graph and declared by every pass. GFx's pushed render targets are drawn into them (a filter's source and its
		 * passes); the draws that read GFx's texture read its scratch image. Each has a pass that draws into it. Their pipelines queued.
		 */
		void MakeScratch(PathState& a_state)
		{
			for (const auto& surface : a_state.surfaces)
				if (surface->Drawable()) {
					a_state.scratchWidth = std::max(a_state.scratchWidth, surface->width);
					a_state.scratchHeight = std::max(a_state.scratchHeight, surface->height);
				}
			if (!a_state.scratchWidth || !a_state.scratchHeight)
				return;
			for (std::uint32_t i = 0; i < kScratchTargets; ++i) {
				org::TextureDescription desc{};
				desc.imageDimensions = { { a_state.scratchWidth, a_state.scratchHeight, 0, 0 } };
				desc.channels = 4;
				desc.format = kScratchFormat;
				desc.hasRTV = true;
				desc.rtvFormat = kScratchFormat;
				desc.hasSRV = true;
				desc.srvFormat = kScratchFormat;
				auto texture = org::PixelBuffer::CreateShared(desc);
				if (!texture) {
					logger::warn("[ORG] Scaleform: scratch image {} could not be made; GFx's render targets beyond it are not drawn", i);
					break;
				}
				auto surface = std::make_shared<Surface>();
				surface->scratch = static_cast<std::int32_t>(i);
				surface->ownId = fmt::format("cs.scaleform.scratch.{}", i);
				texture->SetName(surface->ownId);
				surface->colour = FrameAnchors::Texture(std::static_pointer_cast<org::Resource>(texture));
				surface->colourFormat = kScratchFormat;
				surface->width = a_state.scratchWidth;
				surface->height = a_state.scratchHeight;
				surface->colourSlots = MakeSlots();
				if (i == 0)
					QueuePrograms(a_state, *surface);
				a_state.surfaces.push_back(std::move(surface));
			}
			logger::info("[ORG] Scaleform: {} scratch images of {}x{} for GFx's render targets", kScratchTargets, a_state.scratchWidth, a_state.scratchHeight);
		}
	}

	bool ScratchSize(std::uint32_t& a_width, std::uint32_t& a_height)
	{
		const auto& state = Path();
		a_width = state.scratchWidth;
		a_height = state.scratchHeight;
		return state.setUp && a_width && a_height;
	}

	std::uint32_t ScratchCount()
	{
		const auto& state = Path();
		return static_cast<std::uint32_t>(std::ranges::count_if(state.surfaces, [](const auto& a_surface) { return a_surface->scratch >= 0; }));
	}

	bool Reserve()
	{
		auto& state = Path();
		state.reserved = FrameAnchors::Reserve(Anchor::Interface);
		if (!state.reserved)
			logger::warn("[ORG] Scaleform: CS_ORG_SCALEFORM=1, but the Interface anchor could not be reserved; the D3D11 renderer draws");
		return state.reserved;
	}

	void Setup()
	{
		auto& state = Path();
		if (!state.reserved || state.setUp || state.failed)
			return;
		if (!RenderGraphRuntime::Get().IsActive() || !FrameAnchors::HasEpoch(Anchor::Interface)) {
			state.failed = true;
			logger::warn("[ORG] Scaleform: CS_ORG_SCALEFORM=1, but the render graph or its Interface anchor is not active; the D3D11 renderer draws");
			return;
		}
		for (std::uint32_t i = 0; i < state.samplers.size(); ++i) {
			// CreateShaders' samplers: point or linear (sample mode), wrap or clamp (wrap mode), LOD bias -0.5, the top mip only.
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = (i & 1) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
			desc.AddressU = desc.AddressV = desc.AddressW = (i & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
			desc.MipLODBias = -0.5f;
			desc.MaxAnisotropy = 1;
			desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
			desc.MinLOD = 0.0f;
			desc.MaxLOD = 0.0f;
			state.samplers[i] = ShaderBindings::CreateSamplerFromD3D11(desc);
			if (!state.samplers[i]) {
				state.failed = true;
				logger::error("[ORG] Scaleform: the samplers could not be made (no descriptor service); the D3D11 renderer draws");
				return;
			}
		}
		// kGateEntries per instance (the UI stage's taps of one frame each have their own).
		state.gate = RunGate::Create("cs.scaleform.gate", kGateEntries * kInstances);
		state.staging = FrameStaging::Create({ .name = "cs.scaleform.staging",
			.minimumBytes = kMinimumStaging,
			// About the Interface anchor's runs a frame: a display, and one more per render target or filter pass in it.
			.runsPerFrame = kInstances });
		if (!state.gate || !state.staging) {
			state.failed = true;
			logger::error("[ORG] Scaleform: the run gate or the staging pool could not be made; the D3D11 renderer draws");
			return;
		}
		for (const auto role : kRoles)
			(void)DeclareSurface(state, role);
		MakeScratch(state);
		state.surfacesGeneration = 1;
		if (std::ranges::none_of(state.surfaces, [](const auto& a_surface) { return a_surface->Drawable(); })) {
			state.failed = true;
			logger::error("[ORG] Scaleform: none of the surfaces' targets can be imported; the D3D11 renderer draws");
			return;
		}
		if (!RegisterClient(state))
			return;
		state.surfacesRegistration = state.registrations;
		state.setUp = true;
		logger::info("[ORG] Scaleform: on (CS_ORG_SCALEFORM); the client joins the Interface graph at setup's build point");
	}

	bool Ready()
	{
		const auto& state = Path();
		return state.setUp && !state.failed && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(Anchor::Interface);
	}

	bool Submit(Display& a_display, const char*& a_reason, bool& a_lagged)
	{
		auto& state = Path();
		a_lagged = false;
		if (!Ready()) {
			a_reason = "the graph path is off (not set up, or failed)";
			return false;
		}
		if (a_display.commands.empty() && !a_display.clearTarget)
			return true;  // nothing drawn, as the D3D11 renderer would draw nothing
		const auto commandCount = static_cast<std::uint32_t>(a_display.commands.size());
		NoteSegment(state, commandCount);
		// Without a depth-stencil bound the D3D11 masks do nothing either; with one, the graph must have it. The scratch images have
		// none: masks inside a render target GFx pushed are not drawn (its stencil surface is not a scratch image yet).
		const bool needsDepth = a_display.scratch == -1 && a_display.depth && std::ranges::any_of(a_display.commands, [](const Command& a_command) {
			return a_command.kind == Command::Kind::ClearStencil || a_command.state.stencil != Stencil::Off;
		});
		Surface* surface = SurfaceFor(state, a_display, needsDepth, a_reason);
		if (!surface)
			return false;

		// Every pipeline was queued at setup; one the build thread has not finished yet is built here, never left to D3D11. A key that
		// failed to build leaves its draw out (logged once); the rest of the segment draws.
		state.pendingPrograms.assign(a_display.commands.size(), nullptr);
		for (std::size_t i = 0; i < a_display.commands.size(); ++i) {
			const Command& command = a_display.commands[i];
			if (command.kind != Command::Kind::Draw)
				continue;
			ProgramKey key{ command.state, surface->colourFormat, needsDepth ? surface->depthFormat : rhi::Format::Unknown };
			if (!needsDepth)
				key.state.stencil = Stencil::Off;
			state.pendingPrograms[i] = state.programs.Require(key, [&] { return DescOf(key); });
			if (!state.pendingPrograms[i])
				NoteFailedProgram(state, key);
		}
		// The UI stage takes the run (a world frame): its schedule must hold a registration with the surfaces as they are. Until the stage's
		// build with new surfaces switches in, the display lags (a schedule change takes effect at a frame boundary). A registration for new
		// reads alone keeps the surfaces: the schedule's older registration draws it (the new reads through their raw indices until then).
		const auto hold = FrameAnchors::HeldNow(kClientId, Anchor::Interface);
		if (hold.held) {
			if (!hold.scheduled || hold.generation != state.surfacesGeneration) {
				state.pendingPrograms.clear();
				a_reason = !hold.scheduled ? "the UI stage's schedule does not hold the client yet" : "a surface made again (drawn once the UI stage's build with it switches in)";
				a_lagged = true;
				return false;
			}
		} else if (FrameAnchors::CanRun(kClientId, Anchor::Interface)) {
			// The anchor's own epoch: the latest registration is in its built graph, or an older one with the same surfaces is.
			state.builtRegistration = state.registrations;
		} else if (state.builtRegistration == 0 || state.builtRegistration < state.surfacesRegistration) {
			state.pendingPrograms.clear();
			a_reason = !FrameAnchors::IsRenderOwner()        ? "a segment off the render owner" :
			           !RenderGraphRuntime::Get().IsActive() ? "the graph path is off (not set up, or failed)" :
			                                                   "a surface made again (drawn once the Interface graph is built with it)";
			a_lagged = FrameAnchors::IsRenderOwner() && RenderGraphRuntime::Get().IsActive();
			return false;
		}

		// One run per kGateEntries commands, in order: the target and the depth-stencil carry what each run drew into the next.
		state.pending = &a_display;
		state.pendingSurface = surface;
		state.pendingDepth = needsDepth;
		a_reason = nullptr;
		for (std::uint32_t first = 0; first < commandCount && !a_reason; first += kGateEntries) {
			state.pendingFirst = first;
			state.pendingCount = std::min(kGateEntries, commandCount - first);
			state.committed = false;
			state.executed = false;
			state.commitReason = nullptr;
			const auto result = FrameAnchors::Run(Anchor::Interface);
			if (!result)
				a_reason = ReasonOf(result.status);
			else if (!state.committed)
				a_reason = state.commitReason ? state.commitReason : "the commit did not run";
			else if (!state.executed)
				a_reason = "the client was not in the Interface epoch";
		}
		state.pending = nullptr;
		state.pendingSurface = nullptr;
		state.pendingFirst = state.pendingCount = 0;
		state.pendingPrograms.clear();
		// New tracked reads register the client again now that the display's runs are done.
		if (state.readsChanged)
			RegisterReads(state);
		if (!a_reason)
			return true;
		Failure(a_reason);
		return false;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/Engine/Scaleform/Passes.h"

namespace RenderGraph::Engine::Scaleform::Passes
{
	bool Reserve() { return false; }
	void Setup() {}
	bool Ready() { return false; }
	bool ScratchSize(std::uint32_t& a_width, std::uint32_t& a_height)
	{
		a_width = a_height = 0;
		return false;
	}
	std::uint32_t ScratchCount() { return 0; }
	bool Submit(Display&, const char*& a_reason, bool& a_lagged)
	{
		a_reason = "built without the render graph";
		a_lagged = false;
		return false;
	}
}

#endif
