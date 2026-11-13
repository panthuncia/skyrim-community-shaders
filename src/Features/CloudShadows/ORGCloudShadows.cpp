#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/CloudShadows/ORGCloudShadows.h"

#include "RenderGraph/Switches.h"

bool ORGCloudShadows::Requested()
{
	static const bool requested = Switches::On("CS_ORG_CLOUD_SHADOWS");
	return requested;
}

ORGCloudShadows& ORGCloudShadows::Get()
{
	static ORGCloudShadows s_cloudShadows;
	return s_cloudShadows;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Features/CloudShadows.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/GloballyIndexedResource.h>

#	include <algorithm>
#	include <array>
#	include <cstddef>
#	include <format>
#	include <span>
#	include <utility>
#	include <string>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.cloud-shadows";
	constexpr const char* kShader = "CloudShadows/OcclusionCopyCS.hlsl";
	constexpr int kLastLayer = CloudShadows::kMaxCloudLayers - 1;
	constexpr std::uint32_t kFaces = 6;
	// OcclusionCopyCS's numthreads.
	constexpr std::uint32_t kGroupSize = 8;
	// The layer operations' client (ORGCloudShadows.h, "Layer operations").
	constexpr const char* kLayersClientId = "cs.cloud-shadows.layers";

	// OcclusionCopyCS's push block (OCCLUSION_COPY_RESOURCES), in list order.
	struct OcclusionCopyBlock
	{
		std::array<ShaderBindings::Handle, kFaces> faces;
		ShaderBindings::Handle occlusion;
	};
}

struct ORGCloudShadows::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::unique_ptr<GraphResources::Texture2D> occlusion;
	std::shared_ptr<org::Resource> lastLayer;
	UINT width = 0;
	UINT height = 0;
	UINT mips = 0;
	std::function<void()> onFailed;

	// Render thread: ReflectionsPrepass queued this frame's copy; the commit of the next run opens the gate and clears it.
	bool pending = false;

	/** @brief The gate entry of one face's copy of one mip. */
	std::uint32_t Entry(std::uint32_t a_face, std::uint32_t a_mip) const { return a_face * mips + a_mip; }
};

namespace
{
	using Impl = ORGCloudShadows::Impl;

	struct CopyBindings
	{
		std::vector<org::DeclaredViewToken> layer;    // face + mip * 6: one face of one mip of the last layer
		std::vector<org::DeclaredViewToken> targets;  // face + mip * 6: one face of one mip of the occlusion cubemap
	};

	struct CopyFrame
	{
		std::shared_ptr<const Impl> impl;
	};

