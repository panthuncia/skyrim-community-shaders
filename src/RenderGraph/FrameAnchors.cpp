#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameAnchors.h"

#	include "Features/Upscaling.h"
#	include "Globals.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/FrameAnchorSelfTest.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/TextureDescription.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <map>

namespace FrameAnchors
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		constexpr std::uint32_t kAnchors = static_cast<std::uint32_t>(Anchor::Count);
		static_assert(static_cast<std::uint32_t>(Segment::AnchorPresent) - static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + 1 == kAnchors,
			"one segment per anchor, in the anchors' order");
		static_assert(static_cast<std::uint32_t>(Segment::Count) <= 32, "TakeSubmittedSegments holds one bit per segment");

		constexpr const char* kExtensionId = "cs.org.frame-anchors";

		struct AnchorInfo
		{
			const char* name;
			SegmentNames names;
			bool late;  // may be the last graph work before an upscaler, frame generation or the present
		};

		constexpr std::array<AnchorInfo, kAnchors> kInfo{ {
			{ "ReflectionsPrepass", { "CS anchor: reflections prepass", "CS anchor: reflections prepass inputs", "FrameAnchors::ReflectionsPrepass / ", "cs.anchor.reflections-prepass" }, false },
			{ "EarlyPrepass", { "CS anchor: early prepass", "CS anchor: early prepass inputs", "FrameAnchors::EarlyPrepass / ", "cs.anchor.early-prepass" }, false },
			{ "Prepass", { "CS anchor: prepass", "CS anchor: prepass inputs", "FrameAnchors::Prepass / ", "cs.anchor.prepass" }, false },
			{ "DeferredPasses", { "CS anchor: deferred passes", "CS anchor: deferred passes inputs", "FrameAnchors::DeferredPasses / ", "cs.anchor.deferred-passes" }, false },
			{ "PostProcessing", { "CS anchor: post-processing", "CS anchor: post-processing inputs", "FrameAnchors::PostProcessing / ", "cs.anchor.post-processing" }, true },
			{ "Tonemap", { "CS anchor: tonemap", "CS anchor: tonemap inputs", "FrameAnchors::Tonemap / ", "cs.anchor.tonemap" }, true },
			{ "Present", { "CS anchor: present", "CS anchor: present inputs", "FrameAnchors::Present / ", "cs.anchor.present" }, true },
		} };

		// Where each segment runs in a frame: AddToFrame puts an anchor before the first epoch of the order that runs after it. A
		// segment's place only has to be right against the anchors next to it; the order sizes the frame slots and names the frame,
		// while the ledgers order the work whatever runs.
		constexpr std::array<Segment, static_cast<std::size_t>(Segment::Count)> kTimeline{
			Segment::ShadowView,
			Segment::SkyOcclusion,
			Segment::AnchorEarlyPrepass,  // Main::RenderShadowMaps returned
			Segment::ZPrepass,            // Main::RenderDepth
			Segment::FirstPersonDepth,
			Segment::Reflection,
			Segment::AnchorReflectionsPrepass,  // the cubemap render, nominally here (zero or more times a frame)
			Segment::LightCulling,              // Light Limit Fix's Prepass
			Segment::AnchorPrepass,             // after every feature's Prepass
			Segment::MainOpaque,
			Segment::AnchorDeferredPasses,  // EndDeferred, after the blended decals
			Segment::Forward,
			Segment::FirstPerson,
			Segment::AnchorPostProcessing,  // Main_PostProcessing, before the upscaler
			Segment::AnchorTonemap,         // in the ImageSpace chain after it
			Segment::AnchorPresent,
		};

		std::uint32_t RankOf(std::uint32_t a_segment)
		{
			for (std::uint32_t i = 0; i < kTimeline.size(); ++i)
				if (static_cast<std::uint32_t>(kTimeline[i]) == a_segment)
					return i;
			return static_cast<std::uint32_t>(kTimeline.size());
		}

		std::uint32_t IndexOf(Anchor a_anchor) { return static_cast<std::uint32_t>(a_anchor); }

		std::uint32_t EpochOfAnchor(Anchor a_anchor) { return RenderGraphRuntime::EpochOf(static_cast<Segment>(SegmentOf(a_anchor))); }

		/** @brief An engine target imported for the anchors' passes, kept while the engine's texture stays the same. */
		struct EngineImport
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			std::shared_ptr<org::ExternalTextureResource> resource;
			std::string id;
		};

		/** @brief One pass of the build: its client (which owns its desc) and the desc. */
		struct ScheduledPass
		{
			std::shared_ptr<const ClientDesc> client;
			const PassDesc* pass = nullptr;
		};

		/** @brief What every graph build makes the anchors' passes and resources from. Immutable once published. */
		struct Snapshot
		{
			std::vector<std::shared_ptr<const ClientDesc>> clients;  // registration order
			std::vector<ScheduledPass> passes;                       // by anchor, then order key, then registration
			std::vector<std::pair<std::string, std::shared_ptr<org::ExternalTextureResource>>> imports;
		};

		/** @brief The anchors' state. Render thread, but the snapshot, which a build reads. */
		struct State
		{
			std::array<bool, kAnchors> reserved{};
			bool frozen = false;  // AddToFrame ran: the reservations are the frame's epochs
			org::GraphHost* host = nullptr;
			std::vector<std::shared_ptr<const ClientDesc>> clients;
			std::map<std::string, EngineImport, std::less<>> imports;  // by graph id
			std::atomic<std::shared_ptr<const Snapshot>> snapshot;
		};

		State& Anchors()
		{
			static State s_state;
			return s_state;
		}

		bool HasPasses(const ClientDesc& a_client, Anchor a_anchor)
		{
			return std::ranges::any_of(a_client.passes, [&](const PassDesc& a_pass) { return a_pass.anchor == a_anchor; });
		}

		/** @brief Publishes the clients and imports as they are now, for the next builds. */
		void Publish(State& a_state)
		{
			auto snapshot = std::make_shared<Snapshot>();
			snapshot->clients = a_state.clients;
			for (const auto& client : a_state.clients)
				for (const auto& pass : client->passes)
					snapshot->passes.push_back({ client, &pass });
			// Stable: equal keys keep the registration order, and each client's own.
			std::ranges::stable_sort(snapshot->passes, [](const ScheduledPass& a, const ScheduledPass& b) {
				if (a.pass->anchor != b.pass->anchor)
					return a.pass->anchor < b.pass->anchor;
				return a.pass->order < b.pass->order;
			});
			for (const auto& [key, import] : a_state.imports)
				snapshot->imports.emplace_back(import.id, import.resource);
			a_state.snapshot.store(std::move(snapshot), std::memory_order_release);
		}

		/** @brief The anchors' epochs built again at their next execution (the others keep their graphs). */
		void Rebuild(State& a_state, std::uint32_t a_anchorMask)
		{
			if (!a_state.host)
				return;
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if ((a_anchorMask >> i & 1u) && a_state.reserved[i])
					a_state.host->RequestRebuild(EpochOfAnchor(static_cast<Anchor>(i)));
		}

		std::uint32_t AnchorMask(const ClientDesc& a_client)
		{
			std::uint32_t mask = 0;
			for (const auto& pass : a_client.passes)
				if (IndexOf(pass.anchor) < kAnchors)
					mask |= 1u << IndexOf(pass.anchor);
			return mask;
		}

		/**
		 * @brief The anchors' passes in each graph: an anchor epoch's graph takes its anchor's passes and registers the clients'
		 * resources and the engine imports; every other epoch's graph gets nothing from it.
		 */
		class AnchorExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit AnchorExtension(std::shared_ptr<const Snapshot> a_snapshot) :
				snapshot(std::move(a_snapshot)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				if (!snapshot || !IsAnchorGraph(a_graph))
					return;
				for (const auto& [id, resource] : snapshot->imports)
					a_graph.RegisterResource(org::ResourceIdentifier(id), resource);
				for (const auto& client : snapshot->clients)
					if (client->registerResources)
						client->registerResources(a_graph);
			}

			void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				if (!snapshot || !IsAnchorGraph(a_graph))
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				for (const auto& scheduled : snapshot->passes) {
					const auto& pass = *scheduled.pass;
					if (EpochOfAnchor(pass.anchor) != epoch)
						continue;
					auto made = pass.make ? pass.make() : nullptr;
					if (!made)
						continue;
					auto desc = pass.kind == PassKind::Compute ? org::RenderGraph::ExternalPassDesc::Compute(pass.name, std::move(made)) :
					            pass.kind == PassKind::Copy    ? org::RenderGraph::ExternalPassDesc::Copy(pass.name, std::move(made)) :
					                                             org::RenderGraph::ExternalPassDesc::Render(pass.name, std::move(made));
					// Beside the D3D11 work around the anchor: off the graphics queue, a pass would add a wait on DXVK's stream.
					if (pass.queue == PassQueue::Compute)
						desc.PreferQueue(org::QueueKind::Compute);
					else if (pass.kind != PassKind::Render)
						desc.PreferQueue(org::QueueKind::Graphics);
					a_out.push_back(std::move(desc.Epoch(epoch)));
				}
			}

		private:
			static bool IsAnchorGraph(const org::RenderGraph& a_graph)
			{
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				for (std::uint32_t i = 0; i < kAnchors; ++i)
					if (epoch == EpochOfAnchor(static_cast<Anchor>(i)))
						return true;
				return false;
			}

			std::shared_ptr<const Snapshot> snapshot;
		};

		/** @brief The import's description: its views as their owner (the engine, a feature) made them. Any view may be null. */
		org::TextureDescription DescribeColour(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11ShaderResourceView* a_srv, ID3D11RenderTargetView* a_rtv,
			ID3D11UnorderedAccessView* a_uav)
		{
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_desc.Format);
			desc.channels = 4;
			if (a_srv) {
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				a_srv->GetDesc(&view);
				desc.hasSRV = true;
				desc.srvFormat = rhi::helpers::ToRHI(view.Format);
				desc.format = desc.srvFormat;
			}
			if (a_rtv) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				a_rtv->GetDesc(&view);
				desc.hasRTV = true;
				desc.rtvFormat = rhi::helpers::ToRHI(view.Format);
			}
			if (a_uav) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
				a_uav->GetDesc(&view);
				desc.hasUAV = true;
				desc.uavFormat = rhi::helpers::ToRHI(view.Format);
			}
			return desc;
		}

		org::TextureDescription DescribeDepth(const RE::BSGraphics::DepthStencilData& a_target, const D3D11_TEXTURE2D_DESC& a_desc)
		{
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_desc.Format);
			desc.channels = 1;
			if (a_target.views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC view{};
				a_target.views[0]->GetDesc(&view);
				desc.hasDSV = true;
				desc.dsvFormat = rhi::helpers::ToRHI(view.Format);
				desc.format = desc.dsvFormat;
			}
			if (a_target.depthSRV) {
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				a_target.depthSRV->GetDesc(&view);
				desc.hasSRV = true;
				desc.srvFormat = rhi::helpers::ToRHI(view.Format);
			}
			return desc;
		}

		std::shared_ptr<org::ExternalTextureResource> Import(ID3D11Texture2D* a_texture, const std::string& a_id,
			const std::function<org::TextureDescription(const D3D11_TEXTURE2D_DESC&)>& a_describe)
		{
			auto& state = Anchors();
			auto& runtime = RenderGraphRuntime::Get();
			if (!a_texture || !runtime.IsActive())
				return nullptr;
			if (const auto found = state.imports.find(a_id); found != state.imports.end() && found->second.texture.get() == a_texture)
				return found->second.resource;
			DxvkOrgInteropResourceInfo info{};
			if (!runtime.DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				logger::warn("[ORG] frame anchors: {} cannot be described", a_id);
				return nullptr;
			}
			D3D11_TEXTURE2D_DESC textureDesc{};
			a_texture->GetDesc(&textureDesc);
			auto desc = a_describe(textureDesc);
			// One entry per mip: the graph's views and barriers then cover the whole image.
			const std::uint32_t mips = (std::max)(info.image.mipLevels, 1u);
			for (std::uint32_t mip = 0; mip < mips; ++mip)
				desc.imageDimensions.push_back({ (std::max)(info.image.extent.width >> mip, 1u), (std::max)(info.image.extent.height >> mip, 1u), 0, 0 });
			// A cube (the engine's cubemap targets): its SRV is a cube view, and arraySize counts cubes.
			const std::uint32_t layers = (std::max)(info.image.arrayLayers, 1u);
			desc.isCubemap = (textureDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && layers % 6 == 0;
			const std::uint32_t slices = desc.isCubemap ? layers / 6 : layers;
			desc.isArray = slices > 1;
			desc.arraySize = slices;
			auto resource = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, desc, a_id.c_str());
			if (!resource) {
				logger::warn("[ORG] frame anchors: {} could not be imported", a_id);
				return nullptr;
			}
			EngineImport import;
			import.texture.copy_from(a_texture);
			import.resource = resource;
			import.id = a_id;
			state.imports[a_id] = std::move(import);
			Publish(state);
			// The imports are registered with every anchor graph.
			Rebuild(state, ~0u);
			return resource;
		}
	}

	const char* Name(Anchor a_anchor)
	{
		return IndexOf(a_anchor) < kAnchors ? kInfo[IndexOf(a_anchor)].name : "unknown";
	}

	std::uint32_t SegmentOf(Anchor a_anchor)
	{
		return static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + IndexOf(a_anchor);
	}

	const SegmentNames* NamesOf(std::uint32_t a_segment)
	{
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return nullptr;
		return &kInfo[a_segment - first].names;
	}

	bool IsUnusedSegment(std::uint32_t a_segment)
	{
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return false;
		const auto& state = Anchors();
		return !state.frozen || !state.reserved[a_segment - first];
	}

	bool Reserve(Anchor a_anchor)
	{
		auto& state = Anchors();
		if (IndexOf(a_anchor) >= kAnchors)
			return false;
		if (state.reserved[IndexOf(a_anchor)])
			return true;
		if (state.frozen) {
			logger::warn("[ORG] frame anchor {}: reserved after the render graph initialized; it has no epoch this session", Name(a_anchor));
			return false;
		}
		if (!RenderGraphRuntime::EpochsEnabled()) {
			logger::info("[ORG] frame anchor {}: not reserved, the render graph runs without epochs (CS_ORG_EPOCHS=0)", Name(a_anchor));
			return false;
		}
		state.reserved[IndexOf(a_anchor)] = true;
		return true;
	}

	bool HasEpoch(Anchor a_anchor)
	{
		const auto& state = Anchors();
		return IndexOf(a_anchor) < kAnchors && state.frozen && state.reserved[IndexOf(a_anchor)] && state.host && RenderGraphRuntime::Get().IsActive();
	}

	bool Register(ClientDesc a_client)
	{
		auto& state = Anchors();
		if (!state.host || !RenderGraphRuntime::Get().IsActive()) {
			logger::warn("[ORG] frame anchors: {} not registered, the render graph is not active", a_client.id);
			return false;
		}
		if (a_client.id.empty() || a_client.passes.empty()) {
			logger::warn("[ORG] frame anchors: a client without an id or passes ({}) is not registered", a_client.id);
			return false;
		}
		std::vector<std::string_view> names;
		for (const auto& pass : a_client.passes) {
			if (!HasEpoch(pass.anchor) || pass.name.empty() || !pass.make || std::ranges::find(names, pass.name) != names.end()) {
				logger::warn("[ORG] frame anchors: {} not registered: its pass '{}' at {} {}", a_client.id, pass.name, Name(pass.anchor),
					!HasEpoch(pass.anchor) ? "names an anchor without an epoch (reserve it before the graph initializes)" : "has no name, no make, or a repeated name");
				return false;
			}
			names.push_back(pass.name);
		}
		std::uint32_t mask = AnchorMask(a_client);
		auto client = std::make_shared<const ClientDesc>(std::move(a_client));
		const auto existing = std::ranges::find_if(state.clients, [&](const auto& a_other) { return a_other->id == client->id; });
		if (existing != state.clients.end()) {
			mask |= AnchorMask(**existing);
			*existing = client;
		} else {
			state.clients.push_back(client);
		}
		Publish(state);
		Rebuild(state, mask);
		logger::info("[ORG] frame anchors: {} registered ({} passes)", client->id, client->passes.size());
		return true;
	}

	void Unregister(std::string_view a_id)
	{
		auto& state = Anchors();
		const auto existing = std::ranges::find_if(state.clients, [&](const auto& a_other) { return a_other->id == a_id; });
		if (existing == state.clients.end())
			return;
		const std::uint32_t mask = AnchorMask(**existing);
		state.clients.erase(existing);
		Publish(state);
		Rebuild(state, mask);
	}

	void Refresh(std::string_view a_id)
	{
		auto& state = Anchors();
		const auto existing = std::ranges::find_if(state.clients, [&](const auto& a_other) { return a_other->id == a_id; });
		if (existing != state.clients.end())
			Rebuild(state, AnchorMask(**existing));
	}

	bool Run(const Point& a_point)
	{
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_point.anchor);
		if (index >= kAnchors || !state.reserved[index] || !state.host)
			return false;
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.IsActive())
			return false;
		SelfTest::Ensure();
		GraphResources::SelfTest::Ensure();
		// The clients with work here this frame; none, no epoch.
		std::vector<std::shared_ptr<const ClientDesc>> active;
		for (const auto& client : state.clients)
			if (HasPasses(*client, a_point.anchor) && (!client->active || client->active()))
				active.push_back(client);
		if (active.empty())
			return false;
		const auto segment = static_cast<Segment>(SegmentOf(a_point.anchor));
		bool ran = false;
		{
			RenderGraphRuntime::EpochBodyScope body(segment);
			ran = runtime.ExecuteEpoch(segment, [&](org::RenderGraph&) {
				for (const auto& client : active)
					if (client->commit)
						client->commit(a_point);
			});
		}
		// An epoch must not be the last GPU work before a third-party Vulkan consumer (render-graph.md, "Hazard: external Vulkan
		// work directly after a graph epoch"). After the late anchors an upscaler, frame generation or the present may follow.
		if (ran && kInfo[index].late)
			globals::features::upscaling.SeparateFromGraphEpochs();
		if (ran) {
			SelfTest::AfterRun(a_point.anchor);
			if (a_point.anchor == Anchor::DeferredPasses)
				GraphResources::SelfTest::AfterRun();
			return true;
		}
		// The graph failed and stopped (ExecuteEpoch logged why): every client returns to its D3D11 path.
		logger::error("[ORG] frame anchors: the {} epoch failed; {} clients return to D3D11", Name(a_point.anchor), state.clients.size());
		auto clients = std::move(state.clients);
		state.clients.clear();
		Publish(state);
		for (const auto& client : clients)
			if (client->onFailed)
				client->onFailed();
		return false;
	}

	std::shared_ptr<org::ExternalTextureResource> EngineTarget(std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_target >= static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL))
			return nullptr;
		const auto target = renderer->GetRuntimeData().renderTargets[a_target];
		return Import(target.texture, fmt::format("cs.engine.target.{}", a_target),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, target.SRV, target.RTV, target.UAV); });
	}

	std::shared_ptr<org::ExternalTextureResource> EngineDepth(std::uint32_t a_depthStencil)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_depthStencil >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL))
			return nullptr;
		const auto target = renderer->GetDepthStencilData().depthStencils[a_depthStencil];
		return Import(target.texture, fmt::format("cs.engine.depth.{}", a_depthStencil),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeDepth(target, a_desc); });
	}

	std::shared_ptr<org::ExternalTextureResource> EngineCubemap(std::uint32_t a_cubemap)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_cubemap >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kTOTAL))
			return nullptr;
		const auto target = renderer->GetRendererData().cubemapRenderTargets[a_cubemap];
		// The faces' RTVs share one format; the graph makes its own views of each face.
		return Import(target.texture, fmt::format("cs.engine.cubemap.{}", a_cubemap),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, target.SRV, target.cubeSideRTV[0], nullptr); });
	}

	std::shared_ptr<org::ExternalTextureResource> D3D11Texture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv,
		ID3D11UnorderedAccessView* a_uav)
	{
		if (a_name.empty())
			return nullptr;
		return Import(a_texture, fmt::format("cs.d3d11.{}", a_name),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, a_srv, nullptr, a_uav); });
	}

	void AddToFrame(std::vector<std::uint32_t>& a_epochs)
	{
		auto& state = Anchors();
		if (!RenderGraphRuntime::EpochsEnabled())
			return;
		if (!state.frozen && SelfTest::Requested())
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				state.reserved[i] = true;
		if (!state.frozen && GraphResources::SelfTest::Requested())
			state.reserved[IndexOf(Anchor::DeferredPasses)] = true;
		state.frozen = true;
		for (std::uint32_t i = 0; i < kAnchors; ++i) {
			if (!state.reserved[i])
				continue;
			const std::uint32_t segment = SegmentOf(static_cast<Anchor>(i));
			const std::uint32_t epoch = RenderGraphRuntime::EpochOf(static_cast<Segment>(segment));
			if (std::ranges::find(a_epochs, epoch) != a_epochs.end())
				continue;
			// Before the first epoch the frame runs after it (the epochs are segment values here).
			const auto after = std::ranges::find_if(a_epochs, [&](std::uint32_t a_epoch) { return RankOf(a_epoch) > RankOf(segment); });
			a_epochs.insert(after, epoch);
		}
	}

	void Attach(org::GraphHost& a_host)
	{
		auto& state = Anchors();
		if (std::ranges::none_of(state.reserved, [](bool a_reserved) { return a_reserved; }))
			return;
		state.host = &a_host;
		Publish(state);
		// Made at every graph build from the snapshot published then; with no graph built yet, adding it rebuilds nothing.
		a_host.AddExtension(kExtensionId, [&state]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
			return std::make_unique<AnchorExtension>(state.snapshot.load(std::memory_order_acquire));
		});
		std::string reserved;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i])
				reserved += fmt::format("{}{} (epoch {})", reserved.empty() ? "" : ", ", kInfo[i].name, EpochOfAnchor(static_cast<Anchor>(i)));
		logger::info("[ORG] frame anchors with epochs: {}{}", reserved, SelfTest::Requested() ? " (CS_ORG_ANCHOR_SELFTEST)" : "");
	}
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: no anchor has an epoch, and every hook does nothing.
#	include "RenderGraph/FrameAnchors.h"

namespace FrameAnchors
{
	const char* Name(Anchor) { return "unknown"; }
	bool Reserve(Anchor) { return false; }
	bool HasEpoch(Anchor) { return false; }
	bool Register(ClientDesc) { return false; }
	void Unregister(std::string_view) {}
	void Refresh(std::string_view) {}
	bool Run(const Point&) { return false; }
	std::shared_ptr<org::ExternalTextureResource> EngineTarget(std::uint32_t) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> EngineDepth(std::uint32_t) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> EngineCubemap(std::uint32_t) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> D3D11Texture(std::string_view, ID3D11Texture2D*, ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*) { return nullptr; }
	std::uint32_t SegmentOf(Anchor) { return 0; }
	bool IsUnusedSegment(std::uint32_t) { return false; }
	const SegmentNames* NamesOf(std::uint32_t) { return nullptr; }
	void AddToFrame(std::vector<std::uint32_t>&) {}
	void Attach(org::GraphHost&) {}
}

#endif
