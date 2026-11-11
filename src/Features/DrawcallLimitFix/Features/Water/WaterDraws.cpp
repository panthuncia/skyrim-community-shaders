// The water family's render graph side: a client per view at the EngineDraws anchor, whose pass draws a batch's latched draws in the
// engine's order with CS's Water.hlsl built for ORG, with the engine's stencil state and reference per draw.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/StateConversion.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "ShaderCache.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <optional>

namespace DCLF::Features::Water::WaterDraws
{
	namespace
	{
		constexpr const char* kShader = "Water.hlsl";
		// The most draws a batch latches (WaterCapture's kMaxLatched); a batch's draws past it are the engine's.
		constexpr std::uint32_t kMaxDraws = 64;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawLatch));
		// Runs whose imports and leases are kept: the frames the graph may run behind, with a margin.
		constexpr std::uint32_t kKeptFrames = RenderGraphRuntime::kGameFramesInFlight + 2;
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief Water.hlsl's push block (WATER_ORG_RESOURCES), in list order. */
		struct WaterBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw, strictLights;
			std::array<ShaderBindings::Handle, kHeadersBeforeShadow> headers;                                // t17 to t77
			ShaderBindings::Latch directionalShadow;                                                         // t98
			std::array<ShaderBindings::Handle, kHeaderRegisters.size() - kHeadersBeforeShadow> lateHeaders;  // t102, t103
		};
		static_assert(sizeof(WaterBlock) == 112, "Water.hlsl's push block is 112 bytes");

		const std::array<std::int32_t, 3>& TargetListOf(const Shape& a_shape) { return a_shape.view == View::Mask ? kMaskTargets : kSurfaceTargets; }

		/** @brief A client's state, which its pass shares. */
		struct Client
		{
			Shape shape;
			std::string id;
			std::shared_ptr<org::LatchBlock> latch;
			// The targets as imported at the registration (FrameAnchors keeps each while the engine's texture stays the same).
			std::array<std::shared_ptr<org::ExternalTextureResource>, 3> colours;
			std::shared_ptr<org::ExternalTextureResource> depth;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			std::atomic<bool> prepared{ false };
			bool pending = false;
			bool shapeLogged = false;
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			std::array<org::DeclaredViewToken, 3> colours{};
			std::array<bool, 3> hasColours{};
			org::DeclaredViewToken depth{};
			std::array<org::DeclaredViewToken, kHeaderRegisters.size()> headers{};
			std::array<bool, kHeaderRegisters.size()> hasHeaders{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const org::LatchBlock> latch;
		};

		/** @brief A run's draws, recorded in the engine's order: a render pass and a draw each (RasterProgram::Draw). */
		class WaterPass final : public org::TypedRenderGraphPass<WaterPass, Frame, Bindings>
		{
		public:
			explicit WaterPass(std::shared_ptr<Client> a_client) :
				client(std::move(a_client)) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				const auto& c = *client;
				for (std::size_t i = 0; i < c.colours.size(); ++i) {
					if (!c.colours[i])
						continue;
					bindings.colours[i] = a_builder.RenderTarget(c.colours[i]).View();
					bindings.hasColours[i] = true;
				}
				bindings.depth = a_builder.DepthReadWrite(c.depth).View();
				for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
					if (c.shape.headerIds[h].empty())
						continue;
					bindings.headers[h] = a_builder.ShaderResource(org::ResourceIdentifier(c.shape.headerIds[h])).View();
					bindings.hasHeaders[h] = true;
				}
				return bindings;
			}

			// The run's draws (their programs, targets, stencil references, counts and viewports) and the unpublished headers' indices.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const
			{
				client->prepared.store(true, std::memory_order_release);
				return { std::atomic_load(&client->run), client->latch };
			}

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.run || !a_frame.latch)
					return;
				const auto& run = *a_frame.run;
				const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
				std::array<ShaderBindings::Handle, kHeaderRegisters.size()> headers{};
				for (std::size_t h = 0; h < headers.size(); ++h)
					headers[h] = a_bindings.hasHeaders[h] ? a_recording.Index(a_bindings.headers[h]) : run.headerIndices[h];
				const std::uint32_t slot = a_recording.FrameSlot();
				for (std::uint32_t i = 0; i < run.draws.size(); ++i) {
					const auto& draw = run.draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					const std::uint32_t offset = i * kDrawStride;
					WaterBlock block{};
					block.shared = shared;
					block.draw = ShaderBindings::LatchOf(*a_frame.latch, slot, offset);
					block.strictLights = ShaderBindings::LatchOf(*a_frame.latch, slot, offset + static_cast<std::uint32_t>(offsetof(DrawLatch, strictLights)));
					block.directionalShadow = ShaderBindings::LatchOf(*a_frame.latch, slot, offset + static_cast<std::uint32_t>(offsetof(DrawLatch, directionalShadow)));
					std::copy_n(headers.begin(), kHeadersBeforeShadow, block.headers.begin());
					std::copy(headers.begin() + kHeadersBeforeShadow, headers.end(), block.lateHeaders.begin());

					std::array<RasterProgram::Colour, 2> colours{};
					const std::uint32_t colourCount = Water::TargetsOf(run.view);
					bool targetsOk = true;
					for (std::uint32_t k = 0; k < colourCount; ++k) {
						const auto target = draw.colours[k];
						targetsOk = targetsOk && target < a_bindings.colours.size() && a_bindings.hasColours[target];
						if (targetsOk)
							colours[k] = { .view = &a_bindings.colours[target] };
					}
					if (!targetsOk)
						continue;
					const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(colours.data(), colourCount),
												  .depth = &depth,
												  .viewport = { .x = draw.viewport[0], .y = draw.viewport[1], .width = draw.viewport[2], .height = draw.viewport[3], .minDepth = draw.depthRange[0], .maxDepth = draw.depthRange[1] },
												  .constants = ShaderBindings::Data(block),
												  .vertexCount = draw.indexCount,
												  .debugName = run.view == View::Mask ? "WaterMask" : "Water",
												  .stencilReference = draw.stencilRef });
				}
			}

		private:
			std::shared_ptr<Client> client;
		};

		struct ProgramEntry
		{
			PipelineKey key;
			std::shared_ptr<const RasterProgram> program;
		};

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			std::array<bool, kViews> refused{};  // the view's client could not be made: its batches stay the engine's
			std::vector<ProgramEntry> programs;
			std::vector<PipelineKey> failedPrograms;
			bool reserved = false;
			bool failed = false;
			std::uint64_t runs = 0, draws = 0;
			std::deque<std::pair<std::uint32_t, std::vector<std::shared_ptr<const void>>>> kept;  // (game frame, owners)
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		std::uint32_t GameFrame()
		{
			static REL::Relocation<std::uint32_t*> counter{ REL::Offset(Engine::Address::kFrameCounter) };
			return *counter;
		}

		/** @brief Stops the family for good: every batch is the engine's again. */
		void Fail(const char* a_why)
		{
			auto& state = GetState();
			if (!state.failed)
				logger::error("[DCLF] water: back to the engine's draws for the session: {}", a_why);
			state.failed = true;
			for (auto& client : state.clients)
				if (client)
					FrameAnchors::Unregister(client->id);
		}

		/** @brief The client of a_shape's view, registered with the anchor for a_shape. Null, logged, when a target cannot be imported. */
		std::shared_ptr<Client> MakeClient(const Shape& a_shape)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			auto client = std::make_shared<Client>();
			client->shape = a_shape;
			client->id = a_shape.view == View::Mask ? "cs.water.mask" : "cs.water.surface";
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kMaxDraws * kDrawStride, host->FrameSlots());
			const auto& targets = TargetListOf(a_shape);
			for (std::size_t i = 0; i < targets.size(); ++i) {
				if (targets[i] < 0)
					continue;
				client->colours[i] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(targets[i]));
				if (!client->colours[i]) {
					logger::warn("[DCLF] water: the engine target {} cannot be imported; the {} stays the engine's", targets[i], client->id);
					return nullptr;
				}
			}
			client->depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
			if (!client->depth) {
				logger::warn("[DCLF] water: the main depth cannot be imported; the {} stays the engine's", client->id);
				return nullptr;
			}

			FrameAnchors::ClientDesc desc{};
			desc.id = client->id;
			desc.sharedConstants = true;
			desc.passes.push_back({ .name = client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = a_shape.view == View::Mask ? 0 : 1,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<WaterPass>(client); } });
			desc.commit = [client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!run || !host)
					return;
				const std::uint32_t slot = host->CurrentFrameSlot();
				for (std::uint32_t i = 0; i < run->draws.size(); ++i)
					client->latch->WriteValue(slot, i * kDrawStride, run->draws[i].latch);
			};
			desc.active = [client] { return client->pending; };
			desc.onFailed = [] { Fail("the render graph failed with the water's passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] water: the {} client could not be registered; the water stays the engine's", client->id);
				return nullptr;
			}
			std::string published;
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h)
				if (!a_shape.headerIds[h].empty())
					published += fmt::format("{}t{} {}", published.empty() ? "" : ", ", kHeaderRegisters[h], a_shape.headerIds[h]);
			logger::info("[DCLF] water: the {} client registered (published inputs: {})", client->id, published.empty() ? "none" : published);
			return client;
		}

		/** @brief What a run's recording depends on: everything of its draws but the latched values, and the unpublished headers. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(static_cast<std::uint64_t>(a_run.view));
			mix(a_run.draws.size());
			for (const auto index : a_run.headerIndices)
				mix(index);
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				mix((std::uint64_t(draw.colours[0]) << 8) | draw.colours[1]);
				mix((std::uint64_t(draw.stencilRef) << 32) | draw.indexCount);
				for (const auto v : draw.viewport)
					mix(v);
				std::uint32_t range[2];
				std::memcpy(range, draw.depthRange, sizeof(range));
				mix((std::uint64_t(range[0]) << 32) | range[1]);
			}
			return hash;
		}
	}

	bool Available()
	{
		const auto& state = GetState();
		return state.reserved && !state.failed && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::EngineDraws);
	}

	void Reserve()
	{
		auto& state = GetState();
		state.reserved = FrameAnchors::Reserve(FrameAnchors::Anchor::EngineDraws);
		if (!state.reserved)
			logger::warn("[DCLF] water: the EngineDraws anchor could not be reserved; the water stays the engine's");
	}

	bool Ready(const Shape& a_shape)
	{
		auto& state = GetState();
		if (state.failed || a_shape.view >= View::Count)
			return false;
		const auto index = static_cast<std::size_t>(a_shape.view);
		auto& client = state.clients[index];
		if (client) {
			if (client->shape == a_shape)
				return client->prepared.load(std::memory_order_acquire);
			// No client is registered again during play (a rebuild on the frame's path): the batch stays the engine's.
			if (!client->shapeLogged)
				logger::warn("[DCLF] water: the {} batch's published inputs changed since its client registered; those batches stay the engine's", client->id);
			client->shapeLogged = true;
			return false;
		}
		if (state.refused[index])
			return false;
		client = MakeClient(a_shape);
		state.refused[index] = !client;
		return false;
	}

	std::shared_ptr<const void> Program(const PipelineKey& a_key, const void* a_shader)
	{
		auto& state = GetState();
		for (const auto& entry : state.programs)
			if (entry.key == a_key)
				return entry.program;
		for (const auto& failed : state.failedPrograms)
			if (failed == a_key)
				return nullptr;
		auto* host = RenderGraphRuntime::Get().Host();
		const auto* shader = static_cast<const RE::BSShader*>(a_shader);
		auto* blend = static_cast<ID3D11BlendState*>(const_cast<void*>(a_key.blend));
		auto* depthStencil = static_cast<ID3D11DepthStencilState*>(const_cast<void*>(a_key.depthStencil));
		auto* raster = static_cast<ID3D11RasterizerState*>(const_cast<void*>(a_key.raster));
		if (!host || !shader || !blend || !raster || !depthStencil) {
			state.failedPrograms.push_back(a_key);
			return nullptr;
		}

		RasterProgram::Desc desc{};
		desc.pixelSource = kShader;
		desc.vertexSource = kShader;
		desc.constantWords = ShaderBindings::Words<WaterBlock>();
		// The vertex descriptor's defines serve both stages: the vertex stage reads Wading, which State strips from the pixel descriptor
		// (the pixel stage reads it at run time, as Water.hlsl's pixel build undefines it). Each stage's VSHADER or PSHADER is
		// RasterProgram's (commonDefines).
		for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(*shader, SIE::ShaderClass::Vertex, a_key.vertexDescriptor)) {
			if (name == "VSHADER" || name == "PSHADER")
				continue;
			desc.defines.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		}

		// The engine's state objects for the draw, as SetDirtyStates would bind them. GetDesc answers on a tripwire handle too.
		D3D11_BLEND_DESC blendDesc{};
		blend->GetDesc(&blendDesc);
		rhi::BlendState blendState{};
		std::string error;
		if (!::RenderGraph::Engine::Pipeline::BlendFromD3D11(blendDesc, blendState, error) || ::RenderGraph::Engine::Pipeline::UsesDualSource(blendDesc)) {
			logger::warn("[DCLF] water: a draw's blend state has no ORG form ({}); its technique stays the engine's", error);
			state.failedPrograms.push_back(a_key);
			return nullptr;
		}
		for (std::size_t i = 0; i < a_key.colourFormats.size() && a_key.colourFormats[i]; ++i)
			desc.targets.push_back({ .format = static_cast<rhi::Format>(a_key.colourFormats[i]), .blend = blendState.attachments[i] });
		desc.alphaToCoverage = blendState.alphaToCoverage;
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthStencil->GetDesc(&depthDesc);
		desc.depth = ::RenderGraph::Engine::Pipeline::DepthStencilFromD3D11(depthDesc);
		desc.depthFormat = static_cast<rhi::Format>(a_key.depthFormat);
		D3D11_RASTERIZER_DESC rasterDesc{};
		raster->GetDesc(&rasterDesc);
		desc.raster = ::RenderGraph::Engine::Pipeline::RasterFromD3D11(rasterDesc);

		auto program = RasterProgram::Load(host->Device(), desc);
		const auto technique = (a_key.pixelDescriptor >> 11) & 0xF;
		if (!program) {
			logger::warn("[DCLF] water: the program for technique {} (descriptors {:#x}/{:#x}, {}) could not be built; it stays the engine's", technique,
				a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.view == View::Mask ? "mask" : "surface");
			state.failedPrograms.push_back(a_key);
			return nullptr;
		}
		logger::info("[DCLF] water: built the program for technique {} (descriptors {:#x}/{:#x}, {}, stencil {})", technique, a_key.vertexDescriptor,
			a_key.pixelDescriptor, a_key.view == View::Mask ? "mask" : "surface", depthDesc.StencilEnable ? "on" : "off");
		state.programs.push_back({ a_key, program });
		return program;
	}

	void Submit(std::shared_ptr<Run> a_run)
	{
		auto& state = GetState();
		if (!a_run || state.failed)
			return;
		const auto& client = state.clients[static_cast<std::size_t>(a_run->view)];
		if (!client)
			return;
		a_run->revision = RevisionOf(*a_run);
		const std::uint32_t frame = GameFrame();
		while (!state.kept.empty() && frame - state.kept.front().first > kKeptFrames)
			state.kept.pop_front();
		state.kept.emplace_back(frame, std::move(a_run->owners));
		++state.runs;
		state.draws += a_run->draws.size();
		std::atomic_store(&client->run, std::shared_ptr<const Run>(std::move(a_run)));
		client->pending = true;
		FrameAnchors::Run(FrameAnchors::Anchor::EngineDraws);
		client->pending = false;
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_WATER)";
		if (state.failed)
			return "failed: the engine draws the water";
		return fmt::format("{} runs, {} draws, {} programs", state.runs, state.draws, state.programs.size());
	}
}
