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
#	include <Resources/Buffers/Buffer.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <cstring>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.terrain-shadows";
	constexpr const char* kShader = "TerrainShadows/ShadowUpdate.cs.hlsl";
	// ShadowUpdate.cs.hlsl's NTHREADS: the pixels of one stripe along the light.
	constexpr std::uint32_t kStripeLength = 128;
	// The most stripes a refresh has: a 16384 texel heightmap, D3D11's largest 2D texture.
	constexpr std::uint32_t kMaxDispatches = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / kStripeLength;

	using Constants = TerrainShadows::ShadowUpdateCB;
	static_assert(sizeof(Constants) == 48, "ShadowUpdateConstants in ShadowUpdate.cs.hlsl is 48 bytes");

	// ShadowUpdate.cs.hlsl's push block (SHADOW_UPDATE_RESOURCES), in list order.
	struct ShadowUpdateBlock
	{
		ShaderBindings::Handle heightMap;
		ShaderBindings::Handle shadowHeights;
		ShaderBindings::Latch constants;
	};

	/** @brief One heightmap's graph textures and its stripe count. Immutable once made: the passes are made from it. */
	struct Heightmap
	{
		std::unique_ptr<GraphResources::Texture2D> heightMap;
		std::unique_ptr<GraphResources::Texture2D> shadowHeights;
		std::uint32_t maxDispatches = 0;
	};
}