	/** @brief Each face of the last layer into the occlusion cubemap, every mip, as the D3D11 path's CopyResource. A gated dispatch each. */
	class OcclusionCopyPass final : public org::TypedRenderGraphPass<OcclusionCopyPass, CopyFrame, CopyBindings>
	{
	public:
		// At the build, on the render thread: the textures as they are now (GraphResources' rule).
		explicit OcclusionCopyPass(std::shared_ptr<const Impl> a_impl) :
			impl(std::move(a_impl)), from(impl->lastLayer), to(impl->occlusion->Graph()) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// Single face views of the layer (the import is a cube with views per face and mip) and of the cubemap (the default UAV of
			// a slice and mip is a one-slice array view, so a group's z of 0 writes that face).
			std::vector<org::SrvView> faceViews;
			std::vector<org::UavView> targetViews;
			for (std::uint32_t mip = 0; mip < impl->mips; ++mip) {
				for (std::uint32_t face = 0; face < kFaces; ++face) {
					faceViews.push_back({ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArray), .mip = mip, .slice = face });
					targetViews.push_back({ .mip = mip, .slice = face });
				}
			}
			CopyBindings bindings;
			bindings.layer = a_builder.ShaderResource(from, std::span<const org::SrvView>(faceViews)).views;
			bindings.targets = a_builder.UnorderedAccess(to, std::span<const org::UavView>(targetViews)).views;
			return bindings;
		}

		// The recording depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(impl.get()));
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { impl }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& impl = *a_frame.impl;
			for (std::uint32_t mip = 0; mip < impl.mips; ++mip) {
				for (std::uint32_t face = 0; face < kFaces; ++face) {
					// One face a dispatch (z = 0): the shader reads Face0 and writes the target view's one slice.
					OcclusionCopyBlock block{};
					block.faces.fill(a_recording.Index(a_bindings.layer[mip * kFaces + face]));
					block.occlusion = a_recording.Index(a_bindings.targets[mip * kFaces + face]);
					impl.gate->Dispatch(a_recording, *impl.program, ShaderBindings::Data(block), impl.Entry(face, mip));
				}
			}
		}

	private:
		std::shared_ptr<const Impl> impl;
		std::shared_ptr<org::Resource> from;
		std::shared_ptr<org::Resource> to;
	};

	/** @brief The client's commit: on the run that follows the frame's first reflections prepass, every face and mip copied. */
	void Commit(Impl& a_impl)
	{
		if (!std::exchange(a_impl.pending, false))
			return;
		for (std::uint32_t face = 0; face < kFaces; ++face) {
			for (std::uint32_t mip = 0; mip < a_impl.mips; ++mip) {
				const std::uint32_t width = (std::max)(a_impl.width >> mip, 1u);
				const std::uint32_t height = (std::max)(a_impl.height >> mip, 1u);
				a_impl.gate->OpenDispatch(a_impl.Entry(face, mip), (width + kGroupSize - 1) / kGroupSize, (height + kGroupSize - 1) / kGroupSize, 1);
			}
		}
	}
}

void ORGCloudShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::ReflectionsPrepass))
		logger::warn("[ORG] Cloud Shadows stays on D3D11: the ReflectionsPrepass anchor could not be reserved");
	// The frame's last propagation of the layers (CloudShadows::EarlyPrepass). SkyReflections is the sky family's to reserve.
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Cloud Shadows: the EarlyPrepass anchor could not be reserved; the layers' last propagation stays on D3D11");
}

namespace
{
	/** @brief The layers' twins (ORGCloudShadows::MakeLayer); a null entry is a D3D11 layer. */
	std::array<std::unique_ptr<GraphResources::Texture2D>, CloudShadows::kMaxCloudLayers>& LayerTwins()
	{
		static std::array<std::unique_ptr<GraphResources::Texture2D>, CloudShadows::kMaxCloudLayers> s_twins;
		return s_twins;
	}
}

Texture2D* ORGCloudShadows::MakeLayer(int a_layer, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_srvDesc, const char* a_name)
{
	if (a_layer < 0 || a_layer >= CloudShadows::kMaxCloudLayers || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = LayerTwins()[a_layer];
	if (twin)
		return nullptr;
	// Drawn by the engine's cloud draws (D3D11) and the sky family's, cleared and copied by the layer operations (unordered access)
	// or by D3D11, and sampled by the engine's draws: exported, never aliased. A face keeps its clouds until the next frame draws it.
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = a_desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE;
	desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(a_name),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!*texture || !texture->D3D11() || !std::dynamic_pointer_cast<org::ExternalTextureResource>(texture->Graph())) {
		logger::warn("[ORG] Cloud Shadows: {} could not be made on the render graph; it is a D3D11 texture", a_name);
		return nullptr;
	}
	// The occlusion copy and the sky family's face draws declare it in their graphs: one resource in every graph.
	if (!PublishedResources::Publish(*texture)) {
		logger::warn("[ORG] Cloud Shadows: {} could not be published; it is a D3D11 texture", a_name);
		return nullptr;
	}
	// The wrapper holds a reference of its own: the twin keeps the image, the wrapper the D3D11 texture and its views.
	auto* d3d11 = texture->D3D11();
	d3d11->AddRef();
	auto* wrapper = new Texture2D(d3d11, a_name);
	try {
		wrapper->CreateSRV(a_srvDesc);
	} catch (const std::exception& e) {
		logger::warn("[ORG] Cloud Shadows: {}'s SRV could not be made on the export ({}); it is a D3D11 texture", a_name, e.what());
		delete wrapper;
		return nullptr;
	}
	twin = std::move(texture);
	if (a_layer == 0)
		logger::info("[ORG] Cloud Shadows: the cloud layers are graph textures ({}x{}, {} mips), published by name", desc.Width, desc.Height, desc.MipLevels);
	return wrapper;
}

