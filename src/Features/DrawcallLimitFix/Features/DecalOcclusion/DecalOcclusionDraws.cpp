// The render graph side of decal occlusion: one client at the DecalQueries anchor draws each test's latched shapes
// into kDECAL_OCCLUSION and counts them with occlusion queries.
// In native mode the start stage runs several tests. The k-th test uses instance k, with its own gate entries,
// latch region and share of the query range.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/DecalOcclusion/DecalOcclusion.h"

#include "Globals.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
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
#include <array>
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
		// Tests per start stage run, one tap each.
		constexpr std::uint32_t kInstances = 4;
		static_assert(kInstances * kMaxDraws <= 32, "a run's begun queries are one bit each, and a query ring holds at most 32 a run");

		// DecalOcclusion.hlsl's push block (DECAL_OCCLUSION_RESOURCES).
		struct DrawBlock
		{
			ShaderBindings::Latch draw;
		};

		struct Client
		{
			std::shared_ptr<const RasterProgram> program;
			std::shared_ptr<org::LatchBlock> latch;
			// Opened by the commit for its own run only.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			FrameAnchors::Texture depth;  // kDECAL_OCCLUSION
			std::uint32_t width = 0, height = 0;                  // Render areas are clamped to this.
			std::shared_ptr<Queries::QueryRing> queries;
			// run is the test the next tap commits. Render thread.
			std::shared_ptr<const Run> run;
			std::array<::RenderGraph::RunSlots<Run>, kInstances> runs;
			bool pending = false;
			std::atomic<std::uint64_t> executed{ 0 };
		};

		struct Bindings
		{
			org::DeclaredViewToken depth{};
			std::size_t readbacks = 0;
		};

		struct Frame
		{
			std::shared_ptr<const Client> client;
			std::uint32_t instance = 0;
		};

		// Each draw gets its own render pass and query. The instance's queries are reset first and resolved after.
		class OcclusionPass final : public org::TypedRenderGraphPass<OcclusionPass, Frame, Bindings>
		{
		public:
			OcclusionPass(std::shared_ptr<Client> a_client, std::uint32_t a_instance) :
				client(std::move(a_client)), instance(a_instance) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.depth = a_builder.DepthReadWrite(client->depth).View();
				// Declared as copy destinations so the graph orders and tracks the resolve.
				if (client->queries)
					for (const auto& readback : client->queries->Readbacks()) {
						a_builder.CopyDestination(readback);
						++bindings.readbacks;
					}
				return bindings;
			}

			// The draws are read at execution, so only the instance belongs in the revision.
			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(instance); }

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const { return { client, instance }; }

			// An unused instance records only the barriers and touches none of its queries.
			static bool SkipsAtExecution(const Bindings&, const Frame& a_frame, const org::PassRecordContext& a_recording)
			{
				const auto run = a_frame.client ? a_frame.client->runs[a_frame.instance].Find(a_recording.Run()) : nullptr;
				return !run || run->draws.empty();
			}

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.client || !a_frame.client->program || !a_frame.client->queries)
					return;
				const auto& c = *a_frame.client;
				const auto runRef = c.runs[a_frame.instance].Find(a_recording.Run());
				if (!runRef)
					return;
				const auto& run = *runRef;
				const auto& queries = *c.queries;
				const org::RunRef latchRun = a_recording.Run();
				auto& commands = a_recording.Commands();
				const std::uint32_t first = a_frame.instance * kMaxDraws;
				queries.Reset(commands, latchRun, first, kMaxDraws);
				std::uint32_t begun = 0;
				for (std::uint32_t i = 0; i < run.draws.size() && i < kMaxDraws; ++i) {
					const auto& latched = run.draws[i];
					const std::uint32_t entry = FrameAnchors::InstanceEntry(a_frame.instance, kMaxDraws, i);
					DrawBlock block{};
					block.draw = ShaderBindings::LatchOf(*c.latch, latchRun, entry * kDrawStride);
					RasterProgram::Depth depth{};
					depth.view = &a_bindings.depth;
					depth.readOnly = false;
					depth.load = i == 0 && run.clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
					depth.clear = 1.0f;
					// Clamp to the target, because a render area past it is invalid. An empty one draws nothing and Resolve still closes its query.
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
					draw.indirect = &c.gate->Latch();
					draw.indirectOffset = c.gate->Offset(entry);
					draw.occlusionPool = queries.Pool(latchRun);
					draw.occlusionIndex = queries.Index(latchRun, first + i);
					if (c.program->Draw(a_recording, draw))
						begun |= 1u << i;
				}
				queries.Resolve(commands, latchRun, begun, a_bindings.readbacks, first, kMaxDraws);
			}

		private:
			std::shared_ptr<Client> client;
			std::uint32_t instance = 0;
		};

		// Render thread.
		struct DrawsState
		{
			std::shared_ptr<Client> client;
			bool reserved = false;
			bool setUp = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

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
			// Depth only, both faces, like the engine's silhouette pass. Only whether samples pass matters.
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
			client->latch = std::make_shared<org::LatchBlock>(std::string(kClientId) + ".latch", kInstances * kMaxDraws * kDrawStride);
			client->gate = ::RenderGraph::RunGate::Create(std::string(kClientId) + ".gate", kInstances * kMaxDraws);
			client->depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(kDepthTarget));
			client->queries = Queries::QueryRing::Create(std::string(kClientId) + ".queries", kInstances * kMaxDraws);
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
				.makeInstance = [client](std::uint32_t a_instance) -> std::shared_ptr<org::RenderPass> { return std::make_shared<OcclusionPass>(client, a_instance); },
				// The draws are only known after the commit.
				.atExecution = true });
			clientDesc.maxInstances = kInstances;
			// Declare every instance, because the engine decides late how many decals it tests. Unused ones stay closed.
			clientDesc.instances = [] { return kInstances; };
			clientDesc.commit = [client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
				// 0 on the anchor's own epoch.
				const std::uint32_t instance = (std::min)(FrameAnchors::TapInstance(), kInstances - 1);
				client->runs[instance].Store(latchRun, run);
				if (!run)
					return;
				std::vector<std::int32_t> slots;
				for (std::uint32_t i = 0; i < run->draws.size() && i < kMaxDraws; ++i) {
					const auto& draw = run->draws[i];
					const std::uint32_t entry = FrameAnchors::InstanceEntry(instance, kMaxDraws, i);
					client->latch->WriteValue(latchRun, entry * kDrawStride, draw.constants);
					client->gate->OpenDraw(entry, draw.indexCount, 1);
					slots.push_back(run->query);
				}
				// Every draw counts into the test's ring slot, which answers the sum. A new readback applies from the next build.
				if (client->queries->Commit(latchRun, slots, instance * kMaxDraws))
					FrameAnchors::Refresh(kClientId);
			};
			clientDesc.executed = [client](const FrameAnchors::Point&) {
				client->queries->Executed();
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] decal occlusion: the first {} run drawn by the graph", kClientId);
			};
			clientDesc.active = [client] { return client->pending; };
			if (!FrameAnchors::Register(std::move(clientDesc))) {
				logger::warn("[DCLF] decal occlusion: the {} client could not be registered; the decal tests stay the engine's", kClientId);
				return nullptr;
			}
			logger::info("[DCLF] decal occlusion: the {} client registered at setup (kDECAL_OCCLUSION, {} queries a test, {} tests a frame)", kClientId, kMaxDraws, kInstances);
			return client;
		}

		// Everything about the draws except the latched values.
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
		return state.reserved && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(kAnchor);
	}

	bool Setup()
	{
		auto& state = GetState();
		if (state.setUp || !state.reserved)
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
		return state.client != nullptr;
	}

	bool CanTake()
	{
		const auto& state = GetState();
		if (!state.client)
			return false;
		return Family::HeldForBatch("decal occlusion", kClientId, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Start), kAnchor);
	}

	void Keep(Family::Owners&& a_owners)
	{
		GetState().kept.Keep(std::move(a_owners));
	}

	bool Submit(std::shared_ptr<Run> a_run)
	{
		auto& state = GetState();
		if (!a_run || !state.client)
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
		if (!state.client)
			return state.setUp ? "no client: the engine draws the decal tests" : "not set up yet";
		return std::format("{} tests ({} drawn), {} draws; occlusion queries: {}", state.runs, state.client->executed.load(std::memory_order_relaxed), state.draws,
			Queries::Status());
	}
}