struct ORGTerrainShadows::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::function<void()> onFailed;

	// The current heightmap; null until one is set.
	std::shared_ptr<const Heightmap> heightmap;

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

	/** @brief What a pass is made from: the program, latch and gate, and the heightmap. Immutable. */
	struct UpdateSpec
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::shared_ptr<const Heightmap> heightmap;
	};

	struct UpdateFrame
	{
		std::shared_ptr<const UpdateSpec> spec;
	};

	/**
	 * @brief The heightmap's stripes, one gated dispatch a stripe. The commit opens as many entries as this run updates, so the
	 * recording stays the same whatever the run does.
	 */
	class ShadowUpdatePass final : public org::TypedRenderGraphPass<ShadowUpdatePass, UpdateFrame, UpdateBindings>
	{
	public:
		// At the build, on the render thread: the textures as they are now (GraphResources' rule).
		explicit ShadowUpdatePass(std::shared_ptr<const UpdateSpec> a_spec) :
			spec(std::move(a_spec)), heightMap(spec->heightmap->heightMap->Graph()), shadowHeights(spec->heightmap->shadowHeights->Graph()) {}

		UpdateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.ShaderResource(heightMap).View(), a_builder.UnorderedAccess(shadowHeights).View() };
		}

		// The recording depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

		UpdateFrame Prepare(const UpdateBindings&, const org::PassPrepareContext&) const { return { spec }; }

		static void Record(const UpdateBindings& a_bindings, const UpdateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& spec = *a_frame.spec;
			const std::uint32_t slot = a_recording.FrameSlot();
			for (std::uint32_t i = 0; i < spec.heightmap->maxDispatches; ++i) {
				// Each stripe reads the one before it.
				if (i) {
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					a_recording.Commands().Barriers(rhi::BarrierBatch{ {}, {}, { &global, 1 } });
				}
				const ShadowUpdateBlock block{ a_recording.Index(a_bindings.heightMap), a_recording.Index(a_bindings.shadowHeights),
					ShaderBindings::LatchOf(*spec.latch, slot, i * static_cast<std::uint32_t>(sizeof(Constants))) };
				spec.gate->Dispatch(a_recording, *spec.program, ShaderBindings::Data(block), i);
			}
		}

	private:
		std::shared_ptr<const UpdateSpec> spec;
		std::shared_ptr<org::Resource> heightMap;
		std::shared_ptr<org::Resource> shadowHeights;
	};

	/**
	 * @brief The heightmap's mip 0 in a new upload buffer, rows at the copy's pitch. Null, logged, when the buffer cannot be made or
	 * mapped.
	 */
	std::shared_ptr<org::Buffer> StageHeightMap(const DirectX::Image& a_image, rhi::CopyableFootprint& a_footprint)
	{
		const auto format = rhi::helpers::ToRHI(a_image.format);
		const rhi::helpers::SubresourceData source{ a_image.pixels, static_cast<std::uint32_t>(a_image.rowPitch), static_cast<std::uint32_t>(a_image.slicePitch) };
		const auto plan = rhi::helpers::PlanTextureUploadSubresources(format, static_cast<std::uint32_t>(a_image.width), static_cast<std::uint32_t>(a_image.height), 1, 1, 1,
			rhi::Span<const rhi::helpers::SubresourceData>{ &source, 1 });
		if (plan.footprints.size() != 1 || !plan.totalSize) {
			logger::error("[ORG] Terrain Shadows: no copy layout for the heightmap's format {}", static_cast<int>(a_image.format));
			return nullptr;
		}
		const auto& fp = plan.footprints[0];
		if (fp.rowSize > a_image.rowPitch || static_cast<std::size_t>(fp.rows) * a_image.rowPitch > a_image.slicePitch) {
			logger::error("[ORG] Terrain Shadows: the heightmap's rows do not match its format");
			return nullptr;
		}
		auto staging = org::Buffer::CreateShared(rhi::HeapType::Upload, plan.totalSize);
		staging->SetName("TerrainShadows::HeightMapUpload");
		auto resource = staging->GetAPIResource();
		void* mapped = nullptr;
		resource.Map(&mapped);
		if (!mapped) {
			logger::error("[ORG] Terrain Shadows: the heightmap's upload buffer could not be mapped");
			return nullptr;
		}
		auto* destination = static_cast<std::byte*>(mapped) + fp.offset;
		const auto* rows = reinterpret_cast<const std::byte*>(a_image.pixels);
		for (std::uint32_t row = 0; row < fp.rows; ++row)
			std::memcpy(destination + static_cast<std::size_t>(row) * fp.rowPitch, rows + static_cast<std::size_t>(row) * a_image.rowPitch, fp.rowSize);
		resource.Unmap(0, plan.totalSize);
		a_footprint = { fp.offset, fp.rowPitch, fp.width, fp.height, fp.depth };
		return staging;
	}

	/** @brief The client's commit: this frame's constants at the frame slot, and a gate entry for each stripe. */
	void Commit(Impl& a_impl)
	{
		if (a_impl.pending.empty())
			return;
		const std::uint32_t slot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		a_impl.latch->Write(slot, 0, std::as_bytes(std::span(a_impl.pending)));
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
	try {
		next->latch = std::make_shared<org::LatchBlock>("TerrainShadows::UpdateLatch", kMaxDispatches * static_cast<std::uint32_t>(sizeof(Constants)), host->FrameSlots());
	} catch (const std::exception& e) {
		logger::error("[ORG] Terrain Shadows: its latch block could not be created ({}); it stays on D3D11", e.what());
		return false;
	}
	next->gate = RenderGraph::RunGate::Create("TerrainShadows::UpdateGate", kMaxDispatches);
	if (!next->gate) {
		logger::error("[ORG] Terrain Shadows: its run gate could not be created; it stays on D3D11");
		return false;
	}
	next->pending.reserve(kMaxDispatches);
	next->onFailed = std::move(a_onFailed);
	impl = std::move(next);
	logger::info("[ORG] Terrain Shadows: heightmaps go to the render graph");
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
	const auto width = static_cast<UINT>(a_image.width);
	const auto height = static_cast<UINT>(a_image.height);
	auto heightmap = std::make_shared<Heightmap>();
	heightmap->maxDispatches = ((std::max)(width, height) + kStripeLength - 1) / kStripeLength;
	if (heightmap->maxDispatches > kMaxDispatches) {
		logger::error("[ORG] Terrain Shadows: the {}x{} heightmap is larger than a texture can be; back to D3D11", width, height);
		Stop();
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{ .Width = width,
		.Height = height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = a_image.format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE };
	heightmap->heightMap = std::make_unique<GraphResources::Texture2D>(desc, "TerrainShadows::HeightMap",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
	desc.Format = DXGI_FORMAT_R16G16_UNORM;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	// Engine draws read it at t60 (and Drawcall Limit Fix through that view) until they are ported.
	heightmap->shadowHeights = std::make_unique<GraphResources::Texture2D>(desc, std::string(kShadowHeightId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!*heightmap->heightMap || !*heightmap->shadowHeights || !heightmap->shadowHeights->SRV()) {
		logger::error("[ORG] Terrain Shadows: its {}x{} graph textures could not be created; back to D3D11", width, height);
		Stop();
		return false;
	}
	rhi::CopyableFootprint footprint{};
	auto staging = StageHeightMap(a_image, footprint);
	if (!staging) {
		Stop();
		return false;
	}

	auto spec = std::make_shared<const UpdateSpec>(UpdateSpec{ impl->program, impl->latch, impl->gate, heightmap });
	// The upload runs once, at the anchor's next run; the update reads the heightmap after it.
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 90);
	builder.Add(RenderGraph::AnchorPasses::Upload{ .name = "heightmap-upload", .staging = std::move(staging), .target = heightmap->heightMap->Graph(), .footprint = footprint });
	builder.Add("update", FrameAnchors::PassKind::Compute, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass>(spec); });
	if (builder.Failed()) {
		logger::error("[ORG] Terrain Shadows: {}; back to D3D11", builder.Failure());
		Stop();
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes = builder.Take();
	client.registerResources = [heightmap](org::RenderGraph& a_graph) {
		heightmap->heightMap->Register(a_graph);
		heightmap->shadowHeights->Register(a_graph);
	};
	client.commit = [state = impl](const FrameAnchors::Point&) { Commit(*state); };
	client.active = [state = impl] { return !state->pending.empty(); };
	client.gates = { impl->gate };
	client.onFailed = [] {
		logger::error("[ORG] Terrain Shadows: the render graph failed; back to D3D11");
		auto& self = Get();
		PublishedResources::Withdraw(kShadowHeightId);
		auto onFailed = self.impl ? std::move(self.impl->onFailed) : nullptr;
		self.impl.reset();
		if (onFailed)
			onFailed();
	};
	// Registering again replaces the previous heightmap's client: its textures go once D3D11 is past its last use of the old view.
	if (!FrameAnchors::Register(std::move(client))) {
		Stop();
		return false;
	}
	impl->heightmap = heightmap;
	impl->pending.clear();
	// Drawcall Limit Fix's colour passes read it by name, ordered after the update (Integrations/TerrainShadows).
	if (!PublishedResources::Publish(*heightmap->shadowHeights)) {
		logger::error("[ORG] Terrain Shadows: the shadow height map could not be published; back to D3D11");
		Stop();
		return false;
	}
	logger::info("[ORG] Terrain Shadows: {}x{} heightmap on the render graph, up to {} stripes a refresh", width, height, heightmap->maxDispatches);
	return true;
}

bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups)
{
	if (!IsActive() || !impl->heightmap || a_dispatches.empty() || !a_groups)
		return false;
	const auto count = (std::min)(static_cast<std::uint32_t>(a_dispatches.size()), impl->heightmap->maxDispatches);
	impl->pending.assign(a_dispatches.begin(), a_dispatches.begin() + count);
	impl->pendingGroups = a_groups;
	return true;
}

ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const
{
	return IsActive() && impl->heightmap ? impl->heightmap->shadowHeights->SRV() : nullptr;
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
bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB>, std::uint32_t) { return false; }
ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const { return nullptr; }
ORGTerrainShadows::~ORGTerrainShadows() = default;

#endif