std::shared_ptr<org::ExternalTextureResource> ORGCloudShadows::ImportLayer(int a_layer)
{
	auto& cloudShadows = globals::features::cloudShadows;
	if (a_layer < 0 || a_layer >= CloudShadows::kMaxCloudLayers)
		return nullptr;
	if (const auto& twin = LayerTwins()[a_layer]; twin && RenderGraphRuntime::Get().IsActive())
		return std::dynamic_pointer_cast<org::ExternalTextureResource>(twin->Graph());
	auto* layer = cloudShadows.texCloudShadowLayers[a_layer];
	if (!layer || !layer->resource || !layer->srv || !cloudShadows.cloudShadowLayerRTVs[a_layer][0])
		return nullptr;
	// A D3D11 layer (its twin could not be made): imported. Each lives as long as the feature.
	return FrameAnchors::D3D11Texture(fmt::format("CloudShadows::Layer[{}]", a_layer), layer->resource.get(), layer->srv.get(), nullptr,
		cloudShadows.cloudShadowLayerRTVs[a_layer][0]);
}

bool ORGCloudShadows::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

ID3D11ShaderResourceView* ORGCloudShadows::OcclusionSRV() const
{
	return IsActive() ? impl->occlusion->SRV() : nullptr;
}

void ORGCloudShadows::Stop()
{
	FrameAnchors::Unregister(kLayersClientId);
	layerOps.reset();
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kOcclusionId);
	impl.reset();
}

