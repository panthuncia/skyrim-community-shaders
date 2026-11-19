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
#	include "RenderGraph/Frame/Transients.h"
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
		// GpuTextures' diagnostic source tag for Scaleform's views.
		constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::Scaleform);
		constexpr std::uint32_t kMinimumStaging = 256u << 10;
		// One run gate entry per command. A longer segment is drawn in several runs.
		constexpr std::uint32_t kGateEntries = 1024;
		// The UI stage's fixed instances. Interface runs before a menu's scene use 8 to 15 and runs after it use 0 to 7.
		// All are declared every frame and unused ones stay closed. More runs at one step than that is a mismatch.
		constexpr std::uint32_t kInstances = 16;
		// Scratch images for GFx render targets. A blur needs its source and two intermediates, plus one for a nested target.
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

		// Must match the push block in Scaleform/Filter.hlsli. Padded to the fill block's size so every program has one layout.
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

		/** @brief A pipeline's draw state and pass formats. Depth is Unknown for a pass without masks. */
		struct ProgramKey
		{
			DrawState state;
			rhi::Format colour = rhi::Format::Unknown;
			rhi::Format depth = rhi::Format::Unknown;

			auto operator<=>(const ProgramKey&) const = default;
		};

		struct FrameCommand
		{
			Command::Kind kind = Command::Kind::Draw;
			const RasterProgram* program = nullptr;
			std::uint32_t stencilRef = 0;
			bool lines = false;
			bool filter = false;  // the block is a FilterBlock
			// Per stage, the declared read whose view replaces the block's raw index. Null for an exempt texture read untracked.
			std::array<const void*, 2> reads{};
			FillBlock block{};
		};

		/** @brief A tracked texture the displays read, which the passes declare. Native mode only. */
		struct Read
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;  // keeps the view's identity while it is declared
			FrameAnchors::Texture texture;
			std::uint64_t lastCommit = 0;
		};

		using Reads = std::vector<FrameAnchors::Texture>;

		// A declared read unused for this many commits is dropped at the next registration.
		constexpr std::uint64_t kReadIdleCommits = 600;

		/** @brief What one run of a surface's pass records. Immutable once stored. */
		struct Frame
		{
			std::shared_ptr<org::LatchBlock> staging;
			std::vector<FrameCommand> commands;
			rhi::Viewport viewport{};
			rhi::ScissorRect scissor{};
			std::uint32_t width = 0, height = 0;
			// Cleared to zero first, since PushRenderTarget clears a pushed target whole.
			bool clear = false;
			rhi::Format format = rhi::Format::Unknown;
			std::vector<std::shared_ptr<const void>> owners;
		};

		using Slots = RunSlots<Frame>;

		/**
		 * @brief The targets UI::DrawInterface binds for the displays, the only ones the graph draws onto.
		 * The main and loading menus draw into kMAIN, so it is a surface from setup too.
		 */
		enum class Role : std::uint8_t
		{
			Framebuffer,  // kFRAMEBUFFER's RTV, or HDR's saved one
			HdrUi,        // while HDR redirects the interface into it
			Main,         // the main menu's and loading menu's movies
			// The book menu's pages, drawn into a GFx render texture that is one of these engine targets.
			Book0,
			Book1,
			Book2,
			Book3,
		};
		constexpr std::array<Role, 7> kRoles{ Role::Framebuffer, Role::HdrUi, Role::Main, Role::Book0, Role::Book1, Role::Book2, Role::Book3 };

		bool IsBook(Role a_role) { return a_role >= Role::Book0 && a_role <= Role::Book3; }

		/**
		 * @brief A target the displays draw onto. It has a pass without depth and, when depth can be imported, one with kMAIN's
		 * depth-stencil for masks. Remade only when the role's texture is remade.
		 */
		struct Surface
		{
			Role role = Role::Framebuffer;
			// A scratch image's index, or -1 for a role.
			std::int32_t scratch = -1;
			winrt::com_ptr<ID3D11Texture2D> colourTexture;  // the surface's identity
			winrt::com_ptr<ID3D11Texture2D> depthTexture;
			FrameAnchors::Texture colour;
			FrameAnchors::Texture depth;
			std::string ownId;  // set when this client registers colour itself, as for HDR's twin
			rhi::Format colourFormat = rhi::Format::Unknown;
			rhi::Format depthFormat = rhi::Format::Unknown;
			std::uint32_t width = 0, height = 0;
			// One per instance. Empty when the graph cannot draw onto the surface.
			std::vector<std::shared_ptr<const Slots>> colourSlots;
			std::vector<std::shared_ptr<const Slots>> depthSlots;  // empty without an importable depth-stencil

			bool Drawable() const { return !colourSlots.empty(); }
			bool Masks() const { return !depthSlots.empty(); }

			/** @brief Empties every slot, so an older build's pass on this surface draws nothing more. */
			void Clear() const
			{
				for (const auto& slots : colourSlots)
					slots->Clear();
				for (const auto& slots : depthSlots)
					slots->Clear();
			}
		};

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
			std::array<org::OwnedDescriptorBinding, 4> samplers;  // by wrap * 2 + sample
			std::vector<std::shared_ptr<Surface>> surfaces;       // one per role that has a texture, then the scratch images
			std::uint32_t scratchWidth = 0, scratchHeight = 0;    // the largest role surface's size
			std::shared_ptr<const void> scratchPlacement;        // null when they have memory of their own
			// Tracked textures the displays read. A new one makes the client register again after the run.
			std::vector<Read> reads;
			bool readsChanged = false;
			std::uint64_t commits = 0;
			std::uint32_t registrations = 0;
			// A registration for new reads alone keeps the surfaces, since the graph built before it still draws them.
			std::uint32_t surfacesRegistration = 0;
			std::uint32_t builtRegistration = 0;
			std::uint32_t largestSegment = 0;
			// The schedule draws a display only once it holds a registration with the current surfaces.
			std::uint64_t surfacesGeneration = 0;
			std::uint32_t peakTaps = 0;  // logged at each new peak
			ProgramCache<RasterProgram, ProgramKey> programs{ "Scaleform" };
			std::set<ProgramKey> failedPrograms;
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<FrameStaging> staging;
			// The run in progress, from Submit to the end of its anchor run.
			Display* pending = nullptr;
			std::uint32_t pendingFirst = 0;
			std::uint32_t pendingCount = 0;
			Surface* pendingSurface = nullptr;
			bool pendingDepth = false;
			std::vector<std::shared_ptr<const RasterProgram>> pendingPrograms;  // null for a clear
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
		 * @brief The renderer's blend states. Indices 9 to 15 repeat 1 to 7 with source factor One for premultiplied filter output,
		 * read from SE 0x1417c2588.
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
			default:  // normal
				set(F::SrcAlpha, F::InvSrcAlpha, O::Add, F::One, F::InvSrcAlpha, O::Add);
				break;
			}
			return blend;
		}

		/** @brief The renderer's depth-stencil state. Depth is never tested. */
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
			default:  // test
				face.passOp = rhi::StencilOp::Keep;
				face.func = rhi::CompareOp::Equal;
				break;
			}
			depth.front = face;
			depth.back = face;
			return depth;
		}

		/** @brief What a pipeline is built from. Render thread. */
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
				// Vertex shader indices 7 to 11 are the 3D kinds of 1 to 5.
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
		 * @brief Every draw state the capture can make, so pipelines are built ahead. The set must stay in step with what Renderer.cpp
		 * emits. 3D draws are not enumerated and are built at their first draw.
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
					// A solid fill in Multiply or Darken keeps kind 1, since its colour is moved towards white instead.
					const std::vector<std::uint8_t> blends = pixel == 4 || pixel == 1 ? std::vector<std::uint8_t>{ 1, 2, 3, 4, 5, 6, 7 } :
					                                         multiply(pixel) ? std::vector<std::uint8_t>{ 4, 6, 7 } :
					                                                           std::vector<std::uint8_t>{ 1, 2, 3, 5, 7 };
					add({ .vertexKind = kinds.vertex, .pixelKind = pixel }, blends);
				}
			add({ .vertexKind = 1, .pixelKind = 1, .lines = 1 }, { 1, 2, 3, 4, 5, 6, 7 });
			add({ .vertexKind = 0, .pixelKind = 0, .filter = 23 }, { 9, 10, 11, 13, 15, 7 });
			add({ .vertexKind = 0, .pixelKind = 0, .filter = 24 }, { 12, 14, 7 });
			// The blur's last pass uses the filters' blend for the mode. Earlier passes use the normal one.
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
			std::vector<std::pair<const void*, org::DeclaredViewToken>> reads;
		};

		struct PassFrame
		{
			std::shared_ptr<const Slots> slots;
			std::shared_ptr<const RunGate> gate;
			std::uint32_t gateBase = 0;
		};

		/**
		 * @brief One pass of a surface, drawing the display a commit stored for this run. Each draw takes its vertex count from the
		 * run gate, which only this client's commit opens. So a run without that commit draws nothing.
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
				// Declared so the graph orders the reads after the context's writes and moves them to shader-read.
				if (reads)
					for (const auto& read : *reads)
						if (read && read.Key() != surface->colour.Key() && (!withDepth || read.Key() != surface->depth.Key()))
							bindings.reads.emplace_back(read.Key(), a_builder.ShaderResource(read).View());
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { FrameAnchors::CommitReadRevision(atExecution, a_out); }

			PassFrame Prepare(const PassBindings&, const org::PassPrepareContext&) const { return { slots, gate, gateBase }; }

			// A surface no display drew on this run skips, and its barriers fold into the next pass.
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
				const auto region = a_pass.gate->Latch().At(run);
				const auto latchBuffer = region.buffer->GetAPIResource().GetHandle();
				const std::uint64_t latchOffset = region.offset;
				const RasterProgram* bound = nullptr;
				std::uint32_t reference = 0;
				bool lines = false;
				for (std::uint32_t i = 0; i < frame->commands.size(); ++i) {
					const auto& command = frame->commands[i];
					if (command.kind == Command::Kind::ClearStencil) {
						// This clears the whole target, as D3D11 does.
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
					// A read this build does not declare yet keeps its raw index until the build with it is attached.
					std::uint32_t words[sizeof(FillBlock) / 4];
					std::memcpy(words, &command.block, sizeof(words));
					const std::size_t first = (command.filter ? offsetof(FilterBlock, source) : offsetof(FillBlock, textures)) / 4;
					for (std::uint32_t stage = 0; stage < 2; ++stage) {
						if (!command.reads[stage])
							continue;
						const auto declared = std::ranges::find(a_bindings.reads, command.reads[stage], &std::pair<const void*, org::DeclaredViewToken>::first);
						if (declared != a_bindings.reads.end())
							words[first + stage] = a_recording.Index(declared->second);
					}
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kConstantWords, words);
					// The vertex count is zero unless this run's commit opened the gate entry.
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
			bool atExecution = false;
			std::uint32_t gateBase = 0;
		};

		/**
		 * @brief The declared read for a display texture, or null when it is read untracked. A new tracked texture is added and the
		 * client registers again after the run, so the next build declares it.
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

		const Surface* ScratchSurface(const PathState& a_state, std::int32_t a_scratch)
		{
			if (a_scratch < 0)
				return nullptr;
			for (const auto& surface : a_state.surfaces)
				if (surface->scratch == a_scratch)
					return surface.get();
			return nullptr;
		}

		/** @brief Stores the pending display's frame for this run and opens its gate entries. Other passes' entries are emptied first. */
		void Commit()
		{
			auto& state = Path();
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			const org::RunRef run = host->CurrentRun();
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
				// Until a build declares the read, the raw index is the fallback, and the validator counts it as declared.
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
				// Keep the view too until the GPU work retires, since EndDisplay's queued releases may drop GFx's.
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
					// A scratch image is read through the view every pass declares.
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
			// D3D11 clips to the viewport, since the renderer's rasterizer state has no scissor.
			const float left = std::clamp(display.viewport[0], 0.0f, static_cast<float>(surface.width));
			const float top = std::clamp(display.viewport[1], 0.0f, static_cast<float>(surface.height));
			const float right = std::clamp(display.viewport[0] + display.viewport[2], left, static_cast<float>(surface.width));
			const float bottom = std::clamp(display.viewport[1] + display.viewport[3], top, static_cast<float>(surface.height));
			frame->scissor = { static_cast<std::int32_t>(std::floor(left)), static_cast<std::int32_t>(std::floor(top)),
				static_cast<std::uint32_t>(std::ceil(right) - std::floor(left)), static_cast<std::uint32_t>(std::ceil(bottom) - std::floor(top)) };

			slots->Store(run, std::move(frame));
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
			// Scratch images are declared by every instance but the first. Nothing earlier in the frame wrote them, and a draw into one
			// clears it first, since the frame arena may hold other transients there. The anchor's own epoch declares them in every pass.
			auto withScratch = std::make_shared<Reads>(*declared);
			for (const auto& surface : a_state.surfaces)
				if (surface->scratch >= 0 && surface->Drawable())
					withScratch->push_back(surface->colour);
			std::shared_ptr<const Reads> reads = std::move(withScratch);
			std::shared_ptr<const Reads> firstReads = std::move(declared);
			const auto firstInstance = FrameAnchors::FirstGraphInstance(Anchor::Interface);
			for (std::size_t i = 0; i < a_state.surfaces.size(); ++i) {
				std::shared_ptr<const Surface> surface = a_state.surfaces[i];
				if (!surface->Drawable())
					continue;
				const auto add = [&](const std::vector<std::shared_ptr<const Slots>>& a_slots, bool a_withDepth, const char* a_suffix, std::int32_t a_order) {
					FrameAnchors::PassDesc pass{ .name = fmt::format("cs.scaleform.draw.{}{}", i, a_suffix),
						.anchor = Anchor::Interface,
						.order = a_order,
						.kind = FrameAnchors::PassKind::Render,
						// Recorded at execution from the slot the commit stores, without preparing a ticket again.
						.atExecution = true };
					// One pass per instance, each with its own slots and gate entries.
					pass.makeInstance = [surface, a_slots, gate, a_withDepth, reads, firstReads, firstInstance](std::uint32_t a_instance) -> std::shared_ptr<org::RenderPass> {
						if (a_instance >= a_slots.size())
							return nullptr;
						return std::make_shared<DisplayPass>(surface, a_slots[a_instance], gate, a_withDepth, firstInstance == a_instance ? firstReads : reads,
							FrameAnchors::RecordsAtExecution(Anchor::Interface), a_instance);
					};
					client.passes.push_back(std::move(pass));
				};
				// Scratch passes come first in an instance, as GFx draws a render target before the segment that reads it.
				const std::int32_t order = surface->scratch >= 0 ? 2 * surface->scratch : 100 + 2 * static_cast<std::int32_t>(i);
				add(surface->colourSlots, false, "", order);
				if (surface->Masks())
					add(surface->depthSlots, true, ".masks", order + 1);
			}
			// All instances are declared because how many displays the engine draws is decided late.
			client.maxInstances = kInstances;
			client.instances = [] { return kInstances; };
			client.instanceMajor = true;
			// A registration for new reads keeps the surfaces, slots and gate, so the older registration still draws the displays.
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
			// New reads register only once the display's last run is done, never between its runs.
			client.executed = [](const FrameAnchors::Point&) {
				auto& state = Path();
				if (state.pending)
					state.executed = true;
			};
			return client;
		}

		/** @brief Registers the client with a pass pair for every surface. False, failing the path, when refused. */
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
		 * @brief Registers again with the current reads after a run found a new tracked texture. The graph rebuilds off the render
		 * thread, and until then the old graph draws new reads through their raw indices.
		 */
		void RegisterReads(PathState& a_state)
		{
			a_state.readsChanged = false;
			std::erase_if(a_state.reads, [&](const Read& a_read) { return a_read.lastCommit + kReadIdleCommits < a_state.commits; });
			logger::info("[ORG] Scaleform: {} tracked textures read by the displays; the Interface graph is built again to declare them", a_state.reads.size());
			RegisterClient(a_state);
		}

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

		/** @brief The refusal reason for a display onto another target, naming the engine target when it is one. */
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

		/** @brief The back buffer's RTV, which is HDR's saved one while HDR points kFRAMEBUFFER at its UI texture. */
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

		/** @brief The role's texture now and its view format. Null when the role has none. */
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

		ID3D11Texture2D* MainDepth()
		{
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return nullptr;
			const auto& depth = renderer->GetDepthStencilData().depthStencils[static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)];
			return depth.texture ? Tripwire::Unwrap(depth.texture) : nullptr;
		}

		/** @brief The depth-stencil a role masks with. kBOOK_TEXT's for the book's pages, kMAIN's for the rest. */
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

		/** @brief Imports a non-engine target through the anchors, so it is one graph resource whoever imports it. */
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

		/** @brief The graph texture of a role's target. A remade texture becomes that import's next version, never a new id. */
		FrameAnchors::Texture ColourOf(Role a_role, ID3D11Texture2D* a_colour, DXGI_FORMAT a_format, std::string& a_ownId)
		{
			a_ownId.clear();
			if (const auto target = EngineTargetOf(a_colour); target >= 0)
				return FrameAnchors::EngineTarget(static_cast<std::uint32_t>(target));
			if (auto owned = ORGHDRDisplay::Get().GraphOf(a_colour)) {
				// The graph-owned HDR UI twin, which the composite reads.
				a_ownId = "cs.scaleform.hdr-ui";
				return owned;
			}
			return ImportTarget(a_colour, a_format, fmt::format("cs.scaleform.target.{}", static_cast<int>(a_role)));
		}

		FrameAnchors::Texture DepthTextureOf(ID3D11Texture2D* a_depth)
		{
			if (!a_depth)
				return {};
			const auto depth = EngineDepthOf(a_depth);
			return depth >= 0 ? FrameAnchors::EngineDepth(static_cast<std::uint32_t>(depth)) : FrameAnchors::Texture{};
		}

		/** @brief Makes the role's surface. It has no passes when the target cannot be imported. */
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
		 * @brief Queues every pipeline the surface may need on the build thread, so they build while the game loads.
		 * One needed before its build finished is built on the render thread and logged.
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

		Surface* Find(const PathState& a_state, ID3D11Texture2D* a_colour)
		{
			for (const auto& surface : a_state.surfaces)
				if (surface->scratch < 0 && a_colour && surface->colourTexture.get() == a_colour)
					return surface.get();
			return nullptr;
		}

		/** @brief Replaces the role's surface with one made from its current texture. False when the role has none. */
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
				// An older build's pass that still holds it draws nothing more.
				a_old->Clear();
				RenderGraphRuntime::Get().ReleaseAfterD3D11(a_old);
				return true;
			});
			a_state.surfaces.push_back(std::move(surface));
			return true;
		}

		/**
		 * @brief Keeps a role's surface when its remade texture is the same graph texture, so nothing is rebuilt.
		 * Null when the graph texture changed, and the caller remakes the surface.
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
		 * @brief The surface a_display draws onto. A role whose texture was remade gets a new surface and a re-registration,
		 * and the graph rebuilds at the next build point. Null with a_reason otherwise.
		 */
		Surface* SurfaceFor(PathState& a_state, const Display& a_display, bool a_needsDepth, const char*& a_reason)
		{
			if (a_display.scratch != -1) {
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

		/** @brief Logs each new power of two of the largest segment. */
		void NoteSegment(PathState& a_state, std::uint32_t a_commands)
		{
			if (a_commands <= a_state.largestSegment)
				return;
			const bool log = a_commands >= 64 && std::bit_floor(a_commands) > std::bit_floor(std::max(a_state.largestSegment, 1u));
			a_state.largestSegment = a_commands;
			if (log)
				logger::info("[ORG] Scaleform: the largest segment so far has {} commands (drawn in runs of at most {})", a_commands, kGateEntries);
		}

		/** @brief Names a draw's failed pipeline key once. The draw is left out and the rest of its segment draws. */
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

		/** @brief Logs a graph path error once per reason. Not a readiness state. */
		void Failure(const char* a_reason)
		{
			static std::vector<const char*> logged;
			if (std::ranges::find(logged, a_reason) != logged.end())
				return;
			logged.push_back(a_reason);
			logger::error("[ORG] Scaleform: a segment did not draw on the graph ({}); it is left out", a_reason);
		}

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
		/** @brief Makes the scratch images that GFx's pushed render targets draw into, at the largest role surface's size. */
		void MakeScratch(PathState& a_state)
		{
			for (const auto& surface : a_state.surfaces)
				if (surface->Drawable()) {
					a_state.scratchWidth = std::max(a_state.scratchWidth, surface->width);
					a_state.scratchHeight = std::max(a_state.scratchHeight, surface->height);
				}
			if (!a_state.scratchWidth || !a_state.scratchHeight)
				return;
			// The scratch images share bytes with other stages' transients. A draw into one clears it, and nothing reads one before that.
			std::vector<std::shared_ptr<org::PixelBuffer>> textures;
			for (std::uint32_t i = 0; i < kScratchTargets; ++i) {
				org::TextureDescription desc{};
				desc.imageDimensions = { { a_state.scratchWidth, a_state.scratchHeight, 0, 0 } };
				desc.channels = 4;
				desc.format = kScratchFormat;
				desc.hasRTV = true;
				desc.rtvFormat = kScratchFormat;
				desc.hasSRV = true;
				desc.srvFormat = kScratchFormat;
				auto texture = org::PixelBuffer::CreateSharedUnmaterialized(desc);
				if (!texture) {
					logger::warn("[ORG] Scaleform: scratch image {} could not be made; GFx's render targets beyond it are not drawn", i);
					break;
				}
				texture->SetName(fmt::format("cs.scaleform.scratch.{}", i));
				textures.push_back(std::move(texture));
			}
			// In guest mode or with the arena off they get memory of their own.
			try {
				a_state.scratchPlacement = RenderGraph::Frame::Transients::Place("Scaleform's scratch images", textures);
			} catch (const std::exception& e) {
				stl::report_and_fail(fmt::format("Community Shaders: Scaleform's scratch images could not be placed in the frame arena: {}", e.what()));
			}
			if (!a_state.scratchPlacement)
				for (const auto& texture : textures)
					texture->Materialize();
			for (std::uint32_t i = 0; i < textures.size(); ++i) {
				auto& texture = textures[i];
				auto surface = std::make_shared<Surface>();
				surface->scratch = static_cast<std::int32_t>(i);
				surface->ownId = texture->GetName();
				surface->colour = FrameAnchors::Texture(std::static_pointer_cast<org::Resource>(texture));
				surface->colourFormat = kScratchFormat;
				surface->width = a_state.scratchWidth;
				surface->height = a_state.scratchHeight;
				surface->colourSlots = MakeSlots();
				if (i == 0)
					QueuePrograms(a_state, *surface);
				a_state.surfaces.push_back(std::move(surface));
			}
			logger::info("[ORG] Scaleform: {} scratch images of {}x{} for GFx's render targets ({})", kScratchTargets, a_state.scratchWidth, a_state.scratchHeight,
				a_state.scratchPlacement ? "in the frame arena, span derived from the graphs that use them" : "memory of their own");
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
			// CreateShaders' samplers use LOD bias -0.5 and the top mip only.
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
		// Each instance has its own kGateEntries.
		state.gate = RunGate::Create("cs.scaleform.gate", kGateEntries * kInstances);
		state.staging = FrameStaging::Create({ .name = "cs.scaleform.staging",
			.minimumBytes = kMinimumStaging,
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
			return true;  // the D3D11 renderer would draw nothing too
		const auto commandCount = static_cast<std::uint32_t>(a_display.commands.size());
		NoteSegment(state, commandCount);
		// Masks inside a pushed render target are not drawn, since scratch images have no stencil surface yet.
		const bool needsDepth = a_display.scratch == -1 && a_display.depth && std::ranges::any_of(a_display.commands, [](const Command& a_command) {
			return a_command.kind == Command::Kind::ClearStencil || a_command.state.stencil != Stencil::Off;
		});
		Surface* surface = SurfaceFor(state, a_display, needsDepth, a_reason);
		if (!surface)
			return false;

		// A pipeline still building is built here, never left to D3D11. A key that failed leaves its draw out.
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
		// The UI stage's schedule must hold a registration with the current surfaces. Until a rebuild switches in at a frame
		// boundary the display lags.
		const auto hold = FrameAnchors::HeldNow(kClientId, Anchor::Interface);
		if (hold.held) {
			if (!hold.scheduled || hold.generation != state.surfacesGeneration) {
				state.pendingPrograms.clear();
				a_reason = !hold.scheduled ? "the UI stage's schedule does not hold the client yet" : "a surface made again (drawn once the UI stage's build with it switches in)";
				a_lagged = true;
				return false;
			}
		} else if (FrameAnchors::Scheduled(kClientId, Anchor::Interface)) {
			state.builtRegistration = state.registrations;
		} else if (state.builtRegistration == 0 || state.builtRegistration < state.surfacesRegistration) {
			state.pendingPrograms.clear();
			a_reason = !FrameAnchors::IsRenderOwner()        ? "a segment off the render owner" :
			           !RenderGraphRuntime::Get().IsActive() ? "the graph path is off (not set up, or failed)" :
			                                                   "a surface made again (drawn once the Interface graph is built with it)";
			a_lagged = FrameAnchors::IsRenderOwner() && RenderGraphRuntime::Get().IsActive();
			return false;
		}

		// One run per kGateEntries commands. The target and depth carry each run's result into the next.
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
