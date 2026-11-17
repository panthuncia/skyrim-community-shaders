#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/TerrainShadows/ORGTerrainShadows.h"

#include "RenderGraph/Switches.h"

bool ORGTerrainShadows::Requested()
{
	static const bool requested = Switches::On("CS_ORG_TERRAIN_SHADOWS");
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

#	include <algorithm>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.terrain-shadows";
	constexpr const char* kShader = "TerrainShadows/ShadowUpdate.cs.hlsl";
	// ShadowUpdate.cs.hlsl's NTHREADS: the pixels of one stripe along the light.
	constexpr std::uint32_t kStripeLength = 128;
	// The heightmap texture's one format. Heightmaps come as BC4 or 16-bit; each is converted to it on the CPU.
	constexpr DXGI_FORMAT kHeightFormat = DXGI_FORMAT_R16_UNORM;

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
	 * @brief The two graph textures, made once at Setup at the largest installed heightmap's size, and the stripe count that size
	 * needs. A heightmap fills their top-left corner.
	 */
	struct Textures
	{
		std::unique_ptr<GraphResources::Texture2D> heightMap;
		std::unique_ptr<GraphResources::Texture2D> shadowHeights;
		std::uint32_t width = 0, height = 0;
		std::uint32_t maxDispatches = 0;
	};
}

struct ORGTerrainShadows::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::shared_ptr<const Textures> textures;
	std::function<void()> onFailed;

	// Render thread: the heightmap staged last, which the one-shot upload pass copies into the heightmap texture's corner.
	std::shared_ptr<const GraphResources::StagedUpload> heightmap;
	bool hasHeightmap = false;

	// Render thread: this frame's constants and groups, until the commit latches them.
	std::vector<Constants> pending;
	std::uint32_t pendingGroups = 0;
};

namespace
{
	using Impl = ORGTerrainShadows::Impl;

	struct UpdateBindings
	{
		org::DeclaredViewToken heightMap, shadowHeights;
	};

	/** @brief What the update pass is made from: the program, latch, gate and textures. Immutable. */
	struct UpdateSpec
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::shared_ptr<const Textures> textures;
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
		// At the build, on the render thread: the textures as they are now (GraphResources' rule).
		explicit ShadowUpdatePass(std::shared_ptr<const UpdateSpec> a_spec) :
			spec(std::move(a_spec)), heightMap(spec->textures->heightMap->Graph()), shadowHeights(spec->textures->shadowHeights->Graph()) {}

		UpdateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.ShaderResource(heightMap).View(), a_builder.UnorderedAccess(shadowHeights).View() };
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
			const std::uint32_t end = Refresh ? spec.textures->maxDispatches : (std::min)(1u, spec.textures->maxDispatches);
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
		std::shared_ptr<org::Resource> heightMap;
		std::shared_ptr<org::Resource> shadowHeights;
	};

	/**
	 * @brief The heightmap's mip 0 as R16_UNORM: BC formats decompressed, other formats converted, R16_UNORM as it is. False,
	 * logged, when DirectXTex cannot do it.
	 */
	bool ToHeightFormat(const DirectX::Image& a_image, DirectX::ScratchImage& a_converted, const DirectX::Image*& a_out)
	{
		if (a_image.format == kHeightFormat) {
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
	 * @brief The client's commit: this frame's constants into the run's latch region, and a gate entry for each stripe.
	 */
	void Commit(Impl& a_impl)
	{
		const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
		if (a_impl.pending.empty())
			return;
		a_impl.latch->Write(run, 0, std::as_bytes(std::span(a_impl.pending)));
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(a_impl.pending.size()); ++i)
			a_impl.gate->OpenDispatch(i, a_impl.pendingGroups);
		a_impl.pending.clear();
	}
}

void ORGTerrainShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Terrain Shadows stays on D3D11: the EarlyPrepass anchor could not be reserved");
}

