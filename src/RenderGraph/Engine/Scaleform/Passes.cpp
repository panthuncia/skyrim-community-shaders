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
		constexpr std::uint32_t kSourceTag = 257;
		constexpr std::uint32_t kMinimumStaging = 256u << 10;
		// The most commands one segment draws on the graph: one run gate entry each. A longer segment is replayed into D3D11.
		constexpr std::uint32_t kGateEntries = 1024;
		// How often (in displays) the surfaces are checked for textures the engine released.
		constexpr std::uint64_t kReleaseCheckDisplays = 1000;

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
		 * @brief A target and depth pair the displays draw onto. It has two passes: one with the target alone, for displays without
		 * masks, and one with the depth-stencil too, for displays with masks (when the depth can be imported).
		 */
		struct Surface
		{
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
			bool drawnSinceCheck = true;               // a display drew onto it since the last release check

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
			bool joined = false;                                  // the first registration reached the Interface graph
			std::array<org::OwnedDescriptorBinding, 4> samplers;  // the renderer's, by wrap * 2 + sample
			std::vector<std::shared_ptr<Surface>> surfaces;
			std::uint64_t displays = 0;
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
		 * @brief One pass of a surface: the display a commit stored for this frame slot, as one render pass.
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
				const std::uint32_t frameSlot = a_recording.FrameSlot();
				const auto frame = a_pass.slots->Take(frameSlot);
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
				const auto& latch = a_pass.gate->Latch();
				const auto latchBuffer = latch.Resource()->GetAPIResource().GetHandle();
				const std::uint64_t latchOffset = latch.Offset(frameSlot);
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
		 * @brief Inside the epoch: the pending display's frame, stored in the slot of the pass that draws it, with its draws' gate
		 * entries opened. Every other pass's slot for this frame slot is emptied first, so nothing older is left there.
		 */
		void Commit()
		{
			auto& state = Path();
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			const std::uint32_t slot = host->CurrentFrameSlot();
			for (const auto& surface : state.surfaces) {
				if (surface->colourSlots)
					surface->colourSlots->Store(slot, nullptr);
				if (surface->depthSlots)
					surface->depthSlots->Store(slot, nullptr);
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
			std::memcpy(frame->staging->Slot(0).data(), display.staging.data(), display.staging.size());

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

			slots->Store(slot, std::move(frame));
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
		 * @brief Registers the client with a pass pair for every surface. The anchor's graph is built with it at the next build point
		 * (RequestJoin), between frames; until then the displays go to D3D11. False, and the path failed, when it is refused.
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
			logger::info("[ORG] Scaleform: registered with {} surfaces (registration {}); the Interface graph is built with them at the next build point",
				drawable, a_state.registrations);
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

		/** @brief Whether anyone but this surface still holds a_texture (the probe's own reference aside). DXVK's views hold none. */
		bool HeldElsewhere(ID3D11Texture2D* a_texture)
		{
			a_texture->AddRef();
			return a_texture->Release() > 1;
		}

		/**
		 * @brief Whether the engine released the surface's textures. A surface a display drew onto since the last check is in use. Its
		 * colour is still the engine's while it is an engine target, the framebuffer's texture or HDR's UI texture, or anyone but this
		 * surface holds it; its depth while it is an engine depth-stencil or held elsewhere.
		 */
		bool Released(const Surface& a_surface)
		{
			if (a_surface.drawnSinceCheck)
				return false;
			auto* colour = a_surface.colourTexture.get();
			DXGI_FORMAT format{};
			const auto& hdr = globals::features::hdrDisplay;
			const bool colourLive = EngineTargetOf(colour) >= 0 || TextureOf(FramebufferRtv(), format).get() == colour ||
			                        (hdr.uiTexture && hdr.uiTexture->resource.get() == colour) || HeldElsewhere(colour);
			auto* depth = a_surface.depthTexture.get();
			const bool depthLive = !depth || EngineDepthOf(depth) >= 0 || HeldElsewhere(depth);
			return !colourLive || !depthLive;
		}

		/**
		 * @brief Drops the surfaces whose textures the engine released (a resize, HDR's textures made again). The client is
		 * registered again without them when one had passes; the textures go once D3D11 and the graph are done with them.
		 */
		void DropReleasedSurfaces(PathState& a_state)
		{
			bool hadPasses = false;
			std::erase_if(a_state.surfaces, [&](const std::shared_ptr<Surface>& a_surface) {
				if (!Released(*a_surface)) {
					a_surface->drawnSinceCheck = false;
					return false;
				}
				logger::info("[ORG] Scaleform: the surface {} ({}x{}) was released by the engine; dropped", a_surface->ownId.empty() ? "an engine target" : a_surface->ownId,
					a_surface->width, a_surface->height);
				hadPasses |= a_surface->Drawable();
				a_surface->Clear();
				RenderGraphRuntime::Get().ReleaseAfterD3D11(a_surface);
				return true;
			});
			if (hadPasses && a_state.setUp && !a_state.failed)
				RegisterClient(a_state);
		}

		/** @brief A surface for a_colour with a_depth (DXVK's), imported; without passes when the target cannot be imported. */
		std::shared_ptr<Surface> MakeSurface(PathState& a_state, ID3D11Texture2D* a_colour, DXGI_FORMAT a_format, ID3D11Texture2D* a_depth)
		{
			auto surface = std::make_shared<Surface>();
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
			logger::info("[ORG] Scaleform: a new surface: {} ({}x{}, DXGI format {}), depth {}", surface->ownId.empty() ? "an engine target" : surface->ownId,
				colourDesc.Width, colourDesc.Height, static_cast<int>(a_format),
				surface->depth ? "an engine depth-stencil" : (a_depth ? "not importable" : "none"));
			if (surface->colour && surface->colourFormat != rhi::Format::Unknown) {
				const std::uint32_t slots = RenderGraphRuntime::Get().Host()->FrameSlots();
				surface->colourSlots = std::make_shared<const Slots>(slots);
				if (surface->depth)
					surface->depthSlots = std::make_shared<const Slots>(slots);
			}
			return surface;
		}

		Surface* Find(const PathState& a_state, ID3D11Texture2D* a_colour, ID3D11Texture2D* a_depth)
		{
			for (const auto& surface : a_state.surfaces)
				if (surface->colourTexture.get() == a_colour && surface->depthTexture.get() == a_depth)
					return surface.get();
			return nullptr;
		}

		/**
		 * @brief The surfaces UI::DrawInterface binds, made before the first registration so it holds their passes: the framebuffer
		 * (the back buffer) and, with HDR, its UI texture, each with kMAIN's depth-stencil.
		 */
		void DeclareKnownSurfaces(PathState& a_state)
		{
			auto* depth = MainDepth();
			const auto add = [&](ID3D11RenderTargetView* a_rtv) {
				DXGI_FORMAT format{};
				const auto texture = TextureOf(a_rtv, format);
				if (texture && format != DXGI_FORMAT_UNKNOWN && !Find(a_state, texture.get(), depth))
					a_state.surfaces.push_back(MakeSurface(a_state, texture.get(), format, depth));
			};
			add(FramebufferRtv());
			const auto& hdr = globals::features::hdrDisplay;
			if (hdr.loaded && hdr.uiTexture && hdr.uiTexture->rtv)
				add(hdr.uiTexture->rtv.get());
		}

		/**
		 * @brief The surface a_display draws onto. A new one is imported and the client registered again with passes for it: null
		 * then, as when it cannot be imported; it is drawn once the Interface graph holds the registration.
		 */
		Surface* SurfaceFor(PathState& a_state, const Display& a_display, bool a_needsDepth, const char*& a_reason)
		{
			if (auto* surface = Find(a_state, a_display.target, a_display.depth)) {
				surface->drawnSinceCheck = true;
				if (!surface->Drawable()) {
					a_reason = "a render target the graph cannot import";
					return nullptr;
				}
				if (surface->colourFormat != rhi::helpers::ToRHI(a_display.targetFormat)) {
					a_reason = "a render target view in another format than its surface's";
					return nullptr;
				}
				if (a_needsDepth && !surface->depthSlots) {
					a_reason = "a mask on a depth-stencil the graph cannot import";
					return nullptr;
				}
				return surface;
			}
			// A new target: the released ones go first, so an old framebuffer after a resize is not kept beside the new one.
			DropReleasedSurfaces(a_state);
			auto surface = MakeSurface(a_state, a_display.target, a_display.targetFormat, a_display.depth);
			const bool drawable = surface->Drawable();
			// Kept without passes too, so the same target is not imported again each display; its displays go to D3D11.
			a_state.surfaces.push_back(std::move(surface));
			if (!drawable) {
				a_reason = "a render target the graph cannot import";
				return nullptr;
			}
			a_reason = RegisterClient(a_state) ? "a new surface (drawn once the Interface graph holds it)" : "the client cannot be registered";
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

		/** @brief Once: the samplers, the gate, the staging pool and the known surfaces, then the first registration. */
		bool SetUp(PathState& a_state)
		{
			for (std::uint32_t i = 0; i < a_state.samplers.size(); ++i) {
				// CreateShaders' samplers: point or linear (sample mode), wrap or clamp (wrap mode), LOD bias -0.5, the top mip only.
				D3D11_SAMPLER_DESC desc{};
				desc.Filter = (i & 1) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
				desc.AddressU = desc.AddressV = desc.AddressW = (i & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
				desc.MipLODBias = -0.5f;
				desc.MaxAnisotropy = 1;
				desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
				desc.MinLOD = 0.0f;
				desc.MaxLOD = 0.0f;
				a_state.samplers[i] = ShaderBindings::CreateSamplerFromD3D11(desc);
				if (!a_state.samplers[i]) {
					logger::error("[ORG] Scaleform: the samplers could not be made; the D3D11 renderer draws");
					return false;
				}
			}
			a_state.gate = RunGate::Create("cs.scaleform.gate", kGateEntries);
			a_state.staging = FrameStaging::Create({ .name = "cs.scaleform.staging",
				.minimumBytes = kMinimumStaging,
				.runsPerFrame = FrameAnchors::RunsPerFrame(Anchor::Interface) });
			if (!a_state.gate || !a_state.staging) {
				logger::error("[ORG] Scaleform: the run gate or the staging pool could not be made; the D3D11 renderer draws");
				return false;
			}
			DeclareKnownSurfaces(a_state);
			return RegisterClient(a_state);
		}

		/** @brief Submit's reason for a run that did not submit. */
		const char* ReasonOf(FrameAnchors::RunStatus a_status)
		{
			switch (a_status) {
			case FrameAnchors::RunStatus::Idle:
				return "the Interface epoch did not run (idle)";
			case FrameAnchors::RunStatus::Refused:
				return "the Interface epoch refused the run";
			case FrameAnchors::RunStatus::RingFull:
				return "the Interface anchor's ring is full this frame";
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

	bool Ready()
	{
		auto& state = Path();
		if (!state.reserved || state.failed || !RenderGraphRuntime::Get().IsActive() || !FrameAnchors::HasEpoch(Anchor::Interface))
			return false;
		if (!state.setUp) {
			state.setUp = true;
			if (!SetUp(state)) {
				state.failed = true;
				return false;
			}
		}
		if (state.joined)
			return true;
		// A registration without passes (no known surface could be imported) has nothing to join: Submit adds the surfaces.
		const bool hasPasses = std::ranges::any_of(state.surfaces, [](const auto& a_surface) { return a_surface->Drawable(); });
		if (hasPasses && !FrameAnchors::RequestJoin(kClientId, Anchor::Interface))
			return false;
		state.joined = true;
		logger::info("[ORG] Scaleform: drawing displays on the render graph (Interface anchor)");
		return true;
	}

	bool Submit(Display& a_display, const char*& a_reason)
	{
		auto& state = Path();
		if (!Ready()) {
			a_reason = "the render graph is not up";
			return false;
		}
		if (++state.displays % kReleaseCheckDisplays == 0)
			DropReleasedSurfaces(state);
		if (state.failed) {
			a_reason = "the client cannot be registered";
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

		// Every pipeline the segment needs is asked for, so they build together; D3D11 draws the segment until all are built.
		state.pendingPrograms.assign(a_display.commands.size(), nullptr);
		bool missing = false;
		bool failed = false;
		for (std::size_t i = 0; i < a_display.commands.size(); ++i) {
			const Command& command = a_display.commands[i];
			if (command.kind != Command::Kind::Draw)
				continue;
			ProgramKey key{ command.state, surface->colourFormat, needsDepth ? surface->depthFormat : rhi::Format::Unknown };
			if (!needsDepth)
				key.state.stencil = Stencil::Off;
			state.pendingPrograms[i] = state.programs.Get(key, [&] { return DescOf(key); });
			if (!state.pendingPrograms[i]) {
				missing = true;
				failed |= state.programs.StatusOf(key) == ProgramStatus::Failed;
			}
		}
		if (missing) {
			state.pendingPrograms.clear();
			a_reason = failed ? "a pipeline cannot be made" : "a pipeline is being built";
			return false;
		}
		if (!FrameAnchors::RequestJoin(kClientId, Anchor::Interface)) {
			state.pendingPrograms.clear();
			a_reason = "the Interface graph is being built with a new surface";
			return false;
		}
		if (!FrameAnchors::CanRun(kClientId, Anchor::Interface)) {
			state.pendingPrograms.clear();
			a_reason = "the Interface anchor's ring is full this frame";
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
		return false;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/Engine/Scaleform/Passes.h"

namespace RenderGraph::Engine::Scaleform::Passes
{
	bool Reserve() { return false; }
	bool Ready() { return false; }
	bool Submit(Display&, const char*& a_reason)
	{
		a_reason = "built without the render graph";
		return false;
	}
}

#endif
