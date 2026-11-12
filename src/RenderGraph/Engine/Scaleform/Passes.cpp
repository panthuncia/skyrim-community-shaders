#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/Engine/Scaleform/Passes.h"

#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/HDRDisplay.h"
#	include "Globals.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/Engine/Scaleform/Display.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/ProgramCache.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <bit>
#	include <cmath>
#	include <cstring>
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
		// The most commands one segment draws on the graph: one run gate entry each. A longer segment is replayed into D3D11.
		constexpr std::uint32_t kGateEntries = 1024;

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
			FillBlock block{};
		};

		/** @brief What one run of a surface's pass records: immutable once the commit stored it. */
		struct Frame
		{
			std::shared_ptr<org::LatchBlock> staging;
			std::vector<FrameCommand> commands;
			rhi::Viewport viewport{};
			rhi::ScissorRect scissor{};
			std::uint32_t width = 0, height = 0;
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
		};
		constexpr std::array<Role, 3> kRoles{ Role::Framebuffer, Role::HdrUi, Role::Main };

		/**
		 * @brief A target the displays draw onto, with kMAIN's depth-stencil for masks. It has two passes: one with the target alone,
		 * for displays without masks, and one with the depth-stencil too, for displays with masks (when the depth can be imported).
		 * One surface per role, made at setup; made again only when the role's texture was made again (a resize, HDR's textures).
		 */
		struct Surface
		{
			Role role = Role::Framebuffer;
			winrt::com_ptr<ID3D11Texture2D> colourTexture;  // DXVK's: the surface's identity
			winrt::com_ptr<ID3D11Texture2D> depthTexture;
			std::shared_ptr<org::ExternalTextureResource> colour;
			std::shared_ptr<org::ExternalTextureResource> depth;
			std::string ownId;  // set when this client registers colour (not an engine target)
			rhi::Format colourFormat = rhi::Format::Unknown;
			rhi::Format depthFormat = rhi::Format::Unknown;
			std::uint32_t width = 0, height = 0;
			std::shared_ptr<const Slots> colourSlots;  // null for a surface the graph cannot draw onto (no passes)
			std::shared_ptr<const Slots> depthSlots;   // null without an importable depth-stencil

			bool Drawable() const { return colourSlots != nullptr; }

			/** @brief Every slot emptied: a pass of an older build that still has this surface draws nothing more. */
			void Clear() const
			{
				if (colourSlots)
					colourSlots->Clear();
				if (depthSlots)
					depthSlots->Clear();
			}
		};

		struct PathState
		{
			bool reserved = false;
			bool failed = false;
			bool setUp = false;
			std::array<org::OwnedDescriptorBinding, 4> samplers;  // the renderer's, by wrap * 2 + sample
			std::vector<std::shared_ptr<Surface>> surfaces;       // one per role that has a texture
			std::uint32_t ownImports = 0;
			std::uint32_t registrations = 0;
			std::uint32_t largestSegment = 0;
			ProgramCache<RasterProgram, ProgramKey> programs{ "Scaleform" };
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<FrameStaging> staging;
			// The run in progress (Submit to the end of its anchor run).
			Display* pending = nullptr;
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
				desc.defines = { { L"VS_KIND", std::to_wstring(a_key.state.vertexKind) }, { L"PS_KIND", std::to_wstring(a_key.state.pixelKind) } };
			}
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
		 *   4 with 11, 13, 14, 15, 17, 18 (XY16iCF32 gouraud), 5 with 11, 14, 15, 18 (gouraud type 3); filters 23 and 24;
		 * - blends: the Multiply and Darken kinds (3, 6, 15-18, filter 24) only with the multiply and darken states (4, 6), the other
		 *   kinds with 1, 2, 3, 5, the glyph kind (4) with any, the filters from the filters' array (8 + the index); every kind with 7
		 *   (a mask being drawn, the Alpha and Erase modes);
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
					const std::vector<std::uint8_t> blends = pixel == 4 ? std::vector<std::uint8_t>{ 1, 2, 3, 4, 5, 6, 7 } :
					                                         multiply(pixel) ? std::vector<std::uint8_t>{ 4, 6, 7 } :
					                                                           std::vector<std::uint8_t>{ 1, 2, 3, 5, 7 };
					add({ .vertexKind = kinds.vertex, .pixelKind = pixel }, blends);
				}
			add({ .vertexKind = 0, .pixelKind = 0, .filter = 23 }, { 9, 10, 11, 13, 15, 7 });
			add({ .vertexKind = 0, .pixelKind = 0, .filter = 24 }, { 12, 14, 7 });
			return states;
		}

		struct PassBindings
		{
			org::DeclaredViewToken colour;
			org::DeclaredViewToken depth;
			bool hasDepth = false;
		};

		struct PassFrame
		{
			std::shared_ptr<const Slots> slots;
			std::shared_ptr<const RunGate> gate;
		};

		/**
		 * @brief One pass of a surface: the display a commit stored for this run, as one render pass.
		 *
		 * Each draw reads its vertex count from its command's entry of the client's run gate, which the anchors close at the start of
		 * every run and only this client's commit opens: a run without that commit (a frame left in a slot by a run that failed, a run
		 * kept by another client) draws nothing. The pass has no Revision, so ORG never keeps its recording: it is recorded at every
		 * run from the frame of that run, which names the display's pipelines, and its push blocks are that run's.
		 */
		class DisplayPass final : public org::TypedRenderGraphPass<DisplayPass, PassFrame, PassBindings>
		{
		public:
			DisplayPass(std::shared_ptr<const Surface> a_surface, std::shared_ptr<const Slots> a_slots, std::shared_ptr<const RunGate> a_gate, bool a_withDepth) :
				surface(std::move(a_surface)), slots(std::move(a_slots)), gate(std::move(a_gate)), withDepth(a_withDepth) {}

			PassBindings Declare(org::PassBuilder& a_builder)
			{
				PassBindings bindings{ a_builder.RenderTarget(surface->colour).View() };
				if (withDepth) {
					bindings.depth = a_builder.DepthReadWrite(surface->depth).View();
					bindings.hasDepth = true;
				}
				return bindings;
			}

			PassFrame Prepare(const PassBindings&, const org::PassPrepareContext&) const { return { slots, gate }; }

			static void Record(const PassBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots || !a_pass.gate)
					return;
				const org::RunRef run = a_recording.Run();
				const auto frame = a_pass.slots->Take(run);
				if (!frame || !frame->staging || frame->commands.empty())
					return;
				a_recording.Retain(frame);
				a_recording.Retain(a_pass.gate);
				auto& commands = a_recording.Commands();

				rhi::ColorAttachment colour{};
				colour.rtv = a_recording.View(a_bindings.colour);
				colour.resource = a_recording.Resolve(a_bindings.colour).GetHandle();
				colour.loadOp = rhi::LoadOp::Load;
				colour.storeOp = rhi::StoreOp::Store;
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
					if (command.stencilRef != reference) {
						commands.SetStencilRef(command.stencilRef);
						reference = command.stencilRef;
					}
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&command.block));
					// The vertex count is the gate entry's: zero unless this run's commit opened it.
					commands.ExecuteIndirect(command.program->drawSignature->GetHandle(), latchBuffer, latchOffset + a_pass.gate->Offset(i), {}, 0, 1);
				}
				commands.EndPass();
			}

		private:
			std::shared_ptr<const Surface> surface;
			std::shared_ptr<const Slots> slots;
			std::shared_ptr<const RunGate> gate;
			bool withDepth = false;
		};

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
			for (const auto& surface : state.surfaces) {
				if (surface->colourSlots)
					surface->colourSlots->Store(run, nullptr);
				if (surface->depthSlots)
					surface->depthSlots->Store(run, nullptr);
			}
			if (!state.pending || !state.pendingSurface)
				return;
			const Display& display = *state.pending;
			const Surface& surface = *state.pendingSurface;
			const auto& slots = state.pendingDepth ? surface.depthSlots : surface.colourSlots;
			if (!slots)
				return;
			auto frame = std::make_shared<Frame>();

			std::vector<std::uint32_t> textures(display.textures.size(), 0);
			for (std::size_t i = 0; i < display.textures.size(); ++i) {
				auto* view = display.textures[i];
				auto binding = DCLF::GpuTextures::Get().ResolveBinding(view, kSourceTag);
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

			frame->commands.reserve(display.commands.size());
			for (std::size_t i = 0; i < display.commands.size(); ++i) {
				const Command& command = display.commands[i];
				FrameCommand& out = frame->commands.emplace_back();
				out.kind = command.kind;
				if (command.kind != Command::Kind::Draw)
					continue;
				out.program = state.pendingPrograms[i].get();
				out.stencilRef = command.stencilRef;
				const auto textureOf = [&](std::uint32_t a_stage) {
					const std::int32_t texture = command.textures[a_stage];
					return texture >= 0 ? textures[static_cast<std::size_t>(texture)] : 0u;
				};
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
			frame->viewport = { display.viewport[0], display.viewport[1], display.viewport[2], display.viewport[3], 0.0f, 1.0f };
			// D3D11 clips to the viewport (the renderer's rasterizer state has no scissor).
			const float left = std::clamp(display.viewport[0], 0.0f, static_cast<float>(surface.width));
			const float top = std::clamp(display.viewport[1], 0.0f, static_cast<float>(surface.height));
			const float right = std::clamp(display.viewport[0] + display.viewport[2], left, static_cast<float>(surface.width));
			const float bottom = std::clamp(display.viewport[1] + display.viewport[3], top, static_cast<float>(surface.height));
			frame->scissor = { static_cast<std::int32_t>(std::floor(left)), static_cast<std::int32_t>(std::floor(top)),
				static_cast<std::uint32_t>(std::ceil(right) - std::floor(left)), static_cast<std::uint32_t>(std::ceil(bottom) - std::floor(top)) };

			slots->Store(run, std::move(frame));
			for (std::uint32_t i = 0; i < display.commands.size(); ++i)
				if (display.commands[i].kind == Command::Kind::Draw)
					state.gate->OpenDraw(i, display.commands[i].vertexCount);
			state.committed = true;
		}

		FrameAnchors::ClientDesc MakeClient(const PathState& a_state)
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			std::shared_ptr<const RunGate> gate = a_state.gate;
			for (std::size_t i = 0; i < a_state.surfaces.size(); ++i) {
				std::shared_ptr<const Surface> surface = a_state.surfaces[i];
				if (!surface->Drawable())
					continue;
				const auto add = [&](const std::shared_ptr<const Slots>& a_slots, bool a_withDepth, const char* a_suffix, std::int32_t a_order) {
					client.passes.push_back({ .name = fmt::format("cs.scaleform.draw.{}{}", i, a_suffix),
						.anchor = Anchor::Interface,
						.order = a_order,
						.kind = FrameAnchors::PassKind::Render,
						.make = [surface, a_slots, gate, a_withDepth] { return std::make_shared<DisplayPass>(surface, a_slots, gate, a_withDepth); } });
				};
				add(surface->colourSlots, false, "", 100 + 2 * static_cast<std::int32_t>(i));
				if (surface->depthSlots)
					add(surface->depthSlots, true, ".masks", 101 + 2 * static_cast<std::int32_t>(i));
			}
			client.registerResources = [](org::RenderGraph& a_graph) {
				for (const auto& surface : Path().surfaces)
					if (!surface->ownId.empty() && surface->colour)
						a_graph.RegisterResource(org::ResourceIdentifier(surface->ownId), surface->colour);
			};
			client.commit = [](const FrameAnchors::Point&) { Commit(); };
			client.active = [] { return Path().pending != nullptr; };
			client.gates = { a_state.gate };
			client.executed = [](const FrameAnchors::Point&) {
				if (Path().pending)
					Path().executed = true;
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
					names[i] = fmt::format("a render target other than the framebuffer, HDR's UI texture and kMAIN (engine target {})", i);
				return names;
			}();
			const auto target = EngineTargetOf(a_texture);
			return target >= 0 ? s_engine[static_cast<std::size_t>(target)].c_str() :
			                     "a render target other than the framebuffer, HDR's UI texture and kMAIN (not an engine target)";
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
			if (a_role == Role::Main) {
				auto* renderer = globals::game::renderer;
				a_format = DXGI_FORMAT_UNKNOWN;
				return renderer ? TextureOf(renderer->GetRuntimeData().renderTargets[static_cast<std::uint32_t>(RE::RENDER_TARGET::kMAIN)].RTV, a_format) : nullptr;
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

		/** @brief a_texture (DXVK's), not an engine target, imported as a render target of this client's: HDR's UI texture. */
		std::shared_ptr<org::ExternalTextureResource> ImportTarget(ID3D11Texture2D* a_texture, DXGI_FORMAT a_format, const std::string& a_id)
		{
			auto& runtime = RenderGraphRuntime::Get();
			DxvkOrgInteropResourceInfo info{};
			if (!runtime.Host() || !runtime.DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return nullptr;
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_format);
			desc.channels = 4;
			desc.hasRTV = true;
			desc.rtvFormat = desc.format;
			desc.imageDimensions = { { info.image.extent.width, info.image.extent.height, 0, 0 } };
			return RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, desc, a_id.c_str());
		}

		/** @brief The role's surface for a_colour with a_depth (DXVK's), imported; without passes when the target cannot be imported. */
		std::shared_ptr<Surface> MakeSurface(PathState& a_state, Role a_role, ID3D11Texture2D* a_colour, DXGI_FORMAT a_format, ID3D11Texture2D* a_depth)
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
			if (const auto target = EngineTargetOf(a_colour); target >= 0) {
				surface->colour = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(target));
			} else {
				surface->ownId = fmt::format("cs.scaleform.target.{}", a_state.ownImports++);
				surface->colour = ImportTarget(a_colour, a_format, surface->ownId);
			}
			if (a_depth) {
				if (const auto depth = EngineDepthOf(a_depth); depth >= 0)
					surface->depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(depth));
				if (surface->depth) {
					D3D11_TEXTURE2D_DESC depthDesc{};
					a_depth->GetDesc(&depthDesc);
					surface->width = std::min(surface->width, depthDesc.Width);
					surface->height = std::min(surface->height, depthDesc.Height);
					surface->depthFormat = surface->depth->GetFormat();
				}
			}
			logger::info("[ORG] Scaleform: the {} surface: {} ({}x{}, DXGI format {}), depth {}", RoleName(a_role),
				surface->ownId.empty() ? "an engine target" : surface->ownId, colourDesc.Width, colourDesc.Height, static_cast<int>(a_format),
				surface->depth ? "an engine depth-stencil" : (a_depth ? "not importable" : "none"));
			if (surface->colour && surface->colourFormat != rhi::Format::Unknown) {
				surface->colourSlots = std::make_shared<const Slots>();
				if (surface->depth)
					surface->depthSlots = std::make_shared<const Slots>();
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
			if (a_surface.depthSlots)
				queue(true);
			logger::info("[ORG] Scaleform: {} pipelines queued for the {} surface", queued, RoleName(a_surface.role));
		}

		/** @brief The surface whose colour is a_colour (DXVK's), or null. */
		Surface* Find(const PathState& a_state, ID3D11Texture2D* a_colour)
		{
			for (const auto& surface : a_state.surfaces)
				if (surface->colourTexture.get() == a_colour)
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
			auto surface = MakeSurface(a_state, a_role, texture.get(), format, MainDepth());
			QueuePrograms(a_state, *surface);
			std::erase_if(a_state.surfaces, [&](const std::shared_ptr<Surface>& a_old) {
				if (a_old->role != a_role)
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
		 * @brief The surface a_display draws onto. Only the framebuffer's and HDR's UI texture's, declared at setup, are drawn on the
		 * graph. When one of those roles' textures was made again (a resize, HDR's textures made again), its surface is made again
		 * and the client registered again: the Interface graph is built with it at the next build point, between frames (a rebuild
		 * trigger, logged). Null, with a_reason, otherwise.
		 */
		Surface* SurfaceFor(PathState& a_state, const Display& a_display, bool a_needsDepth, const char*& a_reason)
		{
			if (auto* surface = Find(a_state, a_display.target)) {
				if (!surface->Drawable()) {
					a_reason = "a render target the graph cannot import";
					return nullptr;
				}
				if (surface->colourFormat != rhi::helpers::ToRHI(a_display.targetFormat)) {
					a_reason = "a render target view in another format than its surface's";
					return nullptr;
				}
				if (a_needsDepth && (!surface->depthSlots || a_display.depth != surface->depthTexture.get())) {
					a_reason = "a mask on a depth-stencil other than kMAIN's, or one the graph cannot import";
					return nullptr;
				}
				return surface;
			}
			for (const auto role : kRoles) {
				DXGI_FORMAT format{};
				if (RoleTexture(role, format).get() != a_display.target)
					continue;
				logger::info("[ORG] Scaleform: the {} texture was made again (a resize or an HDR change); the Interface graph is built again with it",
					RoleName(role));
				if (DeclareSurface(a_state, role))
					RegisterClient(a_state);
				a_reason = "a surface made again (drawn once the Interface graph is built with it)";
				return nullptr;
			}
			a_reason = OtherTargetReason(a_display.target);
			return nullptr;
		}

		/** @brief Logs each new power of two of the largest segment, to size kGateEntries from. */
		void NoteSegment(PathState& a_state, std::uint32_t a_commands)
		{
			if (a_commands <= a_state.largestSegment)
				return;
			const bool log = a_commands >= 64 && std::bit_floor(a_commands) > std::bit_floor(std::max(a_state.largestSegment, 1u));
			a_state.largestSegment = a_commands;
			if (log)
				logger::info("[ORG] Scaleform: the largest segment so far has {} commands (the Interface gate holds {})", a_commands, kGateEntries);
		}

		/** @brief A run that did not draw: an error of the graph path, logged once per reason. Not a readiness state. */
		void Failure(const char* a_reason)
		{
			static std::vector<const char*> logged;
			if (std::ranges::find(logged, a_reason) != logged.end())
				return;
			logged.push_back(a_reason);
			logger::error("[ORG] Scaleform: a segment did not draw on the graph ({}); it was replayed into D3D11", a_reason);
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
		state.gate = RunGate::Create("cs.scaleform.gate", kGateEntries);
		state.staging = FrameStaging::Create({ .name = "cs.scaleform.staging",
			.minimumBytes = kMinimumStaging,
			// About the Interface anchor's runs a frame: a display, and one more per D3D11 part.
			.runsPerFrame = 6 });
		if (!state.gate || !state.staging) {
			state.failed = true;
			logger::error("[ORG] Scaleform: the run gate or the staging pool could not be made; the D3D11 renderer draws");
			return;
		}
		for (const auto role : kRoles)
			(void)DeclareSurface(state, role);
		if (std::ranges::none_of(state.surfaces, [](const auto& a_surface) { return a_surface->Drawable(); })) {
			state.failed = true;
			logger::error("[ORG] Scaleform: none of the framebuffer, HDR's UI texture and kMAIN can be imported; the D3D11 renderer draws");
			return;
		}
		if (!RegisterClient(state))
			return;
		state.setUp = true;
		logger::info("[ORG] Scaleform: on (CS_ORG_SCALEFORM); the client joins the Interface graph at setup's build point");
	}

	bool Ready()
	{
		const auto& state = Path();
		return state.setUp && !state.failed && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(Anchor::Interface);
	}

	bool Submit(Display& a_display, const char*& a_reason)
	{
		auto& state = Path();
		if (!Ready()) {
			a_reason = "the graph path is off (not set up, or failed)";
			return false;
		}
		if (a_display.commands.empty())
			return true;  // nothing drawn, as the D3D11 renderer would draw nothing
		const auto commandCount = static_cast<std::uint32_t>(a_display.commands.size());
		NoteSegment(state, commandCount);
		if (commandCount > kGateEntries) {
			a_reason = "a segment with more commands than the Interface gate holds";
			return false;
		}
		// Without a depth-stencil bound the D3D11 masks do nothing either; with one, the graph must have it.
		const bool needsDepth = a_display.depth && std::ranges::any_of(a_display.commands, [](const Command& a_command) {
			return a_command.kind == Command::Kind::ClearStencil || a_command.state.stencil != Stencil::Off;
		});
		Surface* surface = SurfaceFor(state, a_display, needsDepth, a_reason);
		if (!surface)
			return false;

		// Every pipeline was queued at setup; one the build thread has not finished yet is built here, never left to D3D11.
		state.pendingPrograms.assign(a_display.commands.size(), nullptr);
		for (std::size_t i = 0; i < a_display.commands.size(); ++i) {
			const Command& command = a_display.commands[i];
			if (command.kind != Command::Kind::Draw)
				continue;
			ProgramKey key{ command.state, surface->colourFormat, needsDepth ? surface->depthFormat : rhi::Format::Unknown };
			if (!needsDepth)
				key.state.stencil = Stencil::Off;
			state.pendingPrograms[i] = state.programs.Require(key, [&] { return DescOf(key); });
			if (!state.pendingPrograms[i]) {
				state.pendingPrograms.clear();
				a_reason = "a pipeline that cannot be made";
				return false;
			}
		}
		if (!FrameAnchors::CanRun(kClientId, Anchor::Interface)) {
			state.pendingPrograms.clear();
			// Setup's build point put the client in the graph: it is outside it only after a surface was made again, until the next build.
			a_reason = FrameAnchors::InBuiltGraph(kClientId, Anchor::Interface) ? "the Interface anchor's ring is full this frame" :
			                                                                       "a surface made again (drawn once the Interface graph is built with it)";
			return false;
		}

		state.pending = &a_display;
		state.pendingSurface = surface;
		state.pendingDepth = needsDepth;
		state.committed = false;
		state.executed = false;
		state.commitReason = nullptr;
		const auto result = FrameAnchors::Run(Anchor::Interface);
		state.pending = nullptr;
		state.pendingSurface = nullptr;
		state.pendingPrograms.clear();
		if (!result)
			a_reason = ReasonOf(result.status);
		else if (!state.committed)
			a_reason = state.commitReason ? state.commitReason : "the commit did not run";
		else if (!state.executed)
			a_reason = "the client was not in the Interface epoch";
		else
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
	bool Submit(Display&, const char*& a_reason)
	{
		a_reason = "built without the render graph";
		return false;
	}
}

#endif