bool ORGTerrainShadows::Setup(std::uint32_t a_maxWidth, std::uint32_t a_maxHeight, std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	if (!a_maxWidth || !a_maxHeight) {
		logger::info("[ORG] Terrain Shadows: no heightmap is installed; it stays on D3D11");
		return false;
	}
	if (a_maxWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_maxHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
		logger::error("[ORG] Terrain Shadows: a {}x{} heightmap is larger than a texture can be; it stays on D3D11", a_maxWidth, a_maxHeight);
		return false;
	}
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::warn("[ORG] Terrain Shadows: the render graph or its EarlyPrepass anchor is not available; it stays on D3D11");
		return false;
	}
	auto textures = std::make_shared<Textures>();
	textures->width = a_maxWidth;
	textures->height = a_maxHeight;
	textures->maxDispatches = ((std::max)(a_maxWidth, a_maxHeight) + kStripeLength - 1) / kStripeLength;

	auto next = std::make_shared<Impl>();
	next->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<ShadowUpdateBlock>() });
	if (!next->program) {
		logger::error("[ORG] Terrain Shadows: {} could not be compiled; it stays on D3D11", kShader);
		return false;
	}
	try {
		next->latch = std::make_shared<org::LatchBlock>("TerrainShadows::UpdateLatch", textures->maxDispatches * static_cast<std::uint32_t>(sizeof(Constants)));
	} catch (const std::exception& e) {
		logger::error("[ORG] Terrain Shadows: its latch block could not be created ({}); it stays on D3D11", e.what());
		return false;
	}
	next->gate = RenderGraph::RunGate::Create("TerrainShadows::UpdateGate", textures->maxDispatches);
	if (!next->gate) {
		logger::error("[ORG] Terrain Shadows: its run gate could not be created; it stays on D3D11");
		return false;
	}

	// Both textures live for the session: a new worldspace copies its heightmap into their corner, so nothing is replaced and the
	// published id never changes (render-graph.md, "Rebuilds during play").
	D3D11_TEXTURE2D_DESC desc{ .Width = a_maxWidth,
		.Height = a_maxHeight,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = kHeightFormat,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE };
	textures->heightMap = std::make_unique<GraphResources::Texture2D>(desc, "TerrainShadows::HeightMap",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
	desc.Format = DXGI_FORMAT_R16G16_UNORM;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	// Engine draws read it at t60 (and Drawcall Limit Fix through that view) until they are ported.
	textures->shadowHeights = std::make_unique<GraphResources::Texture2D>(desc, std::string(kShadowHeightId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!*textures->heightMap || !*textures->shadowHeights || !textures->shadowHeights->SRV()) {
		logger::error("[ORG] Terrain Shadows: its {}x{} graph textures could not be created; it stays on D3D11", a_maxWidth, a_maxHeight);
		return false;
	}
	next->textures = textures;
	next->pending.reserve(textures->maxDispatches);
	next->onFailed = std::move(a_onFailed);

	auto spec = std::make_shared<const UpdateSpec>(UpdateSpec{ next->program, next->latch, next->gate, textures });
	// Keys 99 (the heightmap copy, a one-shot pass armed again by each new heightmap) and 100 (the update, which reads the
	// heightmap after it). The copy declares its staging buffer and the texture, so the graph orders and tracks it.
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 99);
	builder.Add(
		"heightmap-upload", FrameAnchors::PassKind::Copy,
		[state = std::weak_ptr<Impl>(next)]() -> std::shared_ptr<org::RenderPass> {
			const auto current = state.lock();
			return current ? GraphResources::MakeUploadPass({ current->heightmap }) : nullptr;
		},
		true);
	// Gated: its dispatches take their counts from the client's gate entries, which the commit opens.
	builder.Add(
		"update", FrameAnchors::PassKind::Compute, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass<false>>(spec); },
		false, true);
	// Recorded at execution: it records nothing in a run that is not a refresh.
	if (textures->maxDispatches > 1)
		builder.Add(
			"refresh", FrameAnchors::PassKind::Compute,
			[spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass<true>>(spec); }, false, true, true);
	if (builder.Failed()) {
		logger::error("[ORG] Terrain Shadows: {}; it stays on D3D11", builder.Failure());
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes = builder.Take();
	client.registerResources = [textures](org::RenderGraph& a_graph) {
		textures->heightMap->Register(a_graph);
		textures->shadowHeights->Register(a_graph);
	};
	client.commit = [state = next](const FrameAnchors::Point&) { Commit(*state); };
	client.active = [state = next] { return !state->pending.empty(); };
	client.gates = { next->gate };
	client.onFailed = [] {
		logger::error("[ORG] Terrain Shadows: the render graph failed; back to D3D11");
		auto& self = Get();
		PublishedResources::Withdraw(kShadowHeightId);
		auto onFailed = self.impl ? std::move(self.impl->onFailed) : nullptr;
		self.impl.reset();
		if (onFailed)
			onFailed();
	};
	impl = next;
	if (!FrameAnchors::Register(std::move(client))) {
		impl.reset();
		return false;
	}
	// Drawcall Limit Fix's colour passes read it by name, ordered after the update (Integrations/TerrainShadows). Published once.
	if (!PublishedResources::Publish(*textures->shadowHeights)) {
		logger::error("[ORG] Terrain Shadows: the shadow height map could not be published; it stays on D3D11");
		Stop();
		return false;
	}
	logger::info("[ORG] Terrain Shadows: heightmaps go to the render graph, in {}x{} textures made once (up to {} stripes a refresh)", a_maxWidth,
		a_maxHeight, textures->maxDispatches);
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
	const auto& textures = *impl->textures;
	if (a_image.width > textures.width || a_image.height > textures.height) {
		logger::error("[ORG] Terrain Shadows: the {}x{} heightmap is larger than the {}x{} textures made at setup; back to D3D11", a_image.width,
			a_image.height, textures.width, textures.height);
		Stop();
		return false;
	}
	DirectX::ScratchImage converted;
	const DirectX::Image* image = nullptr;
	if (!ToHeightFormat(a_image, converted, image)) {
		Stop();
		return false;
	}
	// Into the texture's top-left corner, by the one-shot upload pass: armed again, and the graph built again with the new
	// staging (a pass still armed from an earlier heightmap is made again with this one).
	const GraphResources::SubresourceData bytes{ image->pixels, static_cast<UINT>(image->rowPitch), static_cast<UINT>(image->slicePitch) };
	auto staged = textures.heightMap->Stage(std::span<const GraphResources::SubresourceData>(&bytes, 1), static_cast<UINT>(image->width),
		static_cast<UINT>(image->height));
	if (!staged) {
		logger::error("[ORG] Terrain Shadows: the heightmap could not be staged; back to D3D11");
		Stop();
		return false;
	}
	impl->heightmap = std::move(staged);
	FrameAnchors::Rearm(kClientId);
	FrameAnchors::Refresh(kClientId);
	impl->hasHeightmap = true;
	impl->pending.clear();
	logger::info("[ORG] Terrain Shadows: {}x{} heightmap queued for the render graph's {}x{} textures", a_image.width, a_image.height, textures.width,
		textures.height);
	return true;
}

bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups)
{
	if (!IsActive() || !impl->hasHeightmap || a_dispatches.empty() || !a_groups)
		return false;
	const auto count = (std::min)(static_cast<std::uint32_t>(a_dispatches.size()), impl->textures->maxDispatches);
	impl->pending.assign(a_dispatches.begin(), a_dispatches.begin() + count);
	impl->pendingGroups = a_groups;
	return true;
}

ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const
{
	return IsActive() && impl->hasHeightmap ? impl->textures->shadowHeights->SRV() : nullptr;
}

bool ORGTerrainShadows::TextureSize(std::uint32_t& a_width, std::uint32_t& a_height) const
{
	if (!IsActive() || !impl->hasHeightmap)
		return false;
	a_width = impl->textures->width;
	a_height = impl->textures->height;
	return true;
}

ORGTerrainShadows::~ORGTerrainShadows() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Terrain Shadows always runs on D3D11.
struct ORGTerrainShadows::Impl
{};

void ORGTerrainShadows::Reserve() {}
bool ORGTerrainShadows::Setup(std::uint32_t, std::uint32_t, std::function<void()>) { return false; }
bool ORGTerrainShadows::IsActive() const { return false; }
void ORGTerrainShadows::Stop() {}
bool ORGTerrainShadows::SetHeightmap(const DirectX::Image&) { return false; }
bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB>, std::uint32_t) { return false; }
ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const { return nullptr; }
bool ORGTerrainShadows::TextureSize(std::uint32_t&, std::uint32_t&) const { return false; }
ORGTerrainShadows::~ORGTerrainShadows() = default;

#endif
