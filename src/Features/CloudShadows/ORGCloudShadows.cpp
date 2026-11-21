// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/CloudShadows/ORGCloudShadows.h"

#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Switches.h"

#include "Features/CloudShadows.h"
#include "Globals.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/D3D11Wrapper.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/GloballyIndexedResource.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <string>
#include <vector>

namespace
{
	constexpr const char* kClientId = "cs.cloud-shadows";
	constexpr const char* kShader = "CloudShadows/OcclusionCopyCS.hlsl";
	constexpr int kLastLayer = CloudShadows::kMaxCloudLayers - 1;
	constexpr std::uint32_t kFaces = 6;
	// OcclusionCopyCS's numthreads.
	constexpr std::uint32_t kGroupSize = 8;
	constexpr const char* kLayersClientId = "cs.cloud-shadows.layers";

	// Push block in list order.
	struct OcclusionCopyBlock
	{
		std::array<ShaderBindings::Handle, kFaces> faces;
		ShaderBindings::Handle occlusion;
	};
}

struct ORGCloudShadows::Impl
{
	RenderGraph::Programs::Fixed<ComputeProgram> program;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::unique_ptr<GraphResources::Texture2D> occlusion;
	FrameAnchors::Texture lastLayer;
	UINT width = 0;
	UINT height = 0;
	UINT mips = 0;

	// Render thread. Set by ReflectionsPrepass, cleared by the next commit.
	bool pending = false;

	std::uint32_t Entry(std::uint32_t a_face, std::uint32_t a_mip) const { return a_face * mips + a_mip; }
};

namespace
{
	using Impl = ORGCloudShadows::Impl;

	struct CopyBindings
	{
		std::vector<org::DeclaredViewToken> layer;    // face + mip * 6
		std::vector<org::DeclaredViewToken> targets;  // face + mip * 6
	};

	struct CopyFrame
	{
		std::shared_ptr<const Impl> impl;
	};

	/** @brief As the D3D11 path's CopyResource, with one gated dispatch per face and mip. */
	class OcclusionCopyPass final : public org::TypedRenderGraphPass<OcclusionCopyPass, CopyFrame, CopyBindings>
	{
	public:
		// At the build, on the render thread, so it sees the textures as they are now.
		explicit OcclusionCopyPass(std::shared_ptr<const Impl> a_impl) :
			impl(std::move(a_impl)), from(impl->lastLayer), to(impl->occlusion->Graph()) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// Single face views. The default UAV of a slice and mip is a one-slice array, so z of 0 writes that face.
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
					OcclusionCopyBlock block{};
					block.faces.fill(a_recording.Index(a_bindings.layer[mip * kFaces + face]));
					block.occlusion = a_recording.Index(a_bindings.targets[mip * kFaces + face]);
					impl.gate->Dispatch(a_recording, *impl.program.Get(), ShaderBindings::Data(block), impl.Entry(face, mip));
				}
			}
		}

	private:
		std::shared_ptr<const Impl> impl;
		FrameAnchors::Texture from;
		std::shared_ptr<org::Resource> to;
	};


	/**
	* @brief The occlusion copy's client, declared at Setup on the reflection cubemap. Its key is the cubemap's edge. Kept here
	* and not in Impl, since its functions hold the Impl.
	*/
	RenderGraph::Frame::DeclaredClient& CopyClient()
	{
		static RenderGraph::Frame::DeclaredClient s_client;
		return s_client;
	}

	/** @brief The layer operations' declared client. Its registration never changes, so its key is constant. */
	RenderGraph::Frame::DeclaredClient& LayersClient()
	{
		static RenderGraph::Frame::DeclaredClient s_client;
		return s_client;
	}

	/** @brief Copies every face and mip on the run after the frame's first reflections prepass. */
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

namespace
{
	/** @brief In graph mode every layer is a twin. A null entry is a layer not made yet. */
	std::array<std::unique_ptr<GraphResources::Texture2D>, CloudShadows::kMaxCloudLayers>& LayerTwins()
	{
		static std::array<std::unique_ptr<GraphResources::Texture2D>, CloudShadows::kMaxCloudLayers> s_twins;
		return s_twins;
	}