bool ORGCloudShadows::Setup(std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::ReflectionsPrepass)) {
		logger::warn("[ORG] Cloud Shadows stays on D3D11: the render graph or its ReflectionsPrepass anchor is not available");
		return false;
	}
	auto& cloudShadows = globals::features::cloudShadows;
	auto* lastLayer = cloudShadows.texCloudShadowLayers[kLastLayer];
	if (!lastLayer || !lastLayer->srv) {
		logger::error("[ORG] Cloud Shadows: its layer textures are missing; it stays on D3D11");
		return false;
	}

	D3D11_TEXTURE2D_DESC layerDesc{};
	lastLayer->resource->GetDesc(&layerDesc);
	auto next = std::make_shared<Impl>();
	next->width = layerDesc.Width;
	next->height = layerDesc.Height;
	next->mips = layerDesc.MipLevels;
	next->onFailed = std::move(a_onFailed);

	next->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<OcclusionCopyBlock>() });
	if (!next->program) {
		logger::error("[ORG] Cloud Shadows: {} could not be compiled; it stays on D3D11", kShader);
		return false;
	}
	next->gate = RenderGraph::RunGate::Create("CloudShadows::OcclusionCopyGate", kFaces * next->mips);
	if (!next->gate) {
		logger::error("[ORG] Cloud Shadows: its run gate could not be created; it stays on D3D11");
		return false;
	}

	// The last layer's shape (the reflections cubemap's, in R8), written by the copy's dispatches and sampled.
	const D3D11_TEXTURE2D_DESC desc{ .Width = layerDesc.Width,
		.Height = layerDesc.Height,
		.MipLevels = layerDesc.MipLevels,
		.ArraySize = layerDesc.ArraySize,
		.Format = layerDesc.Format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		.MiscFlags = layerDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE };
	// Engine draws read it at t25 and t26 (and Drawcall Limit Fix through that view) until they are ported. A frame without the
	// full sky keeps the last copy, so it lives across frames.
	next->occlusion = std::make_unique<GraphResources::Texture2D>(desc, std::string(kOcclusionId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
	if (!*next->occlusion || !next->occlusion->SRV()) {
		logger::error("[ORG] Cloud Shadows: the {}x{} occlusion cubemap could not be created; it stays on D3D11", desc.Width, desc.Height);
		return false;
	}
	next->lastLayer = ImportLayer(kLastLayer);
	if (!next->lastLayer) {
		logger::error("[ORG] Cloud Shadows: the last cloud layer could not be imported; it stays on D3D11");
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes.push_back({ .name = std::string(kClientId) + ".occlusion-copy",
		.anchor = FrameAnchors::Anchor::ReflectionsPrepass,
		.order = 0,
		.kind = FrameAnchors::PassKind::Compute,
		.make = [next]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<OcclusionCopyPass>(next); } });
	client.registerResources = [next](org::RenderGraph& a_graph) { next->occlusion->Register(a_graph); };
	client.commit = [next](const FrameAnchors::Point&) { Commit(*next); };
	// The anchor runs at every cubemap render; only the run after the frame's first reflections prepass copies, as on D3D11.
	client.active = [next] { return next->pending; };
	client.gates = { next->gate };
	client.onFailed = [] {
		logger::error("[ORG] Cloud Shadows: the render graph failed; back to D3D11");
		auto& self = Get();
		PublishedResources::Withdraw(kOcclusionId);
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
	// Drawcall Limit Fix's colour passes read it by name, ordered after the copy (Integrations/CloudShadows).
	if (!PublishedResources::Publish(*next->occlusion)) {
		logger::error("[ORG] Cloud Shadows: the occlusion cubemap could not be published; it stays on D3D11");
		Stop();
		return false;
	}
	logger::info("[ORG] Cloud Shadows: the occlusion copy runs on the render graph ({}x{}, {} mips)", desc.Width, desc.Height, desc.MipLevels);
	if (!SetupLayerOps())
		logger::warn("[ORG] Cloud Shadows: the layers' clears and copies stay on D3D11");
	return true;
}

bool ORGCloudShadows::QueueOcclusionCopy()
{
	if (!IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::ReflectionsPrepass))
		return false;
	impl->pending = true;
	return true;
}

// Layer operations (ORGCloudShadows.h).

namespace
{
	constexpr const char* kLayerShader = "CloudShadows/LayerOpsCS.hlsl";
	constexpr std::uint32_t kLayers = CloudShadows::kMaxCloudLayers;
	// The scratch array's slots: the most distinct faces one run's copies may read. More, and the queue sends the rest to D3D11.
	constexpr std::uint32_t kScratchSlots = 32;
	// LayerOpsCS's LayerRow: eight words, one per face (six used). The gather's rows, then the scatter's, one per layer.
	constexpr std::uint32_t kRowWords = 8;
	constexpr std::uint32_t kRowBytes = kRowWords * sizeof(std::uint32_t);
	constexpr std::uint32_t kScatterRows = kLayers * kRowBytes;
	constexpr std::uint32_t kLatchBytes = 2 * kScatterRows;
	// A row word: nothing for this face, or (scatter) a clear; else the scratch slot.
	constexpr std::uint32_t kNoWord = 0xFFFFFFFFu;
	constexpr std::uint32_t kZeroWord = 0xFFFFFFFEu;
	// What a layer face holds once the queued operations are done: untouched, zero, or else the layer whose face (as it was before
	// the run) it is a copy of.
	constexpr std::uint8_t kUntouched = 0xFF;
	constexpr std::uint8_t kZero = 0xFE;

	/** @brief LayerOpsCS's gather push block (LAYER_OPS_RESOURCES with LAYER_GATHER): the layer's six faces, the scratch, the row. */
	struct LayerGatherBlock
	{
		std::array<ShaderBindings::Handle, kFaces> faces;
		ShaderBindings::Handle scratch;
		ShaderBindings::Latch row;
	};

	/** @brief LayerOpsCS's scatter push block: the scratch, the layer's face array, the row. */
	struct LayerScatterBlock
	{
		ShaderBindings::Handle scratch, layer;
		ShaderBindings::Latch row;
	};

	FrameAnchors::Anchor AnchorOf(ORGCloudShadows::LayerOpPoint a_point)
	{
		using Point = ORGCloudShadows::LayerOpPoint;
		switch (a_point) {
		case Point::ReflectionsPrepass:
			return FrameAnchors::Anchor::ReflectionsPrepass;
		case Point::SkyReflections:
			return FrameAnchors::Anchor::SkyReflections;
		case Point::EarlyPrepass:
			return FrameAnchors::Anchor::EarlyPrepass;
		default:
			return FrameAnchors::Anchor::Count;
		}
	}
}

struct ORGCloudShadows::LayerOps
{
	// Made at setup, then only read (the passes read them on the graph host's thread).
	std::shared_ptr<const ComputeProgram> gather, scatter;
	std::shared_ptr<RenderGraph::RunGate> gate;  // entries 0-31: the gather of layer l; 32-63: the scatter into layer l
	std::shared_ptr<org::LatchBlock> latch;
	std::unique_ptr<GraphResources::Texture2D> scratch;
	std::array<std::shared_ptr<org::Resource>, kLayers> layers;
	UINT width = 0;
	UINT height = 0;

	// Render thread: the queue since the last run took it.
	std::vector<LayerOp> ops;
	LayerOpPoint point = LayerOpPoint::D3D11;
	std::array<std::array<std::uint8_t, kFaces>, kLayers> value{};
	std::array<std::uint32_t, kFaces> sources{};  // per face, a bit per layer whose face the copies read
	std::uint32_t sourceCount = 0;
	bool failed = false;

	void Reset()
	{
		ops.clear();
		point = LayerOpPoint::D3D11;
		for (auto& faces : value)
			faces.fill(kUntouched);
		sources.fill(0);
		sourceCount = 0;
	}
};

namespace
{
	using LayerOps = ORGCloudShadows::LayerOps;

	struct LayerBindings
	{
		std::array<org::DeclaredViewToken, kLayers> layers;                       // scatter: each layer's face array (mip 0)
		std::array<std::array<org::DeclaredViewToken, kFaces>, kLayers> faces{};  // gather: each layer's faces, one view each (mip 0)
		org::DeclaredViewToken scratch;
	};

	struct LayerFrame
	{
		std::shared_ptr<const LayerOps> ops;
		bool scatter = false;
	};

	/**
	 * @brief The gather (the faces the run's copies read, into scratch slots) or the scatter (every face the run writes, from its slot
	 * or zero): one dispatch per layer through the gate, a group's z its face. The recording depends only on what the pass was made with.
	 */
	class LayerOpsPass final : public org::TypedRenderGraphPass<LayerOpsPass, LayerFrame, LayerBindings>
	{
	public:
		LayerOpsPass(std::shared_ptr<const LayerOps> a_ops, bool a_scatter) :
			ops(std::move(a_ops)), scatter(a_scatter), scratch(ops->scratch->Graph()) {}

		LayerBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// The views ORG makes of a single cube: TextureCube, one Texture2DArray view per face (as the occlusion copy reads), and a
			// Texture2DArrayFull UAV; no Texture2DArrayFull SRV, which only a 2D array has (the scratch). So the gather reads a layer's
			// faces one view each, and the scatter writes its faces through the whole-array UAV.
			const org::SrvView arraySrv{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			const org::UavView arrayUav{ .variant = static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull) };
			std::array<org::SrvView, kFaces> faceViews;
			for (std::uint32_t f = 0; f < kFaces; ++f)
				faceViews[f] = { .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArray), .mip = 0, .slice = f };
			LayerBindings bindings;
			for (std::uint32_t l = 0; l < kLayers; ++l) {
				if (scatter) {
					bindings.layers[l] = a_builder.UnorderedAccess(ops->layers[l], arrayUav).View();
					continue;
				}
				const auto views = a_builder.ShaderResource(ops->layers[l], std::span<const org::SrvView>(faceViews)).views;
				std::copy_n(views.begin(), kFaces, bindings.faces[l].begin());
			}
			bindings.scratch = scatter ? a_builder.ShaderResource(scratch, arraySrv).View() : a_builder.UnorderedAccess(scratch, arrayUav).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(ops.get()));
			a_out.push_back(scatter);
		}

		LayerFrame Prepare(const LayerBindings&, const org::PassPrepareContext&) const { return { ops, scatter }; }

		static void Record(const LayerBindings& a_bindings, const LayerFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& ops = *a_frame.ops;
			const auto& program = a_frame.scatter ? *ops.scatter : *ops.gather;
			const std::uint32_t scratch = a_recording.Index(a_bindings.scratch);
			for (std::uint32_t l = 0; l < kLayers; ++l) {
				if (a_frame.scatter) {
					const LayerScatterBlock block{ .scratch = scratch,
						.layer = a_recording.Index(a_bindings.layers[l]),
						.row = ShaderBindings::LatchOf(*ops.latch, a_recording.Run(), kScatterRows + l * kRowBytes) };
					ops.gate->Dispatch(a_recording, program, ShaderBindings::Data(block), kLayers + l);
					continue;
				}
				LayerGatherBlock block{};
				for (std::uint32_t f = 0; f < kFaces; ++f)
					block.faces[f] = a_recording.Index(a_bindings.faces[l][f]);
				block.scratch = scratch;
				block.row = ShaderBindings::LatchOf(*ops.latch, a_recording.Run(), l * kRowBytes);
				ops.gate->Dispatch(a_recording, program, ShaderBindings::Data(block), l);
			}
		}

	private:
		std::shared_ptr<const LayerOps> ops;
		bool scatter = false;
		std::shared_ptr<org::Resource> scratch;
	};

	/**
	 * @brief The client's commit: the queue as the run's rows (a scratch slot for each face read, then each face written: its slot
	 * or zero) and the gate entries of the layers with work, opened for every face. The queue is empty afterwards.
	 */
	void CommitLayerOps(LayerOps& a_ops)
	{
		if (a_ops.ops.empty())
			return;
		std::array<std::uint32_t, kLayers * kRowWords> gatherRows;
		std::array<std::uint32_t, kLayers * kRowWords> scatterRows;
		gatherRows.fill(kNoWord);
		scatterRows.fill(kNoWord);
		std::uint32_t gatherLayers = 0, scatterLayers = 0, slots = 0;
		for (std::uint32_t l = 0; l < kLayers; ++l) {
			for (std::uint32_t f = 0; f < kFaces; ++f) {
				const std::uint8_t value = a_ops.value[l][f];
				if (value == kUntouched)
					continue;
				scatterLayers |= 1u << l;
				if (value == kZero) {
					scatterRows[l * kRowWords + f] = kZeroWord;
					continue;
				}
				// The face read: its slot, given the first time (the queue counted every face read, so the slots suffice).
				auto& slot = gatherRows[value * kRowWords + f];
				if (slot == kNoWord) {
					slot = slots++;
					gatherLayers |= 1u << value;
				}
				scatterRows[l * kRowWords + f] = slot;
			}
		}
		const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
		a_ops.latch->Write(run, 0, std::as_bytes(std::span(gatherRows)));
		a_ops.latch->Write(run, kScatterRows, std::as_bytes(std::span(scatterRows)));
		const std::uint32_t groupsX = (a_ops.width + kGroupSize - 1) / kGroupSize;
		const std::uint32_t groupsY = (a_ops.height + kGroupSize - 1) / kGroupSize;
		for (std::uint32_t l = 0; l < kLayers; ++l) {
			if (gatherLayers & (1u << l))
				a_ops.gate->OpenDispatch(l, groupsX, groupsY, kFaces);
			if (scatterLayers & (1u << l))
				a_ops.gate->OpenDispatch(kLayers + l, groupsX, groupsY, kFaces);
		}
		a_ops.Reset();
	}
}

