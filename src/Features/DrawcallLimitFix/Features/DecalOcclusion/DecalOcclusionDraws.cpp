// The decal occlusion family's render graph side: one client at the DecalQueries anchor, made once at setup, whose pass draws a test's
// latched draws into kDECAL_OCCLUSION with RenderGraph/DecalOcclusion.hlsl, each counted by an ORG occlusion query of the run's range.
// The engine ring slot the test began answers the sum (RenderGraph/Engine/Queries).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/DecalOcclusion/DecalOcclusion.h"

#include "Globals.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <vector>

namespace DCLF::Features::DecalOcclusion::DecalDraws
{
	namespace
	{
		namespace Queries = ::RenderGraph::Engine::OcclusionQueries;

		constexpr const char* kShader = "RenderGraph/DecalOcclusion.hlsl";
		constexpr const char* kClientId = "cs.decals.occlusion";
		constexpr auto kAnchor = FrameAnchors::Anchor::DecalQueries;
		constexpr auto kDepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL::kDECAL_OCCLUSION;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawConstants));
		static_assert(kMaxDraws <= 32, "a run's begun queries are one bit each, and a query ring holds at most 32 a run");

		/** @brief DecalOcclusion.hlsl's push block (DECAL_OCCLUSION_RESOURCES): the draw's latch. */
		struct DrawBlock
		{
			ShaderBindings::Latch draw;
		};

		/** @brief The client's state, which its pass shares. */
		struct Client
		{
			std::shared_ptr<const RasterProgram> program;
			std::shared_ptr<org::LatchBlock> latch;
			// A draw's index count, opened by the commit for its run only.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			std::shared_ptr<org::ExternalTextureResource> depth;  // kDECAL_OCCLUSION, imported at the registration
			std::uint32_t width = 0, height = 0;                  // its size: a draw's render area is clamped to it
			std::shared_ptr<Queries::QueryRing> queries;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			bool pending = false;
			std::atomic<std::uint64_t> executed{ 0 };
		};

		struct Bindings
		{
			org::DeclaredViewToken depth{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const Client> client;
		};

		/**
		 * @brief A test's draws, each in a render pass of its own on kDECAL_OCCLUSION (RasterProgram::Draw), counted by query i of the
		 * run's range: the range reset first, resolved after (every query available).
		 */
		class OcclusionPass final : public org::TypedRenderGraphPass<OcclusionPass, Frame, Bindings>
		{
		public:
			explicit OcclusionPass(std::shared_ptr<Client> a_client) :
				client(std::move(a_client)) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.depth = a_builder.DepthReadWrite(client->depth).View();
				return bindings;
			}

			// The run's draws (their counts and viewports) and its clear, as the render thread published them before the run. The latched
			// values (matrices, geometry) are not in it.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const { return { std::atomic_load(&client->run), client }; }

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.run || !a_frame.client || !a_frame.client->program || !a_frame.client->queries)
					return;
				const auto& run = *a_frame.run;
				const auto& c = *a_frame.client;
				const auto& queries = *c.queries;
				const org::RunRef latchRun = a_recording.Run();
				auto& commands = a_recording.Commands();
				queries.Reset(commands, latchRun);
				std::uint32_t begun = 0;
				for (std::uint32_t i = 0; i < run.draws.size() && i < queries.PerRun(); ++i) {
					const auto& latched = run.draws[i];
					DrawBlock block{};
					block.draw = ShaderBindings::LatchOf(*c.latch, latchRun, i * kDrawStride);
					// The decal's clear, at the test's first draw, as the engine's target change asks for it.
					RasterProgram::Depth depth{};
					depth.view = &a_bindings.depth;
					depth.readOnly = false;
					depth.load = i == 0 && run.clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
					depth.clear = 1.0f;
					// The shadow state's viewport, inside the target (a render area past it is invalid). An empty one draws nothing, and its
					// query is begun and ended empty by Resolve.
					const auto x = (std::min)(latched.viewport.rect[0], c.width);
					const auto y = (std::min)(latched.viewport.rect[1], c.height);
					RasterProgram::DrawDesc draw{};
					draw.depth = &depth;
					draw.viewport = { .x = x,
						.y = y,
						.width = (std::min)(latched.viewport.rect[2], c.width - x),
						.height = (std::min)(latched.viewport.rect[3], c.height - y),
						.minDepth = latched.viewport.depthRange[0],
						.maxDepth = latched.viewport.depthRange[1] };
					draw.constants = ShaderBindings::Data(block);
					draw.debugName = "DecalOcclusion";
					// The count comes from the draw's gate entry, which the client's commit opens for this run only.
					draw.indirect = &c.gate->Latch();
					draw.indirectOffset = c.gate->Offset(i);
					draw.occlusionPool = queries.Pool(latchRun);
					draw.occlusionIndex = queries.Index(latchRun, i);
					if (c.program->Draw(a_recording, draw))
						begun |= 1u << i;
				}
				queries.Resolve(commands, latchRun, begun);
			}

		private:
			std::shared_ptr<Client> client;
		};

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::shared_ptr<Client> client;
			bool reserved = false;
			bool setUp = false;
			bool failed = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Stops the family for good: the tests are the engine's again. */
		void Fail(const char* a_why)
		{
			auto& state = GetState();
			if (!state.failed)
				logger::error("[DCLF] decal occlusion: back to the engine's decal tests for the session: {}", a_why);
			state.failed = true;
			if (state.client)
				FrameAnchors::Unregister(kClientId);
		}

		/** @brief kDECAL_OCCLUSION's depth format, from its first view, and its size (Unknown when it has no view). */
		rhi::Format DepthTarget(std::uint32_t& a_width, std::uint32_t& a_height)
		{
			a_width = a_height = 0;
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return rhi::Format::Unknown;
			auto* view = renderer->GetDepthStencilData().depthStencils[static_cast<std::size_t>(kDepthTarget)].views[0];
			if (!view)
				return rhi::Format::Unknown;
			auto* real = ::RenderGraph::Engine::Tripwire::Unwrap(view);
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			real->GetDesc(&desc);
			ID3D11Resource* resource = nullptr;
			real->GetResource(&resource);
			if (resource) {
				ID3D11Texture2D* texture = nullptr;
				if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture) {
					D3D11_TEXTURE2D_DESC textureDesc{};
					texture->GetDesc(&textureDesc);
					a_width = textureDesc.Width;
					a_height = textureDesc.Height;
					texture->Release();
				}
				resource->Release();
			}
			return rhi::helpers::ToRHI(desc.Format);
		}

		/** @brief The client, registered with its anchor. Null, logged, when its program, target, gate or queries cannot be made. */
		std::shared_ptr<Client> MakeClient()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			std::uint32_t width = 0, height = 0;
			const auto format = DepthTarget(width, height);
			if (format == rhi::Format::Unknown || !width || !height) {
				logger::warn("[DCLF] decal occlusion: kDECAL_OCCLUSION has no depth view to read its format and size from; the decal tests stay the engine's");
				return nullptr;
			}
			auto client = std::make_shared<Client>();
			client->width = width;
			client->height = height;
			// Depth only: the position's depth, tested and written as the engine's silhouette pass does, both faces drawn (only the
			// presence of samples matters to the engine's read).
			RasterProgram::Desc desc{};
			desc.pixelSource = kShader;
			desc.pixelEntry = L"PSMain";
			desc.vertexSource = kShader;
			desc.vertexEntry = L"VSMain";
			desc.constantWords = ShaderBindings::Words<DrawBlock>();
			desc.depthFormat = format;
			desc.depth = { .depthEnable = true, .depthWrite = true, .depthFunc = rhi::CompareOp::LessEqual };
			desc.raster = { .cull = rhi::CullMode::None };
			desc.commonDefines = false;
			client->program = RasterProgram::Load(host->Device(), desc);
			client->latch = std::make_shared<org::LatchBlock>(std::string(kClientId) + ".latch", kMaxDraws * kDrawStride);
			client->gate = ::RenderGraph::RunGate::Create(std::string(kClientId) + ".gate", kMaxDraws);
			client->depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(kDepthTarget));
			client->queries = Queries::QueryRing::Create(std::string(kClientId) + ".queries", kMaxDraws);
			if (!client->program || !client->gate || !client->depth || !client->queries) {
				logger::warn("[DCLF] decal occlusion: the client cannot be made (program {}, run gate {}, kDECAL_OCCLUSION {}, query ring {}); the decal tests stay the engine's",
					client->program ? "made" : "failed", client->gate ? "made" : "failed", client->depth ? "imported" : "failed", client->queries ? "made" : "failed");
				return nullptr;
			}

			FrameAnchors::ClientDesc clientDesc{};
			clientDesc.id = kClientId;
			clientDesc.gates = { client->gate };
			clientDesc.passes.push_back({ .name = std::string(kClientId) + ".draws",
				.anchor = kAnchor,
				.order = 0,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<OcclusionPass>(client); } });
			clientDesc.commit = [client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!run || !host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
				std::vector<std::int32_t> slots;
				for (std::uint32_t i = 0; i < run->draws.size() && i < kMaxDraws; ++i) {
					const auto& draw = run->draws[i];
					client->latch->WriteValue(latchRun, i * kDrawStride, draw.constants);
					client->gate->OpenDraw(i, draw.indexCount, 1);
					slots.push_back(run->query);
				}
				// Every draw counts the test's ring slot, which answers their sum.
				client->queries->Commit(latchRun, slots);
			};
			clientDesc.executed = [client](const FrameAnchors::Point&) {
				client->queries->Executed();
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] decal occlusion: the first {} run drawn by the graph", kClientId);
			};
			clientDesc.active = [client] { return client->pending; };
			clientDesc.onFailed = [] { Fail("the render graph failed with the decal occlusion pass in it"); };
			if (!FrameAnchors::Register(std::move(clientDesc))) {
				logger::warn("[DCLF] decal occlusion: the {} client could not be registered; the decal tests stay the engine's", kClientId);
				return nullptr;
			}
			logger::info("[DCLF] decal occlusion: the {} client registered at setup (kDECAL_OCCLUSION, {} queries a test)", kClientId, kMaxDraws);
			// The one build request: the next build point (a menu frame's end) builds the graph with the client, before any world frame.
			FrameAnchors::RequestJoin(kClientId, kAnchor);
			return client;
		}

		/** @brief What a run's recording depends on: everything of its draws but the latched values. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(a_run.clear);
			mix(a_run.draws.size());
			for (const auto& draw : a_run.draws) {
				for (const auto v : draw.viewport.rect)
					mix(v);
				std::uint32_t range[2];
				std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
				mix((std::uint64_t(range[0]) << 32) | range[1]);
			}
			return hash;
		}
	}

	void Reserve()
	{
		auto& state = GetState();
		state.reserved = FrameAnchors::Reserve(kAnchor);
		if (!state.reserved)
			logger::warn("[DCLF] decal occlusion: the DecalQueries anchor could not be reserved; the decal tests stay the engine's");
	}

	bool Available()
	{
		const auto& state = GetState();
		return state.reserved && !state.failed && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(kAnchor);
	}

	bool Setup()
	{
		auto& state = GetState();
		if (state.setUp || state.failed || !state.reserved)
			return true;
		if (!RenderGraphRuntime::Get().Host() || !FrameAnchors::IsRenderOwner() || !Available())
			return false;
		state.setUp = true;
		if (!Queries::Active()) {
			logger::warn("[DCLF] decal occlusion: the occlusion query detours are not installed; the decal tests stay the engine's");
			return true;
		}
		state.client = MakeClient();
		return true;
	}

	bool HasClient()
	{
		const auto& state = GetState();
		return !state.failed && state.client != nullptr;
	}

	bool CanTake()
	{
		const auto& state = GetState();
		if (state.failed || !state.client)
			return false;
		if (!FrameAnchors::CanRun(kClientId, kAnchor)) {
			static Util::ReasonLog log("[DCLF] decal occlusion: a decal test answers visible", spdlog::level::err);
			log.Report(std::format("{}: its anchor cannot run now (not in the built graph, or its ring is full)", kClientId));
			return false;
		}
		return true;
	}

	void Keep(Family::Owners&& a_owners)
	{
		GetState().kept.Keep(std::move(a_owners));
	}

	bool Submit(std::shared_ptr<Run> a_run)
	{
		auto& state = GetState();
		if (!a_run || state.failed || !state.client)
			return false;
		const auto& client = state.client;
		a_run->revision = RevisionOf(*a_run);
		state.kept.Keep(std::move(a_run->owners));
		++state.runs;
		state.draws += a_run->draws.size();
		std::atomic_store(&client->run, std::shared_ptr<const Run>(std::move(a_run)));
		return Family::RunWithheld("decal occlusion", kClientId, kAnchor, client->pending);
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_OCCLUSION_QUERIES)";
		if (state.failed)
			return "failed: the engine draws the decal tests";
		if (!state.client)
			return state.setUp ? "no client: the engine draws the decal tests" : "not set up yet";
		return std::format("{} tests ({} drawn), {} draws; occlusion queries: {}", state.runs, state.client->executed.load(std::memory_order_relaxed), state.draws,
			Queries::Status());
	}
}