	/** @brief The layers' D3D11 wrappers' registrations, kept with their twins. */
	std::array<GraphResources::D3D11Wrapper<::Texture2D>, CloudShadows::kMaxCloudLayers>& LayerWrappers()
	{
		static std::array<GraphResources::D3D11Wrapper<::Texture2D>, CloudShadows::kMaxCloudLayers> s_wrappers;
		return s_wrappers;
	}
}

Texture2D* ORGCloudShadows::MakeLayer(int a_layer, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SHADER_RESOURCE_VIEW_DESC& a_srvDesc, const char* a_name)
{
	if (a_layer < 0 || a_layer >= CloudShadows::kMaxCloudLayers || !On())
		return nullptr;
	auto& twin = LayerTwins()[a_layer];
	// Made again, the layer stays the twin it is.
	if (twin)
		return LayerWrappers()[a_layer].Get();
	// Drawn by D3D11 and the graph, written by layer ops, and sampled by engine draws, so exported and never aliased.
	// A face keeps its clouds until the next frame draws it.
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = a_desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE;
	desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(a_name),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!texture->D3D11() || !std::dynamic_pointer_cast<org::ExternalTextureResource>(texture->Graph()))
		Fatal(fmt::format("{} could not be made on the render graph", a_name));
	// Published so every graph that declares it gets one resource.
	if (!PublishedResources::Publish(*texture))
		Fatal(fmt::format("{} could not be published", a_name));
	// The twin keeps the image, the wrapper keeps the D3D11 texture and its own SRV. A failed view is fatal there.
	GraphResources::WrapperViews views;
	views.srv = a_srvDesc;
	auto wrapper = GraphResources::MakeWrapper(*texture, a_name, views);
	LayerWrappers()[a_layer] = wrapper;
	twin = std::move(texture);
	if (a_layer == 0)
		logger::info("[ORG] Cloud Shadows: the cloud layers are graph textures ({}x{}, {} mips), published by name", desc.Width, desc.Height, desc.MipLevels);
	return wrapper.Get();
}

std::string ORGCloudShadows::PublishedLayerId(int a_layer)
{
	if (a_layer < 0 || a_layer >= CloudShadows::kMaxCloudLayers || !LayerTwins()[a_layer])
		return {};
	return LayerTwins()[a_layer]->Name();
}

FrameAnchors::Texture ORGCloudShadows::ImportLayer(int a_layer)
{
	if (a_layer < 0 || a_layer >= CloudShadows::kMaxCloudLayers)
		return nullptr;
	const auto& twin = LayerTwins()[a_layer];
	return twin ? std::dynamic_pointer_cast<org::ExternalTextureResource>(twin->Graph()) : nullptr;
}


ID3D11ShaderResourceView* ORGCloudShadows::OcclusionSRV() const
{
	return IsActive() ? impl->occlusion->SRV() : nullptr;
}