bool ORGCloudShadows::SetupLayerOps()
{
	layerOps.reset();
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host)
		return false;
	auto next = std::make_shared<LayerOps>();
	next->Reset();
	// Every layer a twin: the scatter writes them through unordered access, which a D3D11 layer's import has not.
	for (std::uint32_t l = 0; l < kLayers; ++l) {
		if (!LayerTwins()[l]) {
			logger::warn("[ORG] Cloud Shadows: layer {} is a D3D11 texture, so the layer operations stay on D3D11", l);
			return false;
		}
		next->layers[l] = ImportLayer(static_cast<int>(l));
		if (!next->layers[l])
			return false;
	}
	D3D11_TEXTURE2D_DESC layerDesc{};
	globals::features::cloudShadows.texCloudShadowLayers[0]->resource->GetDesc(&layerDesc);
	next->width = layerDesc.Width;
	next->height = layerDesc.Height;

	next->gather = ComputeProgram::Load(host->Device(),
		{ .source = kLayerShader, .constantWords = ShaderBindings::Words<LayerGatherBlock>(), .defines = { { L"LAYER_GATHER", L"" } } });
	next->scatter = ComputeProgram::Load(host->Device(), { .source = kLayerShader, .constantWords = ShaderBindings::Words<LayerScatterBlock>() });
	next->gate = RenderGraph::RunGate::Create("CloudShadows::LayerOpsGate", 2 * kLayers);
	next->latch = std::make_shared<org::LatchBlock>(std::string(kLayersClientId) + ".latch", kLatchBytes);
	if (!next->gather || !next->scatter || !next->gate) {
		logger::error("[ORG] Cloud Shadows: {} or its run gate could not be made", kLayerShader);
		return false;
	}
	// The faces one run's copies read, between its gather and its scatter. Every run writes the slots it reads.
	const D3D11_TEXTURE2D_DESC scratchDesc{ .Width = layerDesc.Width,
		.Height = layerDesc.Height,
		.MipLevels = 1,
		.ArraySize = kScratchSlots,
		.Format = layerDesc.Format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS };
	next->scratch = std::make_unique<GraphResources::Texture2D>(scratchDesc, "CloudShadows::LayerScratch",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
	if (!*next->scratch) {
		logger::error("[ORG] Cloud Shadows: the layers' {}x{} scratch array could not be made", scratchDesc.Width, scratchDesc.Height);
		return false;
	}

	// The gather and the scatter before everything else at each anchor that follows a layer operation: the occlusion copy (0) at
	// ReflectionsPrepass, the sky family's face draws (0 and 1) at SkyReflections. Where an anchor has no epoch (SkyReflections
	// without the sky family), its operations stay on D3D11.
	FrameAnchors::ClientDesc client{};
	client.id = kLayersClientId;
	for (const auto [anchor, label] : { std::pair{ FrameAnchors::Anchor::ReflectionsPrepass, "reflections-prepass" },
			 std::pair{ FrameAnchors::Anchor::SkyReflections, "sky-reflections" }, std::pair{ FrameAnchors::Anchor::EarlyPrepass, "early-prepass" } }) {
		if (!FrameAnchors::HasEpoch(anchor))
			continue;
		for (const bool scatter : { false, true }) {
			client.passes.push_back({ .name = std::format("{}.{}.{}", kLayersClientId, label, scatter ? "scatter" : "gather"),
				.anchor = anchor,
				.order = scatter ? -1 : -2,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [next, scatter]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<LayerOpsPass>(next, scatter); } });
		}
	}
	if (client.passes.empty())
		return false;
	client.registerResources = [next](org::RenderGraph& a_graph) { next->scratch->Register(a_graph); };
	client.commit = [next](const FrameAnchors::Point&) { CommitLayerOps(*next); };
	client.active = [next] { return !next->ops.empty(); };
	client.gates = { next->gate };
	client.onFailed = [next] {
		logger::error("[ORG] Cloud Shadows: the render graph failed with the layer operations; they return to D3D11");
		next->failed = true;
		auto& self = Get();
		auto ops = self.TakeLayerOps();
		self.layerOps.reset();
		globals::features::cloudShadows.ApplyLayerOps(ops);
	};
	layerOps = next;
	if (!FrameAnchors::Register(std::move(client))) {
		layerOps.reset();
		return false;
	}
	logger::info("[ORG] Cloud Shadows: the layers' clears and copies run on the render graph ({} scratch slots{})", kScratchSlots,
		FrameAnchors::HasEpoch(FrameAnchors::Anchor::SkyReflections) ? "" : "; SkyReflections has no epoch, so a face's run stays on D3D11");
	return true;
}

