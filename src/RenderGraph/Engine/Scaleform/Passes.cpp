#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/Engine/Scaleform/Passes.h"

#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Globals.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/Engine/Scaleform/Display.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
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
#	include <atomic>
#	include <bit>
#	include <cmath>
#	include <cstring>
#	include <map>
#	include <mutex>
#	include <set>

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
		constexpr std::size_t kSpareStaging = 8;
		// A surface not drawn onto for this many displays is dropped when a new one is made (an old framebuffer after a resize).
		constexpr std::uint64_t kSurfaceIdleDisplays = 600;

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

		/** @brief A pipeline: the draw's state and the surface's formats. */
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
			std::uint32_t vertexCount = 0;
			FillBlock block{};
		};

		/** @brief What one execution of a surface's pass records: immutable once the commit stored it. */
		struct Frame
		{
			std::uint64_t generation = 0;  // the registration it was committed for
			std::shared_ptr<org::LatchBlock> staging;
			std::vector<FrameCommand> commands;
			rhi::Viewport viewport{};
			rhi::ScissorRect scissor{};
			std::uint32_t width = 0, height = 0;
			std::vector<std::shared_ptr<const void>> owners;  // programs, samplers, texture descriptors and views
		};

		/** @brief The frames the commits stored, one per host frame slot; Record takes its slot's. */
		struct Slots
		{
			explicit Slots(std::uint32_t a_count) :
				frames(std::make_unique<std::atomic<std::shared_ptr<const Frame>>[]>(a_count)), count(a_count) {}

			std::shared_ptr<const Frame> Take(std::uint32_t a_slot) const { return frames[a_slot % count].exchange(nullptr, std::memory_order_acq_rel); }
			void Store(std::uint32_t a_slot, std::shared_ptr<const Frame> a_frame) const { frames[a_slot % count].store(std::move(a_frame), std::memory_order_release); }

			std::unique_ptr<std::atomic<std::shared_ptr<const Frame>>[]> frames;
			std::uint32_t count;
		};

		/** @brief A target and depth pair the displays draw onto, with a pass of its own. */
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
			std::shared_ptr<const Slots> slots;  // null for a surface the graph cannot draw onto (no pass)
			std::uint64_t lastUse = 0;           // the display count when it was last drawn onto

			/** @brief Every slot emptied: a pass of an older build that still has this surface draws nothing more. */
			void Clear() const
			{
				if (slots)
					for (std::uint32_t i = 0; i < slots->count; ++i)
						slots->Store(i, nullptr);
			}
		};

		/** @brief Host-visible staging buffers, reused once the frame that used one retired. */
		struct StagingPool
		{
			std::mutex mutex;
			std::vector<std::shared_ptr<org::LatchBlock>> spare;
		};

		struct PathState
		{
			bool reserved = false;
			bool failed = false;
			bool ready = false;
			std::array<org::OwnedDescriptorBinding, 4> samplers;  // the renderer's, by wrap * 2 + sample
			std::vector<std::shared_ptr<Surface>> surfaces;
			std::uint64_t generation = 0;  // bumped by each registration
			std::uint64_t displays = 0;
			std::uint32_t ownImports = 0;
			std::map<ProgramKey, std::shared_ptr<const RasterProgram>> programs;
			std::set<ProgramKey> failedPrograms;
			std::shared_ptr<StagingPool> pool = std::make_shared<StagingPool>();
			// The run in progress (Submit to the end of its anchor run).
			Display* pending = nullptr;
			Surface* pendingSurface = nullptr;
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

		/** @brief A staging buffer of at least a_bytes, given back to the pool when its last owner lets it go. */
		std::shared_ptr<org::LatchBlock> AcquireStaging(const std::shared_ptr<StagingPool>& a_pool, std::uint64_t a_bytes)
		{
			std::shared_ptr<org::LatchBlock> block;
			{
				std::scoped_lock lock(a_pool->mutex);
				auto best = a_pool->spare.end();
				for (auto it = a_pool->spare.begin(); it != a_pool->spare.end(); ++it)
					if ((*it)->Stride() >= a_bytes && (best == a_pool->spare.end() || (*it)->Stride() < (*best)->Stride()))
						best = it;
				if (best != a_pool->spare.end()) {
					block = std::move(*best);
					a_pool->spare.erase(best);
				}
			}
			if (!block) {
				const std::uint64_t size = std::bit_ceil(std::max<std::uint64_t>(a_bytes, kMinimumStaging));
				if (size > 0xFFFFFFFFull)
					return nullptr;
				try {
					block = org::LatchBlock::Create("cs.scaleform.staging", static_cast<std::uint32_t>(size), 1);
				} catch (const std::exception& e) {
					logger::error("[ORG] Scaleform: a staging buffer of {} bytes could not be made: {}", size, e.what());
					return nullptr;
				}
			}
			std::weak_ptr<StagingPool> pool = a_pool;
			auto* raw = block.get();
			return std::shared_ptr<org::LatchBlock>(raw, [pool, block = std::move(block)](org::LatchBlock*) mutable {
				if (auto owner = pool.lock()) {
					std::scoped_lock lock(owner->mutex);
					if (owner->spare.size() < kSpareStaging)
						owner->spare.push_back(std::move(block));
				}
			});
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

		const RasterProgram* ProgramFor(PathState& a_state, const ProgramKey& a_key, std::shared_ptr<const RasterProgram>& a_out)
		{
			if (const auto found = a_state.programs.find(a_key); found != a_state.programs.end()) {
				a_out = found->second;
				return a_out.get();
			}
			if (a_state.failedPrograms.contains(a_key))
				return nullptr;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
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
			auto program = RasterProgram::Load(host->Device(), desc);
			if (!program) {
				a_state.failedPrograms.insert(a_key);
				logger::error("[ORG] Scaleform: pipeline VS {} PS {} filter {} blend {} stencil {} (formats {} / {}) could not be made; displays that need it are drawn by D3D11",
					static_cast<int>(a_key.state.vertexKind), static_cast<int>(a_key.state.pixelKind), static_cast<int>(a_key.state.filter), static_cast<int>(a_key.state.blend),
					static_cast<int>(a_key.state.stencil), static_cast<int>(a_key.colour),
					static_cast<int>(a_key.depth));
				return nullptr;
			}
			a_state.programs.emplace(a_key, program);
			a_out = std::move(program);
			return a_out.get();
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
			std::uint64_t generation = 0;
		};

		/** @brief One surface's pass: the display a commit stored for this frame slot, as one render pass. */
		class DisplayPass final : public org::TypedRenderGraphPass<DisplayPass, PassFrame, PassBindings>
		{
		public:
			DisplayPass(std::shared_ptr<const Surface> a_surface, std::uint64_t a_generation) :
				surface(std::move(a_surface)), generation(a_generation) {}

			PassBindings Declare(org::PassBuilder& a_builder)
			{
				PassBindings bindings{ a_builder.RenderTarget(surface->colour).View() };
				if (surface->depth) {
					bindings.depth = a_builder.DepthReadWrite(surface->depth).View();
					bindings.hasDepth = true;
				}
				return bindings;
			}

			PassFrame Prepare(const PassBindings&, const org::PassPrepareContext&) const { return { surface->slots, generation }; }

			static void Record(const PassBindings& a_bindings, const PassFrame& a_pass, org::PassRecordContext& a_recording)
			{
				if (!a_pass.slots)
					return;
				const auto frame = a_pass.slots->Take(a_recording.FrameSlot());
				// A frame committed for a later registration is not this pass's: its display went to D3D11.
				if (!frame || frame->generation != a_pass.generation || !frame->staging || frame->commands.empty())
					return;
				a_recording.Retain(frame);
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
				const RasterProgram* bound = nullptr;
				std::uint32_t reference = 0;
				for (const auto& command : frame->commands) {
					if (command.kind == Command::Kind::ClearStencil) {
						// Inside the pass this is vkCmdClearAttachments over the render area: the whole target, as D3D11 clears it.
						if (a_bindings.hasDepth)
							commands.ClearDepthStencilView(depth.dsv, false, true, 0.0f, 0);
						continue;
					}
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
					commands.Draw(command.vertexCount, 1, 0, 0);
				}
				commands.EndPass();
			}

		private:
			std::shared_ptr<const Surface> surface;
			std::uint64_t generation = 0;
		};

		/** @brief Inside the epoch: the pending display's frame, stored in its surface's slot (and nothing in the others'). */
		void Commit()
		{
			auto& state = Path();
			auto* host = RenderGraphRuntime::Get().Host();
			if (!state.pending || !state.pendingSurface || !host)
				return;
			const Display& display = *state.pending;
			const Surface& surface = *state.pendingSurface;
			auto frame = std::make_shared<Frame>();
			frame->generation = state.generation;

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

			frame->staging = AcquireStaging(state.pool, std::max<std::uint64_t>(display.staging.size(), 4));
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
				out.vertexCount = command.vertexCount;
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

			const std::uint32_t slot = host->CurrentFrameSlot();
			for (const auto& other : state.surfaces)
				if (other->slots)
					other->slots->Store(slot, other.get() == &surface ? frame : nullptr);
			state.committed = true;
		}

		FrameAnchors::ClientDesc MakeClient(const PathState& a_state)
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			const std::uint64_t generation = a_state.generation;
			for (std::size_t i = 0; i < a_state.surfaces.size(); ++i) {
				std::shared_ptr<const Surface> surface = a_state.surfaces[i];
				if (!surface->slots)
					continue;
				client.passes.push_back({ .name = fmt::format("cs.scaleform.draw.{}", i),
					.anchor = FrameAnchors::Anchor::Interface,
					.order = 100 + static_cast<std::int32_t>(i),
					.kind = FrameAnchors::PassKind::Render,
					.make = [surface, generation] { return std::make_shared<DisplayPass>(surface, generation); } });
			}
			client.registerResources = [](org::RenderGraph& a_graph) {
				for (const auto& surface : Path().surfaces)
					if (!surface->ownId.empty() && surface->colour)
						a_graph.RegisterResource(org::ResourceIdentifier(surface->ownId), surface->colour);
			};
			client.commit = [](const FrameAnchors::Point&) { Commit(); };
			client.active = [] { return Path().pending != nullptr; };
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

		/** @brief Drops the surfaces not drawn onto lately: their textures are released once D3D11 and the graph are done with them. */
		void DropIdleSurfaces(PathState& a_state)
		{
			std::erase_if(a_state.surfaces, [&](const std::shared_ptr<Surface>& a_surface) {
				if (a_state.displays - a_surface->lastUse < kSurfaceIdleDisplays)
					return false;
				logger::info("[ORG] Scaleform: the surface {}x{} is no longer drawn onto; dropped", a_surface->width, a_surface->height);
				a_surface->Clear();
				RenderGraphRuntime::Get().ReleaseAfterD3D11(a_surface);
				return true;
			});
		}

		/**
		 * @brief The surface a_display draws onto. A new one is imported and the client registered again with a pass for it: null
		 * then (the client is in the anchor's graph from the next run), as when it cannot be imported.
		 */
		Surface* SurfaceFor(PathState& a_state, const Display& a_display, bool a_needsDepth, const char*& a_reason)
		{
			for (const auto& surface : a_state.surfaces)
				if (surface->colourTexture.get() == a_display.target && surface->depthTexture.get() == a_display.depth) {
					surface->lastUse = a_state.displays;
					if (!surface->slots) {
						a_reason = "a render target the graph cannot import";
						return nullptr;
					}
					if (a_needsDepth && !surface->depth) {
						a_reason = "a mask on a depth-stencil the graph cannot import";
						return nullptr;
					}
					return surface.get();
				}

			auto surface = std::make_shared<Surface>();
			surface->colourTexture.copy_from(a_display.target);
			surface->depthTexture.copy_from(a_display.depth);
			surface->lastUse = a_state.displays;
			D3D11_TEXTURE2D_DESC colourDesc{};
			a_display.target->GetDesc(&colourDesc);
			surface->width = colourDesc.Width;
			surface->height = colourDesc.Height;
			surface->colourFormat = rhi::helpers::ToRHI(a_display.targetFormat);
			if (const auto target = EngineTargetOf(a_display.target); target >= 0) {
				surface->colour = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(target));
			} else {
				surface->ownId = fmt::format("cs.scaleform.target.{}", a_state.ownImports++);
				surface->colour = ImportTarget(a_display.target, a_display.targetFormat, surface->ownId);
			}
			if (a_display.depth) {
				if (const auto depth = EngineDepthOf(a_display.depth); depth >= 0)
					surface->depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(depth));
				if (surface->depth) {
					D3D11_TEXTURE2D_DESC depthDesc{};
					a_display.depth->GetDesc(&depthDesc);
					surface->width = std::min(surface->width, depthDesc.Width);
					surface->height = std::min(surface->height, depthDesc.Height);
					surface->depthFormat = surface->depth->GetFormat();
				}
			}
			logger::info("[ORG] Scaleform: a new surface: {} ({}x{}, DXGI format {}), depth {}", surface->ownId.empty() ? "an engine target" : surface->ownId,
				colourDesc.Width, colourDesc.Height, static_cast<int>(a_display.targetFormat),
				surface->depth ? "an engine depth-stencil" : (a_display.depth ? "not importable" : "none"));
			DropIdleSurfaces(a_state);
			if (!surface->colour || surface->colourFormat == rhi::Format::Unknown) {
				// Kept without a pass, so the same target is not imported again each display; its displays go to D3D11.
				a_state.surfaces.push_back(std::move(surface));
				a_reason = "a render target the graph cannot import";
				return nullptr;
			}
			surface->slots = std::make_shared<const Slots>(RenderGraphRuntime::Get().Host()->FrameSlots());
			a_state.surfaces.push_back(std::move(surface));
			++a_state.generation;
			if (!FrameAnchors::Register(MakeClient(a_state))) {
				a_state.failed = true;
				logger::error("[ORG] Scaleform: the Interface anchor's client could not be registered; D3D11 draws every display");
				a_reason = "the client cannot be registered";
				return nullptr;
			}
			a_reason = "a new surface (drawn from the next display)";
			return nullptr;
		}
	}

	bool Reserve()
	{
		auto& state = Path();
		state.reserved = FrameAnchors::Reserve(FrameAnchors::Anchor::Interface);
		if (!state.reserved)
			logger::warn("[ORG] Scaleform: CS_ORG_SCALEFORM=1, but the Interface anchor could not be reserved; the D3D11 renderer draws");
		return state.reserved;
	}

	bool Ready()
	{
		auto& state = Path();
		if (!state.reserved || state.failed || !RenderGraphRuntime::Get().IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Interface))
			return false;
		if (state.ready)
			return true;
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
				logger::error("[ORG] Scaleform: the samplers could not be made; the D3D11 renderer draws");
				return false;
			}
		}
		state.ready = true;
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
		++state.displays;
		if (a_display.commands.empty())
			return true;  // nothing drawn, as the D3D11 renderer would draw nothing
		// Without a depth-stencil bound the D3D11 masks do nothing either; with one, the graph must have it.
		const bool needsDepth = a_display.depth && std::ranges::any_of(a_display.commands, [](const Command& a_command) {
			return a_command.kind == Command::Kind::ClearStencil || a_command.state.stencil != Stencil::Off;
		});
		Surface* surface = SurfaceFor(state, a_display, needsDepth, a_reason);
		if (!surface)
			return false;

		state.pendingPrograms.assign(a_display.commands.size(), nullptr);
		const rhi::Format depthFormat = surface->depth ? surface->depthFormat : rhi::Format::Unknown;
		for (std::size_t i = 0; i < a_display.commands.size(); ++i) {
			const Command& command = a_display.commands[i];
			if (command.kind != Command::Kind::Draw)
				continue;
			ProgramKey key{ command.state, surface->colourFormat, depthFormat };
			if (!surface->depth)
				key.state.stencil = Stencil::Off;
			if (!ProgramFor(state, key, state.pendingPrograms[i])) {
				a_reason = "a pipeline cannot be made";
				state.pendingPrograms.clear();
				return false;
			}
		}

		state.pending = &a_display;
		state.pendingSurface = surface;
		state.committed = false;
		state.executed = false;
		state.commitReason = nullptr;
		const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::Interface);
		state.pending = nullptr;
		state.pendingSurface = nullptr;
		state.pendingPrograms.clear();
		if (!ran)
			a_reason = "the Interface epoch did not run";
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