bool ORGCloudShadows::Setup()
{
	// Once a session. The occlusion cubemap is published, so it is never torn down.
	if (!BeginSetup())
		return IsActive();
	auto& cloudShadows = globals::features::cloudShadows;
	auto* lastLayer = cloudShadows.texCloudShadowLayers[kLastLayer];
	if (!lastLayer || !lastLayer->srv)
		Fatal("its layer textures are missing");

	D3D11_TEXTURE2D_DESC layerDesc{};
	lastLayer->resource->GetDesc(&layerDesc);
	auto next = std::make_shared<Impl>();
	next->width = layerDesc.Width;
	next->height = layerDesc.Height;
	next->mips = layerDesc.MipLevels;

	next->program = RenderGraph::Programs::Queue("CloudShadows", ComputeProgram::Desc{ .source = kShader, .constantWords = ShaderBindings::Words<OcclusionCopyBlock>() });
	next->gate = RenderGraph::RunGate::Create("CloudShadows::OcclusionCopyGate", kFaces * next->mips);

	const D3D11_TEXTURE2D_DESC desc{ .Width = layerDesc.Width,
		.Height = layerDesc.Height,
		.MipLevels = layerDesc.MipLevels,
		.ArraySize = layerDesc.ArraySize,
		.Format = layerDesc.Format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		.MiscFlags = layerDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE };
	// Engine draws read it at t25 and t26 until they are ported. A frame without the full sky keeps the last copy, so it is kept across frames.
	next->occlusion = std::make_unique<GraphResources::Texture2D>(desc, std::string(kOcclusionId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
	if (!next->occlusion->SRV())
		Fatal(fmt::format("the {}x{} occlusion cubemap could not be created", desc.Width, desc.Height));
	next->lastLayer = ImportLayer(kLastLayer);
	if (!next->lastLayer)
		Fatal("the last cloud layer could not be imported");

	// The layers and the occlusion cubemap are sized once, from the engine's reflection cubemap, which the engine makes once a
	// session (Q4). The client is declared on that cubemap, so a revision that carries another edge is fatal: the D3D11 layers
	// and their views do not follow it. The first evaluation is at the next revision.
	const UINT layerEdge = next->width;
	CopyClient() = RenderGraph::Frame::DeclareClient<std::uint32_t>(
		kClientId, RenderGraph::Frame::DependsOn(RenderGraph::Frame::Vars::ReflectionCubemap()),
		[layerEdge](const RenderGraph::Frame::Inputs& a_in) -> std::optional<std::uint32_t> {
			// Zero before the engine's targets raised it: the layers' edge.
			const std::uint32_t edge = a_in.Get(RenderGraph::Frame::Vars::ReflectionCubemap()).size;
			return edge ? edge : layerEdge;
		},
		[next](const std::uint32_t& a_edge, FrameAnchors::ClientDesc& a_client) {
			if (a_edge != next->width)
				RenderGraph::Fatal(kName, fmt::format("the reflection cubemap is {} wide, but the cloud layers were made {} wide", a_edge, next->width));
			a_client.id = kClientId;
			a_client.passes.push_back({ .name = std::string(kClientId) + ".occlusion-copy",
				.anchor = FrameAnchors::Anchor::ReflectionsPrepass,
				.order = 0,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [next]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<OcclusionCopyPass>(next); } });
			a_client.registerResources = [next](org::RenderGraph& a_graph) { next->occlusion->Register(a_graph); };
			a_client.commit = [next](const FrameAnchors::Point&) { Commit(*next); };
			// The anchor runs at every cubemap render, but only the run after the frame's first reflections prepass copies, as on D3D11.
			a_client.active = [next] { return next->pending; };
			a_client.gates = { next->gate };
			return true;
		});
	// DCLF's colour passes read it by name, ordered after the copy.
	if (!PublishedResources::Publish(*next->occlusion))
		Fatal("the occlusion cubemap could not be published");
	impl = next;
	logger::info("[ORG] Cloud Shadows: the occlusion copy runs on the render graph ({}x{}, {} mips); its client follows the frame revisions", desc.Width,
		desc.Height, desc.MipLevels);
	SetupLayerOps();
	return true;
}

bool ORGCloudShadows::QueueOcclusionCopy()
{
	if (!IsActive()) {
		if (On())
			Fatal("an occlusion copy was asked for before setup");
		return false;
	}
	impl->pending = true;
	return true;
}

// ---- Layer operations ----

namespace
{
	constexpr const char* kLayerShader = "CloudShadows/LayerOpsCS.hlsl";
	constexpr std::uint32_t kLayers = CloudShadows::kMaxCloudLayers;
	// Each anchor has its own gate range, since a frame phase may hold two of them in one execution.
	constexpr std::array<FrameAnchors::Anchor, 3> kLayerAnchors{ FrameAnchors::Anchor::ReflectionsPrepass, FrameAnchors::Anchor::SkyReflections,
		FrameAnchors::Anchor::EarlyPrepass };
	constexpr std::uint32_t kLayerEntries = 2;  // per anchor and tap: the gather, then the scatter

	/** @brief 0 for another anchor. */
	std::uint32_t LayerSlot(FrameAnchors::Anchor a_anchor)
	{
		for (std::uint32_t i = 0; i < kLayerAnchors.size(); ++i)
			if (kLayerAnchors[i] == a_anchor)
				return i;
		return 0;
	}
	// The most distinct faces one run may read. More is fatal.
	constexpr std::uint32_t kScratchSlots = 32;
	// A tap's rows: the gather entries, then the scatter entries.
	constexpr std::uint32_t kGatherWords = kScratchSlots;
	constexpr std::uint32_t kScatterWords = kLayers * kFaces;
	constexpr std::uint32_t kScatterRow = kGatherWords * sizeof(std::uint32_t);
	constexpr std::uint32_t kLatchBytes = (kGatherWords + kScatterWords) * sizeof(std::uint32_t);
	// Layer in bits 24 to 28, face in bits 16 to 18, and the scratch slot or a clear in the low half.
	constexpr std::uint32_t kNoWord = 0xFFFFFFFFu;
	constexpr std::uint32_t kZeroWord = 0xFFFEu;
	constexpr std::uint32_t Entry(std::uint32_t a_layer, std::uint32_t a_face, std::uint32_t a_value) { return a_layer << 24 | a_face << 16 | a_value; }
	// After the queued ops each layer face is untouched, zero, or a copy of some layer's face as it was before the run.
	constexpr std::uint8_t kUntouched = 0xFF;
	constexpr std::uint8_t kZero = 0xFE;
	// LayerOpsCS packs layer handles in 24 bits each.
	constexpr std::uint32_t kHandleBits = 24;
	constexpr std::uint32_t kPackedWords = kLayers * kHandleBits / 32;

	/** @brief Written by hand in LayerOpsCS: 32 layer UAV handles packed in 24 words, the scratch, and the tap's row. */
	struct LayerOpsBlock
	{
		std::array<std::uint32_t, kPackedWords> layers{};
		ShaderBindings::Handle scratch = 0;
		ShaderBindings::Latch row;
	};
	static_assert(kLayers == 32 && kPackedWords == 24, "LayerOpsCS packs 32 layer handles in 24 words");

	/** @brief Layer i below 24 sits in word i's low 24 bits. Layer 24 + j sits in the top bytes of words 3j to 3j + 2. */
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
	// Made at setup, then only read on the graph host's thread.
	RenderGraph::Programs::Fixed<ComputeProgram> gather, scatter;
	std::shared_ptr<RenderGraph::RunGate> gate;  // per anchor: gather of layer l at l, scatter into layer l at kLayers + l
	std::shared_ptr<org::LatchBlock> latch;
	std::unique_ptr<GraphResources::Texture2D> scratch;
	std::array<FrameAnchors::Texture, kLayers> layers;
	UINT width = 0;
	UINT height = 0;
	// One tap per reflection face when the start stage holds SkyReflections, else 1. Sets no tap opened stay closed.
	std::uint32_t instances = 1;
	// Taps of one run need distinct slots, since every gather runs before every scatter.
	std::uint32_t slotsUsed = 0;
	std::uint64_t slotsFrame = 0;

	std::uint32_t SlotsTaken() const
	{
		return instances > 1 && slotsFrame == RenderGraph::Frame::Driver::Current().frame ? slotsUsed : 0;
	}

	// Render thread. The queue since the last run took it.
	std::vector<LayerOp> ops;
	LayerOpPoint point = LayerOpPoint::D3D11;
	std::array<std::array<std::uint8_t, kFaces>, kLayers> value{};
	std::array<std::uint32_t, kFaces> sources{};  // per face, a bit per layer whose face the copies read
	std::uint32_t sourceCount = 0;

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
		std::array<org::DeclaredViewToken, kLayers> layers;  // each layer's face array, mip 0
		org::DeclaredViewToken scratch;
	};

	struct LayerFrame
	{
		std::shared_ptr<const LayerOps> ops;
		bool scatter = false;
		std::uint32_t base = 0;
		std::uint32_t sets = 1;  // one per tap of a run
	};

	/**
	* @brief The gather copies read faces into scratch slots, the scatter writes changed faces from their slot or as zero.
	* Every layer is bound by its whole-array UAV, so one dispatch per tap reaches any face.
	*/
	class LayerOpsPass final : public org::TypedRenderGraphPass<LayerOpsPass, LayerFrame, LayerBindings>
	{
	public:
		LayerOpsPass(std::shared_ptr<const LayerOps> a_ops, bool a_scatter, std::uint32_t a_base, std::uint32_t a_sets = 1) :
			ops(std::move(a_ops)), scatter(a_scatter), base(a_base), sets(a_sets), scratch(ops->scratch->Graph()) {}

		LayerBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// A single cube has a Texture2DArrayFull UAV but no such SRV, so the gather reads through the UAV.
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
			const auto program = a_frame.scatter ? ops.scatter.Get() : ops.gather.Get();
			std::array<std::uint32_t, kLayers> handles{};
			for (std::uint32_t l = 0; l < kLayers; ++l) {
				handles[l] = a_recording.Index(a_bindings.layers[l]);
				if (handles[l] >> kHandleBits)
					logger::error("[ORG] Cloud Shadows: layer {}'s descriptor index {} does not fit LayerOpsCS's 24 bits", l, handles[l]);
			}
			LayerOpsBlock block{};
			PackLayers(handles, block.layers);
			block.scratch = a_recording.Index(a_bindings.scratch);
			// One entry set per tap, in tap order.
			for (std::uint32_t k = 0; k < a_frame.sets; ++k) {
				const std::uint32_t rows = k * kLatchBytes + (a_frame.scatter ? kScatterRow : 0);
				block.row = ShaderBindings::LatchOf(*ops.latch, a_recording.Run(), rows);
				ops.gate->Dispatch(a_recording, *program, ShaderBindings::Data(block), a_frame.base + k * kLayerEntries + (a_frame.scatter ? 1 : 0));
			}
		}

	private:
		std::shared_ptr<const LayerOps> ops;
		bool scatter = false;
		std::uint32_t base = 0;
		std::uint32_t sets = 1;
		std::shared_ptr<org::Resource> scratch;
	};

	/** @brief Turns the queue into the run's rows and opens the matching gate entries. The queue is empty afterwards. */
	void CommitLayerOps(LayerOps& a_ops, FrameAnchors::Anchor a_anchor)
	{
		if (a_ops.ops.empty())
			return;
		std::array<std::uint32_t, kGatherWords> gatherRow;
		std::array<std::uint32_t, kScatterWords> scatterRow;
		gatherRow.fill(kNoWord);
		scatterRow.fill(kNoWord);
		std::uint32_t gatherCount = 0, scatterCount = 0;
		std::array<std::uint32_t, kLayers * kFaces> slotOf;
		slotOf.fill(kNoWord);
		// A start-stage SkyReflections tap uses its face's entry set and the slots after those earlier face taps took.
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
				// The queue counted every face read, so the slots suffice.
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

void ORGCloudShadows::SetupLayerOps()
{
	layerOps.reset();
	auto next = std::make_shared<LayerOps>();
	next->Reset();
	// In graph mode every layer is a twin.
	for (std::uint32_t l = 0; l < kLayers; ++l) {
		next->layers[l] = ImportLayer(static_cast<int>(l));
		if (!next->layers[l])
			Fatal(fmt::format("layer {} could not be imported", l));
	}
	D3D11_TEXTURE2D_DESC layerDesc{};
	globals::features::cloudShadows.texCloudShadowLayers[0]->resource->GetDesc(&layerDesc);
	next->width = layerDesc.Width;
	next->height = layerDesc.Height;

	next->gather = RenderGraph::Programs::Queue("CloudShadows",
		ComputeProgram::Desc{ .source = kLayerShader, .constantWords = ShaderBindings::Words<LayerOpsBlock>(), .defines = { { L"LAYER_GATHER", L"" } } });
	next->scatter = RenderGraph::Programs::Queue("CloudShadows", ComputeProgram::Desc{ .source = kLayerShader, .constantWords = ShaderBindings::Words<LayerOpsBlock>() });
	// One entry set per reflection face: the start stage holds the faces' sky.
	next->instances = kFaces;
	next->gate = RenderGraph::RunGate::Create("CloudShadows::LayerOpsGate", static_cast<std::uint32_t>(kLayerAnchors.size()) * next->instances * kLayerEntries);
	next->latch = std::make_shared<org::LatchBlock>(std::string(kLayersClientId) + ".latch", next->instances * kLatchBytes);
	// Every run writes the slots it reads.
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

	// First at each anchor that follows a layer op.
	FrameAnchors::ClientDesc client{};
	client.id = kLayersClientId;
	for (const auto [anchor, label] : { std::pair{ FrameAnchors::Anchor::ReflectionsPrepass, "reflections-prepass" },
			 std::pair{ FrameAnchors::Anchor::SkyReflections, "sky-reflections" }, std::pair{ FrameAnchors::Anchor::EarlyPrepass, "early-prepass" } }) {
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
		Fatal("none of the layer operations' anchors has an epoch");
	client.registerResources = [next](org::RenderGraph& a_graph) { next->scratch->Register(a_graph); };
	client.commit = [next](const FrameAnchors::Point& a_point) { CommitLayerOps(*next, a_point.anchor); };
	client.active = [next] { return !next->ops.empty(); };
	client.gates = { next->gate };
	// The start stage taps once per reflection face.
	client.repeats = next->instances > 1;
	layerOps = next;
	// Registered at the next revision start, which waits for the stage builds that hold it.
	LayersClient() = RenderGraph::Frame::DeclareClient<bool>(
		kLayersClientId, {}, [](const RenderGraph::Frame::Inputs&) -> std::optional<bool> { return true; },
		[client = std::move(client)](const bool&, FrameAnchors::ClientDesc& a_out) {
			a_out = client;
			return true;
		});
	logger::info("[ORG] Cloud Shadows: the layers' clears and copies run on the render graph ({} scratch slots)", kScratchSlots);
}

bool ORGCloudShadows::QueueLayerOp(LayerOpPoint a_point, LayerOp a_op)
{
	// Outside graph mode every op is on D3D11. An engine-drawn face's ops go with its D3D11 draw.
	if (!On() || a_point == LayerOpPoint::D3D11)
		return false;
	auto* ops = layerOps.get();
	if (!ops)
		Fatal("a layer operation came before setup");
	if (a_op.dst >= kLayers || a_op.face >= kFaces || (a_op.src != kClearOp && a_op.src >= kLayers))
		Fatal(fmt::format("layer operation {} from {} on face {} is out of range", a_op.dst, a_op.src, a_op.face));
	// An op is queued only while its anchor's run is scheduled, and that run takes the whole queue. Ops left for another
	// anchor mean a scheduled run never came.
	if (!ops->ops.empty() && ops->point != a_point)
		Fatal(fmt::format("{} layer operations for anchor point {} were not taken by its run before an op for point {}", ops->ops.size(),
			static_cast<int>(ops->point), static_cast<int>(a_point)));
	if (ops->ops.empty()) {
		const auto place = FrameAnchors::Place(kLayersClientId, AnchorOf(a_point));
		if (!place.draws)
			Fatal(fmt::format("no run of anchor point {} takes the layer operations ({})", static_cast<int>(a_point), FrameAnchors::ReasonName(place.reason)));
	}
	auto& target = ops->value[a_op.dst][a_op.face];
	if (a_op.src == kClearOp) {
		target = kZero;
	} else {
		// Zero, a face as it was before the run, or itself.
		std::uint8_t value = ops->value[a_op.src][a_op.face];
		if (value == kUntouched)
			value = a_op.src;
		if (value == a_op.dst) {
			target = kUntouched;  // the face's own contents again, or a copy onto itself
		} else {
			if (value != kZero && !(ops->sources[a_op.face] & (1u << value))) {
				// Slots taken by earlier face taps this frame are not free, since their gathers run with this one.
				if (ops->sourceCount + (a_point == LayerOpPoint::SkyReflections ? ops->SlotsTaken() : 0) >= kScratchSlots)
					Fatal(fmt::format("a run's layer operations read more than {} faces", kScratchSlots));
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

void ORGCloudShadows::NoteD3D11Work()
{
	if (On())
		RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::D3D11Path, RenderGraph::Frame::SegmentId::Count,
			fmt::format("Cloud Shadows ({}) drew a cloud layer's occlusion on D3D11", kClientId));
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

ORGCloudShadows::ORGCloudShadows() = default;
ORGCloudShadows::~ORGCloudShadows() = default;