bool ORGCloudShadows::QueueLayerOp(LayerOpPoint a_point, LayerOp a_op)
{
	auto* ops = layerOps.get();
	if (!ops || ops->failed || a_point == LayerOpPoint::D3D11 || !RenderGraphRuntime::Get().IsActive())
		return false;
	if (a_op.dst >= kLayers || a_op.face >= kFaces || (a_op.src != kClearOp && a_op.src >= kLayers))
		return false;
	// Queued for another anchor whose run has not come: the caller sends those to D3D11 first.
	if (!ops->ops.empty() && ops->point != a_point)
		return false;
	if (ops->ops.empty() && !FrameAnchors::CanRun(kLayersClientId, AnchorOf(a_point)))
		return false;
	auto& target = ops->value[a_op.dst][a_op.face];
	if (a_op.src == kClearOp) {
		target = kZero;
	} else {
		// What the source face holds now: zero, a face as it was before the run, or itself.
		std::uint8_t value = ops->value[a_op.src][a_op.face];
		if (value == kUntouched)
			value = a_op.src;
		if (value == a_op.dst) {
			target = kUntouched;  // the face's own contents again (or a copy onto itself)
		} else {
			if (value != kZero && !(ops->sources[a_op.face] & (1u << value))) {
				if (ops->sourceCount == kScratchSlots)
					return false;
				ops->sources[a_op.face] |= 1u << value;
				++ops->sourceCount;
			}
			target = value;
		}
	}
	ops->point = a_point;
	ops->ops.push_back(a_op);
	return true;
}

