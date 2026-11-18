#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/CloudShadows/ORGCloudShadows.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Switches.h"

bool ORGCloudShadows::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
	FrameAnchors::Texture lastLayer;
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
		FrameAnchors::Texture from;
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

std::string ORGCloudShadows::PublishedLayerId(int a_layer)
{
	if (a_layer < 0 || a_layer >= CloudShadows::kMaxCloudLayers || !LayerTwins()[a_layer] || !RenderGraphRuntime::Get().IsActive())
		return {};
	return LayerTwins()[a_layer]->Name();
}

FrameAnchors::Texture ORGCloudShadows::ImportLayer(int a_layer)
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
	// The anchors the layer operations run at, each with its own range of the gate's entries: a frame phase may hold two of them in one
	// execution, and each pass then dispatches only for its own anchor's commit.
	constexpr std::array<FrameAnchors::Anchor, 3> kLayerAnchors{ FrameAnchors::Anchor::ReflectionsPrepass, FrameAnchors::Anchor::SkyReflections,
		FrameAnchors::Anchor::EarlyPrepass };
	constexpr std::uint32_t kLayerEntries = 2;  // per anchor and tap: the gather, then the scatter

	/** @brief a_anchor's place in kLayerAnchors (its range of the gate's entries); 0 for another anchor. */
	std::uint32_t LayerSlot(FrameAnchors::Anchor a_anchor)
	{
		for (std::uint32_t i = 0; i < kLayerAnchors.size(); ++i)
			if (kLayerAnchors[i] == a_anchor)
				return i;
		return 0;
	}
	// The scratch array's slots: the most distinct faces one run's copies may read. More, and the queue sends the rest to D3D11.
	constexpr std::uint32_t kScratchSlots = 32;
	// A tap's rows (LayerOpsCS): the gather's entries (one per scratch slot at most), then the scatter's (one per layer face at most).
	constexpr std::uint32_t kGatherWords = kScratchSlots;
	constexpr std::uint32_t kScatterWords = kLayers * kFaces;
	constexpr std::uint32_t kScatterRow = kGatherWords * sizeof(std::uint32_t);
	constexpr std::uint32_t kLatchBytes = (kGatherWords + kScatterWords) * sizeof(std::uint32_t);
	// An entry: the layer in bits 24 to 28, the face in bits 16 to 18, and the scratch slot or (scatter) a clear in the low half.
	constexpr std::uint32_t kNoWord = 0xFFFFFFFFu;
	constexpr std::uint32_t kZeroWord = 0xFFFEu;
	constexpr std::uint32_t Entry(std::uint32_t a_layer, std::uint32_t a_face, std::uint32_t a_value) { return a_layer << 24 | a_face << 16 | a_value; }
	// What a layer face holds once the queued operations are done: untouched, zero, or else the layer whose face (as it was before
	// the run) it is a copy of.
	constexpr std::uint8_t kUntouched = 0xFF;
	constexpr std::uint8_t kZero = 0xFE;
	// LayerOpsCS's layer handles are 24 bits each (the descriptor heap holds about a million).
	constexpr std::uint32_t kHandleBits = 24;
	constexpr std::uint32_t kPackedWords = kLayers * kHandleBits / 32;

	/**
	 * @brief LayerOpsCS's push block (written by hand there): the 32 layers' whole-array UAV handles packed in 24 words, the scratch
	 * (its UAV in the gather, its SRV in the scatter), and the tap's row.
	 */
	struct LayerOpsBlock
	{
		std::array<std::uint32_t, kPackedWords> layers{};
		ShaderBindings::Handle scratch = 0;
		ShaderBindings::Latch row;
	};
	static_assert(kLayers == 32 && kPackedWords == 24, "LayerOpsCS packs 32 layer handles in 24 words");

	/** @brief The layer handles as LayerOpsCS unpacks them: layer i below 24 in word i's low 24 bits, layer 24 + j in the top bytes of words 3j to 3j + 2. */
	void PackLayers(const std::array<std::uint32_t, kLayers>& a_handles, std::array<std::uint32_t, kPackedWords>& a_out)
	{
		for (std::uint32_t i = 0; i < kPackedWords; ++i)
			a_out[i] = a_handles[i] & 0xFFFFFFu;
		for (std::uint32_t j = 0; j < kLayers - kPackedWords; ++j)
			for (std::uint32_t b = 0; b < 3; ++b)
				a_out[3 * j + b] |= ((a_handles[kPackedWords + j] >> (8 * b)) & 0xFFu) << 24;
	}

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
	std::shared_ptr<RenderGraph::RunGate> gate;  // per anchor (LayerSlot): the gather of layer l at l, the scatter into layer l at kLayers + l
	std::shared_ptr<org::LatchBlock> latch;
	std::unique_ptr<GraphResources::Texture2D> scratch;
	std::array<FrameAnchors::Texture, kLayers> layers;
	UINT width = 0;
	UINT height = 0;
	// The taps one anchor's gate entries and latch rows serve in one run: one per reflection face when the start stage holds
	// SkyReflections (its k-th face writes entry set k), else 1. The passes dispatch every set; the sets no tap opened stay closed.
	std::uint32_t instances = 1;
	// The scratch slots earlier face taps of this frame gave out (the gathers of every tap run before the scatters, so the taps of
	// one run take distinct slots), and the frame they were given in.
	std::uint32_t slotsUsed = 0;
	std::uint64_t slotsFrame = 0;

	/** @brief The scratch slots earlier SkyReflections taps of this frame gave out. */
	std::uint32_t SlotsTaken() const
	{
		return instances > 1 && slotsFrame == RenderGraph::Frame::Driver::Current().frame ? slotsUsed : 0;
	}

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
		std::array<org::DeclaredViewToken, kLayers> layers;  // each layer's face array (mip 0)
		org::DeclaredViewToken scratch;
	};

	struct LayerFrame
	{
		std::shared_ptr<const LayerOps> ops;
		bool scatter = false;
		std::uint32_t base = 0;  // the pass's anchor's first gate entry
		std::uint32_t sets = 1;  // the anchor's entry sets: one per tap of a run
	};

	/**
	 * @brief The gather (the faces the run's copies read, into scratch slots) or the scatter (every face the run writes, from its slot
	 * or zero): one dispatch per tap through the gate, a group's z an entry of the tap's row. Every layer is bound by its whole-array
	 * UAV, so a single dispatch reaches the faces of any layer. The recording depends only on what the pass was made with.
	 */
	class LayerOpsPass final : public org::TypedRenderGraphPass<LayerOpsPass, LayerFrame, LayerBindings>
	{
	public:
		LayerOpsPass(std::shared_ptr<const LayerOps> a_ops, bool a_scatter, std::uint32_t a_base, std::uint32_t a_sets = 1) :
			ops(std::move(a_ops)), scatter(a_scatter), base(a_base), sets(a_sets), scratch(ops->scratch->Graph()) {}

		LayerBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// A single cube has a Texture2DArrayFull UAV (its six faces) and no such SRV, so the gather reads a layer through its UAV too.
			const org::SrvView arraySrv{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			const org::UavView arrayUav{ .variant = static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull) };
			LayerBindings bindings;
			for (std::uint32_t l = 0; l < kLayers; ++l)
				bindings.layers[l] = a_builder.UnorderedAccess(ops->layers[l], arrayUav).View();
			bindings.scratch = scatter ? a_builder.ShaderResource(scratch, arraySrv).View() : a_builder.UnorderedAccess(scratch, arrayUav).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(ops.get()));
			a_out.push_back(scatter);
			a_out.push_back(base);
			a_out.push_back(sets);
		}

		LayerFrame Prepare(const LayerBindings&, const org::PassPrepareContext&) const { return { ops, scatter, base, sets }; }

		static void Record(const LayerBindings& a_bindings, const LayerFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& ops = *a_frame.ops;
			const auto& program = a_frame.scatter ? *ops.scatter : *ops.gather;
			std::array<std::uint32_t, kLayers> handles{};
			for (std::uint32_t l = 0; l < kLayers; ++l) {
				handles[l] = a_recording.Index(a_bindings.layers[l]);
				if (handles[l] >> kHandleBits)
					logger::error("[ORG] Cloud Shadows: layer {}'s descriptor index {} does not fit LayerOpsCS's 24 bits", l, handles[l]);
			}
			LayerOpsBlock block{};
			PackLayers(handles, block.layers);
			block.scratch = a_recording.Index(a_bindings.scratch);
			// Each entry set of the anchor (one per tap of a run), in tap order.
			for (std::uint32_t k = 0; k < a_frame.sets; ++k) {
				const std::uint32_t rows = k * kLatchBytes + (a_frame.scatter ? kScatterRow : 0);
				block.row = ShaderBindings::LatchOf(*ops.latch, a_recording.Run(), rows);
				ops.gate->Dispatch(a_recording, program, ShaderBindings::Data(block), a_frame.base + k * kLayerEntries + (a_frame.scatter ? 1 : 0));
			}
		}

	private:
		std::shared_ptr<const LayerOps> ops;
		bool scatter = false;
		std::uint32_t base = 0;
		std::uint32_t sets = 1;
		std::shared_ptr<org::Resource> scratch;
	};

	/**
	 * @brief The client's commit: the queue as the run's rows (a scratch slot for each face read, then each face written: its slot
	 * or zero) and the tap's gather and scatter gate entries, opened for the entries in their rows. The queue is empty afterwards.
	 */
	void CommitLayerOps(LayerOps& a_ops, FrameAnchors::Anchor a_anchor)
	{
		if (a_ops.ops.empty())
			return;
		// The faces read, each with its scratch slot, and the faces written: a slot, or zero.
		std::array<std::uint32_t, kGatherWords> gatherRow;
		std::array<std::uint32_t, kScatterWords> scatterRow;
		gatherRow.fill(kNoWord);
		scatterRow.fill(kNoWord);
		std::uint32_t gatherCount = 0, scatterCount = 0;
		std::array<std::uint32_t, kLayers * kFaces> slotOf;
		slotOf.fill(kNoWord);
		// The tap's entry set and its first scratch slot: a SkyReflections tap of the start stage takes the set of its face's instance and
		// the slots after those the frame's earlier face taps took.
		const bool faces = a_anchor == FrameAnchors::Anchor::SkyReflections && a_ops.instances > 1;
		const std::uint32_t set = faces ? (std::min)(FrameAnchors::TapInstance(), a_ops.instances - 1) : 0;
		const std::uint32_t first = faces && set > 0 ? a_ops.SlotsTaken() : 0;
		std::uint32_t slots = first;
		for (std::uint32_t l = 0; l < kLayers; ++l) {
			for (std::uint32_t f = 0; f < kFaces; ++f) {
				const std::uint8_t value = a_ops.value[l][f];
				if (value == kUntouched)
					continue;
				if (value == kZero) {
					scatterRow[scatterCount++] = Entry(l, f, kZeroWord);
					continue;
				}
				// The face read: its slot, given the first time (the queue counted every face read, so the slots suffice).
				auto& slot = slotOf[value * kFaces + f];
				if (slot == kNoWord) {
					slot = slots++;
					gatherRow[gatherCount++] = Entry(value, f, slot);
				}
				scatterRow[scatterCount++] = Entry(l, f, slot);
			}
		}
		if (faces) {
			a_ops.slotsUsed = slots;
			a_ops.slotsFrame = RenderGraph::Frame::Driver::Current().frame;
		}
		const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
		a_ops.latch->Write(run, set * kLatchBytes, std::as_bytes(std::span(gatherRow)));
		a_ops.latch->Write(run, set * kLatchBytes + kScatterRow, std::as_bytes(std::span(scatterRow)));
		const std::uint32_t groupsX = (a_ops.width + kGroupSize - 1) / kGroupSize;
		const std::uint32_t groupsY = (a_ops.height + kGroupSize - 1) / kGroupSize;
		const std::uint32_t base = (LayerSlot(a_anchor) * a_ops.instances + set) * kLayerEntries;
		if (gatherCount)
			a_ops.gate->OpenDispatch(base, groupsX, groupsY, gatherCount);
		if (scatterCount)
			a_ops.gate->OpenDispatch(base + 1, groupsX, groupsY, scatterCount);
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
		{ .source = kLayerShader, .constantWords = ShaderBindings::Words<LayerOpsBlock>(), .defines = { { L"LAYER_GATHER", L"" } } });
	next->scatter = ComputeProgram::Load(host->Device(), { .source = kLayerShader, .constantWords = ShaderBindings::Words<LayerOpsBlock>() });
	// One entry set per reflection face a frame may draw when the start stage holds the faces' sky.
	next->instances = FrameAnchors::HeldByStage(FrameAnchors::Anchor::SkyReflections) ? kFaces : 1;
	next->gate = RenderGraph::RunGate::Create("CloudShadows::LayerOpsGate", static_cast<std::uint32_t>(kLayerAnchors.size()) * next->instances * kLayerEntries);
	next->latch = std::make_shared<org::LatchBlock>(std::string(kLayersClientId) + ".latch", next->instances * kLatchBytes);
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
		const std::uint32_t base = LayerSlot(anchor) * next->instances * kLayerEntries;
		const std::uint32_t sets = anchor == FrameAnchors::Anchor::SkyReflections ? next->instances : 1;
		for (const bool scatter : { false, true }) {
			client.passes.push_back({ .name = std::format("{}.{}.{}", kLayersClientId, label, scatter ? "scatter" : "gather"),
				.anchor = anchor,
				.order = scatter ? -1 : -2,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [next, scatter, base, sets]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<LayerOpsPass>(next, scatter, base, sets); } });
		}
	}
	if (client.passes.empty())
		return false;
	client.registerResources = [next](org::RenderGraph& a_graph) { next->scratch->Register(a_graph); };
	client.commit = [next](const FrameAnchors::Point& a_point) { CommitLayerOps(*next, a_point.anchor); };
	client.active = [next] { return !next->ops.empty(); };
	client.gates = { next->gate };
	// The start stage: a tap per reflection face, each writing its own entry set and latch rows.
	client.repeats = next->instances > 1;
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
	if (ops->ops.empty() && !FrameAnchors::Scheduled(kLayersClientId, AnchorOf(a_point)))
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
				// The slots this frame's earlier face taps took are not free: their gathers run with this tap's.
				if (ops->sourceCount + (a_point == LayerOpPoint::SkyReflections ? ops->SlotsTaken() : 0) >= kScratchSlots)
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
std::string ORGCloudShadows::PublishedLayerId(int) { return {}; }
FrameAnchors::Texture ORGCloudShadows::ImportLayer(int) { return nullptr; }
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
