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
#	include "RenderGraph/FrameStaging.h"
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

	/** @brief A heightmap's mip 0 in an upload buffer, rows at the copy's pitch. Immutable once made. */
	struct HeightUpload
	{
		std::uint64_t serial = 0;
		std::shared_ptr<org::Buffer> staging;
		rhi::CopyableFootprint footprint{};
	};
}

struct ORGTerrainShadows::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::shared_ptr<const Textures> textures;
	// The upload each run's copy pass records, per host frame slot.
	std::shared_ptr<const RenderGraph::RunSlots<HeightUpload>> uploads;
	std::function<void()> onFailed;

	// Render thread: the heightmap to copy, until a run that stored it reached submission (the client's executed callback).
	std::shared_ptr<const HeightUpload> upload;
	std::uint64_t uploadSerial = 0;
	std::uint64_t storedSerial = 0;
	bool hasHeightmap = false;

	// Render thread: this frame's constants and groups, until the commit latches them.
	std::vector<Constants> pending;
	std::uint32_t pendingGroups = 0;
};

namespace
{
	using Impl = ORGTerrainShadows::Impl;

	struct UploadBindings
	{
		org::ResourceBindingToken heightMap;
	};

	struct UploadFrame
	{
		std::shared_ptr<const RenderGraph::RunSlots<HeightUpload>> uploads;
	};

	/**
	 * @brief The heightmap's bytes copied into the heightmap texture's corner, on the run whose commit stored them. Recorded every
	 * run (no Revision): the slot decides, and a run with no new heightmap records nothing (Bink's upload pass does the same).
	 */
	class HeightUploadPass final : public org::TypedRenderGraphPass<HeightUploadPass, UploadFrame, UploadBindings>
	{
	public:
		HeightUploadPass(std::shared_ptr<org::Resource> a_heightMap, std::shared_ptr<const RenderGraph::RunSlots<HeightUpload>> a_uploads) :
			heightMap(std::move(a_heightMap)), uploads(std::move(a_uploads)) {}

		UploadBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(heightMap) };
		}

		UploadFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { uploads }; }

		static void Record(const UploadBindings& a_bindings, const UploadFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto upload = a_frame.uploads->Take(a_recording.FrameSlot());
			if (!upload || !upload->staging)
				return;
			// The upload buffer stays until this recording's GPU work retires.
			a_recording.Retain(upload);
			rhi::BufferTextureCopyFootprint copy{};
			copy.texture = a_recording.Resolve(a_bindings.heightMap).GetHandle();
			copy.buffer = upload->staging->GetAPIResource().GetHandle();
			copy.footprint = upload->footprint;
			a_recording.Commands().CopyBufferToTexture(copy);
		}

	private:
		std::shared_ptr<org::Resource> heightMap;
		std::shared_ptr<const RenderGraph::RunSlots<HeightUpload>> uploads;
	};

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
	 */
	class ShadowUpdatePass final : public org::TypedRenderGraphPass<ShadowUpdatePass, UpdateFrame, UpdateBindings>
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

		static void Record(const UpdateBindings& a_bindings, const UpdateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& spec = *a_frame.spec;
			const std::uint32_t slot = a_recording.FrameSlot();
			for (std::uint32_t i = 0; i < spec.textures->maxDispatches; ++i) {
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

	/** @brief a_image in a new upload buffer, rows at the copy's pitch. Null, logged, when the buffer cannot be made or mapped. */
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

	/**
	 * @brief The client's commit: a heightmap not yet copied goes to the copy pass at this frame slot; this frame's constants at the
	 * frame slot, and a gate entry for each stripe.
	 */
	void Commit(Impl& a_impl)
	{
		const std::uint32_t slot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		if (a_impl.upload) {
			a_impl.uploads->Store(slot, a_impl.upload);
			a_impl.storedSerial = a_impl.upload->serial;
		}
		if (a_impl.pending.empty())
			return;
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
		next->latch = std::make_shared<org::LatchBlock>("TerrainShadows::UpdateLatch", textures->maxDispatches * static_cast<std::uint32_t>(sizeof(Constants)),
			host->FrameSlots());
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
	next->uploads = std::make_shared<const RenderGraph::RunSlots<HeightUpload>>(host->FrameSlots());
	next->pending.reserve(textures->maxDispatches);
	next->onFailed = std::move(a_onFailed);

	auto spec = std::make_shared<const UpdateSpec>(UpdateSpec{ next->program, next->latch, next->gate, textures });
	// Keys 99 (the heightmap copy, on runs that store one) and 100 (the update, which reads the heightmap after it).
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 99);
	builder.Add("heightmap-upload", FrameAnchors::PassKind::Copy,
		[heightMap = textures->heightMap->Graph(), uploads = next->uploads]() -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<HeightUploadPass>(heightMap, uploads);
		});
	builder.Add("update", FrameAnchors::PassKind::Compute, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass>(spec); });
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
	client.active = [state = next] { return state->upload || !state->pending.empty(); };
	client.executed = [state = next](const FrameAnchors::Point&) {
		// The copy this run stored was submitted: the heightmap is in the texture for every later run.
		if (state->upload && state->upload->serial == state->storedSerial)
			state->upload.reset();
	};
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
	auto upload = std::make_shared<HeightUpload>();
	upload->staging = StageHeightMap(*image, upload->footprint);
	if (!upload->staging) {
		Stop();
		return false;
	}
	upload->serial = ++impl->uploadSerial;
	// A previous heightmap stored at a slot no run recorded yet must not be copied over this one.
	impl->uploads->Clear();
	impl->upload = std::move(upload);
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