std::vector<ORGCloudShadows::LayerOp> ORGCloudShadows::TakeLayerOps()
{
	auto* ops = layerOps.get();
	if (!ops || ops->ops.empty())
		return {};
	auto taken = std::move(ops->ops);
	ops->Reset();
	return taken;
}

ORGCloudShadows::~ORGCloudShadows() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Cloud Shadows always copies on D3D11.
struct ORGCloudShadows::Impl
{};
struct ORGCloudShadows::LayerOps
{};

void ORGCloudShadows::Reserve() {}
std::shared_ptr<org::ExternalTextureResource> ORGCloudShadows::ImportLayer(int) { return nullptr; }
Texture2D* ORGCloudShadows::MakeLayer(int, const D3D11_TEXTURE2D_DESC&, const D3D11_SHADER_RESOURCE_VIEW_DESC&, const char*) { return nullptr; }
bool ORGCloudShadows::Setup(std::function<void()>) { return false; }
bool ORGCloudShadows::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGCloudShadows::OcclusionSRV() const { return nullptr; }
void ORGCloudShadows::Stop() {}
bool ORGCloudShadows::QueueOcclusionCopy() { return false; }
bool ORGCloudShadows::SetupLayerOps() { return false; }
bool ORGCloudShadows::QueueLayerOp(LayerOpPoint, LayerOp) { return false; }
std::vector<ORGCloudShadows::LayerOp> ORGCloudShadows::TakeLayerOps() { return {}; }
ORGCloudShadows::~ORGCloudShadows() = default;

#endif
