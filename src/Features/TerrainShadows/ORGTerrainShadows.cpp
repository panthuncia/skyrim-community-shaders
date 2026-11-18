#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGTerrainShadows::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

ORGTerrainShadows& ORGTerrainShadows::Get()
{
	static ORGTerrainShadows s_shadows;
	return s_shadows;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <DirectXTex.h>
#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/VersionedTexture.h>

#	include <algorithm>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.terrain-shadows";
	constexpr const char* kShader = "TerrainShadows/ShadowUpdate.cs.hlsl";
	// ShadowUpdate.cs.hlsl's NTHREADS: the pixels of one stripe along the light.
	constexpr std::uint32_t kStripeLength = 128;
	// The heightmap formats kept as they are. Any other format is converted to R16_UNORM on the CPU.
	constexpr DXGI_FORMAT kHeightFormat = DXGI_FORMAT_R16_UNORM;
	constexpr DXGI_FORMAT kHeightBlockFormat = DXGI_FORMAT_BC4_UNORM;

	using Constants = TerrainShadows::ShadowUpdateCB;
	static_assert(sizeof(Constants) == 48, "ShadowUpdateConstants in ShadowUpdate.cs.hlsl is 48 bytes");

	// ShadowUpdate.cs.hlsl's push block (SHADOW_UPDATE_RESOURCES), in list order.
	struct ShadowUpdateBlock
	{
		ShaderBindings::Handle heightMap;
		ShaderBindings::Handle shadowHeights;
		ShaderBindings::Latch constants;
	};

	/**
	 * @brief What one heightmap's passes are made from. Immutable. Each heightmap gets its own: the heightmap texture at its size
	 * and format, the latch and gate sized for its stripes, and the shadow height map's slot.
	 */
	struct UpdateSpec
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::shared_ptr<org::Resource> heightMap;
		std::shared_ptr<org::VersionedTexture> shadowHeights;
		std::uint32_t maxDispatches = 0;
	};
}

/**
 * @brief One heightmap's registration of the client: its texture, its staged bytes, its latch and gate, and the stripes waiting for
 * the commit. A new heightmap registers the client again with a new one. The old registration's gate stays closed until its graphs
 * are gone, so its passes do no work in the meantime.
 */
struct ORGTerrainShadows::Heightmap
{
	std::uint32_t width = 0, height = 0;
	int format = 0;  // DXGI_FORMAT, for the log
	std::unique_ptr<GraphResources::Texture2D> texture;
	std::shared_ptr<const GraphResources::StagedUpload> staged;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::shared_ptr<const UpdateSpec> spec;

	// Render thread: this frame's constants and groups, until the commit latches them.
	std::vector<Constants> pending;
	std::uint32_t pendingGroups = 0;
	bool pendingRefresh = false;
	// Render thread: a full refresh was committed in a graph that holds this registration (and so its upload).
	bool refreshed = false;
};

struct ORGTerrainShadows::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	// Made at setup, 1x1, and published once. Each heightmap resizes it to its own size: a new version of the slot, which
	// Drawcall Limit Fix's graphs follow with no build.
	std::unique_ptr<GraphResources::Texture2D> shadowHeights;
	std::function<void()> onFailed;
	std::shared_ptr<Heightmap> current;
	// Render thread: the heightmap staged last, until the next frame boundary switches it in (Switch).
	std::shared_ptr<Heightmap> pending;
};

namespace
{
	using Impl = ORGTerrainShadows::Impl;
	using Heightmap = ORGTerrainShadows::Heightmap;

	struct UpdateBindings
	{
		org::DeclaredViewToken heightMap, shadowHeights;
	};

	struct UpdateFrame
	{
		std::shared_ptr<const UpdateSpec> spec;
	};

	/**
	 * @brief The heightmap's stripes, one gated dispatch a stripe. The commit opens as many entries as this run updates, so the
	 * recording stays the same whatever the run does.
	 *
	 * Two passes: the update (stripe 0, the one a steady frame runs) is recorded ahead. The refresh (stripes 1 and up, which only a
	 * refresh opens) is recorded at execution and skips a run that did not open stripe 1. Each refresh stripe waits for the one
	 * before it with a full barrier, so a steady frame no longer runs those barriers around empty dispatches.
	 */
	template <bool Refresh>
	class ShadowUpdatePass final : public org::TypedRenderGraphPass<ShadowUpdatePass<Refresh>, UpdateFrame, UpdateBindings>
	{
	public:
		explicit ShadowUpdatePass(std::shared_ptr<const UpdateSpec> a_spec) :
			spec(std::move(a_spec)) {}

		UpdateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// The shadow height map's slot: a new worldspace's size is a new version.
			return { a_builder.ShaderResource(spec->heightMap).View(),
				a_builder.UnorderedAccess(org::ResourceResolverAndRange(*spec->shadowHeights)).View() };
		}

		// The recording depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

		UpdateFrame Prepare(const UpdateBindings&, const org::PassPrepareContext&) const { return { spec }; }

		// The refresh's commands depend on the run only through this answer, so a kept late batch keeps its list while it holds.
		static constexpr bool kRecordKeyedBySkip = true;

		// Recorded at execution (the refresh only): a run that did not open stripe 1 is not a refresh.
		static bool SkipsAtExecution(const UpdateBindings&, const UpdateFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return Refresh && !a_frame.spec->gate->IsOpen(a_recording.Run(), 1);
		}

		static void Record(const UpdateBindings& a_bindings, const UpdateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& spec = *a_frame.spec;
			const org::RunRef run = a_recording.Run();
			const std::uint32_t first = Refresh ? 1 : 0;
			const std::uint32_t end = Refresh ? spec.maxDispatches : (std::min)(1u, spec.maxDispatches);
			for (std::uint32_t i = first; i < end; ++i) {
				// Each stripe reads the one before it. The graph orders the refresh's first stripe after the update's.
				if (i > first) {
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					a_recording.Commands().Barriers(rhi::BarrierBatch{ {}, {}, { &global, 1 } });
				}
				const ShadowUpdateBlock block{ a_recording.Index(a_bindings.heightMap), a_recording.Index(a_bindings.shadowHeights),
					ShaderBindings::LatchOf(*spec.latch, run, i * static_cast<std::uint32_t>(sizeof(Constants))) };
				spec.gate->Dispatch(a_recording, *spec.program, ShaderBindings::Data(block), i);
			}
		}

	private:
		std::shared_ptr<const UpdateSpec> spec;
	};

	/**
	 * @brief The heightmap's mip 0 in a format the texture takes: BC4_UNORM and R16_UNORM as they are, any other format converted to
	 * R16_UNORM. False, logged, when DirectXTex cannot convert it.
	 */
	bool ToHeightFormat(const DirectX::Image& a_image, DirectX::ScratchImage& a_converted, const DirectX::Image*& a_out)
	{
		if (a_image.format == kHeightFormat || a_image.format == kHeightBlockFormat) {
			a_out = &a_image;
			return true;
		}
		const HRESULT hr = DirectX::IsCompressed(a_image.format) ?
		                       DirectX::Decompress(a_image, kHeightFormat, a_converted) :
		                       DirectX::Convert(a_image, kHeightFormat, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, a_converted);
		a_out = SUCCEEDED(hr) ? a_converted.GetImage(0, 0, 0) : nullptr;
		if (!a_out) {
			logger::error("[ORG] Terrain Shadows: the heightmap's format {} cannot be converted to R16_UNORM ({:#x})", static_cast<int>(a_image.format),
				static_cast<std::uint32_t>(hr));
			return false;
		}
		return true;
	}

	/**
	 * @brief The client's commit: this frame's constants into the run's latch region, and a gate entry for each stripe. It runs only
	 * in a graph that holds this registration, so a committed refresh covers the heightmap this registration uploads.
	 */
	void Commit(Heightmap& a_heightmap)
	{
		const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
		if (a_heightmap.pending.empty())
			return;
		a_heightmap.latch->Write(run, 0, std::as_bytes(std::span(a_heightmap.pending)));
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(a_heightmap.pending.size()); ++i)
			a_heightmap.gate->OpenDispatch(i, a_heightmap.pendingGroups);
		a_heightmap.refreshed = a_heightmap.refreshed || a_heightmap.pendingRefresh;
		a_heightmap.pending.clear();
	}
}

void ORGTerrainShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Terrain Shadows stays on D3D11: the EarlyPrepass anchor could not be reserved");
}

