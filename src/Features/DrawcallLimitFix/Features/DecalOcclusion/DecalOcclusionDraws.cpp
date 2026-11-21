// The render graph side of decal occlusion: one client at the DecalQueries anchor draws each test's latched shapes
// into kDECAL_OCCLUSION and counts them with occlusion queries.
// In native mode the start stage runs several tests. The k-th test uses instance k, with its own gate entries,
// latch region and share of the query range.
// The client is declared once (frame revision plan, 6.19). A new readback chunk of its query ring registers it again with the
// next frame revision.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/DecalOcclusion/DecalOcclusion.h"

#include "Features/DrawcallLimitFix/Core/Engine/Family/FamilyClient.h"

#include "Globals.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "Utils/Hash.h"
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
#include <optional>
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

		struct Client : Family::ClientState<Run>
		{
			// Queued at setup. Passes read it after the revision start that collected it.
			::RenderGraph::Programs::Fixed<RasterProgram> program;
			std::shared_ptr<org::LatchBlock> latch;
			// Opened by the commit for its own run only.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			FrameAnchors::Texture depth;  // kDECAL_OCCLUSION
			std::uint32_t width = 0, height = 0;                  // Render areas are clamped to this.
			std::shared_ptr<Queries::QueryRing> queries;
			// The ring's readback count, observed at setup and when a commit adds a chunk. The client's key reads it.
			::RenderGraph::Frame::Observed<std::size_t> readbacks{ "DCLF.DecalOcclusion.Readbacks" };
			// The base's run is the test the next tap commits.
			std::array<::RenderGraph::RunSlots<Run>, kInstances> runs;
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
			Family::Client<Run, Client> clients{ "decal occlusion" };
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

		/** @brief The client's program, latch, gate, depth import and queries. Null, logged, when a part cannot be made. */
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
			client->id = kClientId;
			client->anchor = kAnchor;
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
			client->program = ::RenderGraph::Programs::Queue("DCLF decal occlusion", std::move(desc));
			client->latch = std::make_shared<org::LatchBlock>(std::string(kClientId) + ".latch", kInstances * kMaxDraws * kDrawStride);
			client->gate = ::RenderGraph::RunGate::Create(std::string(kClientId) + ".gate", kInstances * kMaxDraws);
			client->depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(static_cast<std::uint32_t>(kDepthTarget)));
			client->queries = Queries::QueryRing::Create(std::string(kClientId) + ".queries", kInstances * kMaxDraws);
			if (!client->program || !client->depth || !client->queries) {
				logger::warn("[DCLF] decal occlusion: the client cannot be made (program {}, kDECAL_OCCLUSION {}, query ring {}); the decal tests stay the engine's",
					client->program ? "queued" : "failed", client->depth ? "imported" : "failed", client->queries ? "made" : "failed");
				return nullptr;
			}
			client->readbacks.Observe(client->queries->Readbacks().size());
			return client;
		}

		/** @brief What a registration is made for: how many query readbacks its pass declares. */
		struct ClientKey
		{
			std::size_t readbacks = 0;
			bool operator==(const ClientKey&) const = default;
		};

		/** @brief The registration of a_client. Its pass declares every readback chunk the ring has now. */
		bool MakeDesc(const std::shared_ptr<Client>& a_client, FrameAnchors::ClientDesc& a_desc)
		{
			a_desc.id = kClientId;
			a_desc.gates = { a_client->gate };
			a_desc.passes.push_back({ .name = std::string(kClientId) + ".draws",
				.anchor = kAnchor,
				.order = 0,
				.kind = FrameAnchors::PassKind::Render,
				.makeInstance = [client = a_client](std::uint32_t a_instance) -> std::shared_ptr<org::RenderPass> { return std::make_shared<OcclusionPass>(client, a_instance); },
				// The draws are only known after the commit.
				.atExecution = true });
			a_desc.maxInstances = kInstances;
			// Declare every instance, because the engine decides late how many decals it tests. Unused ones stay closed.
			a_desc.instances = [] { return kInstances; };
			a_desc.commit = [client = a_client](const FrameAnchors::Point&) {
				const auto run = client->Current();
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
				// Every draw counts into the test's ring slot, which answers the sum. A new readback chunk changes the key, so the
				// next revision registers the client again with it.
				if (client->queries->Commit(latchRun, slots, instance * kMaxDraws))
					client->readbacks.Observe(client->queries->Readbacks().size());
			};
			a_desc.executed = [client = a_client](const FrameAnchors::Point&) {
				client->queries->Executed();
				if (client->CountExecuted())
					logger::info("[DCLF] decal occlusion: the first {} run drawn by the graph", kClientId);
			};
			a_desc.active = [client = a_client] { return client->pending; };
			logger::info("[DCLF] decal occlusion: the {} client is made (kDECAL_OCCLUSION, {} queries a test, {} tests a frame, {} readback chunks)", kClientId,
				kMaxDraws, kInstances, a_client->queries->Readbacks().size());
			return true;
		}
	}

	bool Available()
	{
		return GetState().clients.Available();
	}

	bool Setup()
	{
		auto& state = GetState();
		if (const auto result = state.clients.BeginSetup())
			return *result;
		if (!Queries::Active()) {
			logger::warn("[DCLF] decal occlusion: the occlusion query detours are not installed; the decal tests stay the engine's");
			return true;
		}
		auto client = MakeClient();
		if (!client)
			return true;
		namespace RF = ::RenderGraph::Frame;
		// kDECAL_OCCLUSION is made once with the engine's targets. The key is the ring's readback count, so a chunk the ring makes
		// later (more than its setup's lease indices) registers the client again with the next revision.
		state.clients.Declare<ClientKey>(
			0, client, RF::DependsOn(client->readbacks.Var()),
			[client](const RF::Inputs& a_in) -> std::optional<ClientKey> {
				// MakeClient observed the setup count, so a revision always carries one.
				const auto* readbacks = client->readbacks.Get(a_in);
				return ClientKey{ readbacks ? *readbacks : 0 };
			},
			[client](const ClientKey&, FrameAnchors::ClientDesc& a_desc) { return MakeDesc(client, a_desc); },
			"the decal tests stay the engine's");
		return true;
	}

	bool HasClient()
	{
		return GetState().clients.At(0) != nullptr;
	}

	bool CanTake()
	{
		// Until a revision registers the client, the engine's test answers.
		return GetState().clients.CanTake(0, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Start));
	}

	void Keep(Family::Owners&& a_owners)
	{
		GetState().clients.Keep(std::move(a_owners));
	}

	bool Submit(std::shared_ptr<Run> a_run)
	{
		return GetState().clients.Submit(0, std::move(a_run));
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (not graph mode)";
		if (!state.clients.At(0))
			return state.clients.SetUp() ? "no client: the engine draws the decal tests" : "not set up yet";
		return std::format("{} tests ({} drawn), {} draws; occlusion queries: {}", state.clients.Runs(), state.clients.Executed(), state.clients.Draws(),
			Queries::Status());
	}
}

namespace DCLF::Features::DecalOcclusion
{
	std::uint64_t Run::Hash() const
	{
		Util::Fnv1a hash;
		hash.Mix(clear);
		hash.Mix(draws.size());
		for (const auto& draw : draws) {
			for (const auto v : draw.viewport.rect)
				hash.Mix(v);
			std::uint32_t range[2];
			std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
			hash.Mix((std::uint64_t(range[0]) << 32) | range[1]);
		}
		return hash.Value();
	}
}