bool ORGTerrainShadows::Setup(std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::warn("[ORG] Terrain Shadows: the render graph or its EarlyPrepass anchor is not available; it stays on D3D11");
		return false;
	}
	auto next = std::make_shared<Impl>();
	next->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<ShadowUpdateBlock>() });
	if (!next->program) {
		logger::error("[ORG] Terrain Shadows: {} could not be compiled; it stays on D3D11", kShader);
		return false;
	}
	// Engine draws read it at t60 (and Drawcall Limit Fix through that view) until they are ported. Versioned: each heightmap's
	// size is a new version of the same slot.
	const D3D11_TEXTURE2D_DESC desc{ .Width = 1,
		.Height = 1,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16_UNORM,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS };
	next->shadowHeights = std::make_unique<GraphResources::Texture2D>(desc, std::string(kShadowHeightId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV, .versioned = true });
	if (!*next->shadowHeights || !next->shadowHeights->SRV() || !next->shadowHeights->Versioned()) {
		logger::error("[ORG] Terrain Shadows: its shadow height map could not be created; it stays on D3D11");
		return false;
	}
	next->onFailed = std::move(a_onFailed);
	impl = next;
	// Drawcall Limit Fix's colour passes read it by name, ordered after the update (Integrations/TerrainShadows). Published once.
	if (!PublishedResources::Publish(*next->shadowHeights)) {
		logger::error("[ORG] Terrain Shadows: the shadow height map could not be published; it stays on D3D11");
		Stop();
		return false;
	}
	logger::info("[ORG] Terrain Shadows: heightmaps go to the render graph, each in textures of its own size");
	return true;
}

bool ORGTerrainShadows::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

void ORGTerrainShadows::Stop()
{
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kShadowHeightId);
	impl.reset();
}

bool ORGTerrainShadows::SetHeightmap(const DirectX::Image& a_image)
{
	if (!IsActive())
		return false;
	if (a_image.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_image.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
		logger::error("[ORG] Terrain Shadows: a {}x{} heightmap is larger than a texture can be; back to D3D11", a_image.width, a_image.height);
		Stop();
		return false;
	}
	DirectX::ScratchImage converted;
	const DirectX::Image* image = nullptr;
	if (!ToHeightFormat(a_image, converted, image)) {
		Stop();
		return false;
	}
	auto next = std::make_shared<Heightmap>();
	next->width = static_cast<std::uint32_t>(image->width);
	next->height = static_cast<std::uint32_t>(image->height);
	next->format = static_cast<int>(image->format);
	const D3D11_TEXTURE2D_DESC desc{ .Width = next->width,
		.Height = next->height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = image->format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE };
	// Nothing outside the graph reads it: no D3D11 view.
	next->texture = std::make_unique<GraphResources::Texture2D>(desc, "TerrainShadows::HeightMap",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
	if (!*next->texture) {
		logger::error("[ORG] Terrain Shadows: its {}x{} heightmap texture could not be created; back to D3D11", next->width, next->height);
		Stop();
		return false;
	}
	const GraphResources::SubresourceData bytes{ image->pixels, static_cast<UINT>(image->rowPitch), static_cast<UINT>(image->slicePitch) };
	next->staged = next->texture->Stage(std::span<const GraphResources::SubresourceData>(&bytes, 1));
	if (!next->staged) {
		logger::error("[ORG] Terrain Shadows: the heightmap could not be staged; back to D3D11");
		Stop();
		return false;
	}
	// The shadow height map's resize publishes a new version, which only a frame boundary may do: the switch waits for the next
	// one. Until then no stripes are taken, and NeedsRefresh keeps terrain shadows off.
	impl->current.reset();
	impl->pending = next;
	RenderGraph::Frame::Driver::AtNextBegin([state = std::weak_ptr<Impl>(impl), next] {
		if (const auto current = state.lock(); current && current->pending == next)
			Get().Switch();
	});
	logger::info("[ORG] Terrain Shadows: {}x{} heightmap (format {}) staged; it switches in at the next frame boundary", next->width, next->height,
		next->format);
	return true;
}

void ORGTerrainShadows::Switch()
{
	auto next = std::exchange(impl->pending, nullptr);
	const auto fail = [&](std::string_view a_why) {
		logger::error("[ORG] Terrain Shadows: {}; back to D3D11", a_why);
		auto onFailed = std::move(impl->onFailed);
		Stop();
		if (onFailed)
			onFailed();
	};
	// The shadow height map takes the heightmap's size as a new version. The new registration's refresh writes all of it.
	auto& shadowHeights = *impl->shadowHeights;
	shadowHeights.Resize(next->width, next->height);
	if (!shadowHeights || !shadowHeights.SRV() || !shadowHeights.Versioned())
		return fail(fmt::format("the shadow height map could not be resized to {}x{}", next->width, next->height));
	const std::uint32_t maxDispatches = ((std::max)(next->width, next->height) + kStripeLength - 1) / kStripeLength;
	try {
		next->latch = std::make_shared<org::LatchBlock>("TerrainShadows::UpdateLatch", maxDispatches * static_cast<std::uint32_t>(sizeof(Constants)));
	} catch (const std::exception& e) {
		return fail(fmt::format("its latch block could not be created ({})", e.what()));
	}
	next->gate = RenderGraph::RunGate::Create("TerrainShadows::UpdateGate", maxDispatches);
	if (!next->gate)
		return fail("its run gate could not be created");
	next->spec = std::make_shared<const UpdateSpec>(
		UpdateSpec{ impl->program, next->latch, next->gate, next->texture->Graph(), shadowHeights.Versioned(), maxDispatches });
	next->pending.reserve(maxDispatches);

	// Keys 99 (the heightmap copy, a one-shot pass) and 100 (the update, which reads the heightmap after it). The copy declares its
	// staging buffer and the texture, so the graph orders and tracks it.
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 99);
	builder.Add(
		"heightmap-upload", FrameAnchors::PassKind::Copy,
		[staged = next->staged]() -> std::shared_ptr<org::RenderPass> { return GraphResources::MakeUploadPass({ staged }); }, true);
	// Gated: its dispatches take their counts from the registration's gate entries, which the commit opens.
	builder.Add(
		"update", FrameAnchors::PassKind::Compute,
		[spec = next->spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass<false>>(spec); });
	// Recorded at execution: it records nothing in a run that is not a refresh.
	if (maxDispatches > 1)
		builder.Add(
			"refresh", FrameAnchors::PassKind::Compute,
			[spec = next->spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass<true>>(spec); }, false, true);
	if (builder.Failed())
		return fail(builder.Failure());

	// A new registration replaces the last: the EarlyPrepass graphs are built again, staged, with this heightmap's passes.
	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes = builder.Take();
	client.registerResources = [heightmap = next, state = std::weak_ptr<Impl>(impl)](org::RenderGraph& a_graph) {
		heightmap->texture->Register(a_graph);
		if (const auto current = state.lock(); current && current->shadowHeights)
			current->shadowHeights->Register(a_graph);
	};
	client.commit = [heightmap = next](const FrameAnchors::Point&) { Commit(*heightmap); };
	client.active = [heightmap = next] { return !heightmap->pending.empty(); };
	client.gates = { next->gate };
	impl->current = next;
	if (!FrameAnchors::Register(std::move(client)))
		return fail("the client could not be registered");
	logger::info("[ORG] Terrain Shadows: {}x{} heightmap (format {}) registered with the render graph, up to {} stripes a refresh", next->width,
		next->height, next->format, maxDispatches);
}

bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups, bool a_refresh)
{
	if (!IsActive() || !impl->current || a_dispatches.empty() || !a_groups)
		return false;
	auto& heightmap = *impl->current;
	const auto count = (std::min)(static_cast<std::uint32_t>(a_dispatches.size()), heightmap.spec->maxDispatches);
	heightmap.pending.assign(a_dispatches.begin(), a_dispatches.begin() + count);
	heightmap.pendingGroups = a_groups;
	heightmap.pendingRefresh = a_refresh;
	return true;
}

bool ORGTerrainShadows::NeedsRefresh() const
{
	return IsActive() && (impl->pending || (impl->current && !impl->current->refreshed));
}

ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const
{
	return IsActive() && (impl->current || impl->pending) ? impl->shadowHeights->SRV() : nullptr;
}

ORGTerrainShadows::~ORGTerrainShadows() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Terrain Shadows always runs on D3D11.
struct ORGTerrainShadows::Impl
{};

void ORGTerrainShadows::Reserve() {}
bool ORGTerrainShadows::Setup(std::function<void()>) { return false; }
bool ORGTerrainShadows::IsActive() const { return false; }
void ORGTerrainShadows::Stop() {}
bool ORGTerrainShadows::SetHeightmap(const DirectX::Image&) { return false; }
bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB>, std::uint32_t, bool) { return false; }
bool ORGTerrainShadows::NeedsRefresh() const { return false; }
ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const { return nullptr; }
ORGTerrainShadows::~ORGTerrainShadows() = default;

#endif
