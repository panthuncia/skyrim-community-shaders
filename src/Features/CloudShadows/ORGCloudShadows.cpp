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

namespace
{
	// RequestGraphFaces: set at the sky family's install, before Setup.
	bool s_graphFacesRequested = false;
}

void ORGCloudShadows::RequestGraphFaces()
{
	s_graphFacesRequested = true;
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
#	include <span>
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
	// The copy's sources, in gate entry order (ORGCloudShadows::FaceSource).
	constexpr std::uint32_t kSources = 2;

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
	// The sky family's faces (RequestGraphFaces); null without them.
	std::unique_ptr<GraphResources::Texture2D> faces;
	UINT width = 0;
	UINT height = 0;
	UINT mips = 0;
	std::function<void()> onFailed;

	// Render thread: ReflectionsPrepass queued this frame's copy; the commit of the next run opens the gate and clears it.
	bool pending = false;
	// Render thread: the source each face is copied from (NoteFace), read by the commit.
	std::array<ORGCloudShadows::FaceSource, kFaces> sources{};

	/** @brief The gate entry of one face's copy of one mip from one source. */
	std::uint32_t Entry(ORGCloudShadows::FaceSource a_source, std::uint32_t a_face, std::uint32_t a_mip) const
	{
		return (static_cast<std::uint32_t>(a_source) * kFaces + a_face) * mips + a_mip;
	}
};

namespace
{
	using Impl = ORGCloudShadows::Impl;

	struct CopyBindings
	{
		std::vector<org::DeclaredViewToken> layer;    // face + mip * 6: one face of one mip of the last layer
		std::vector<org::DeclaredViewToken> faces;    // face + mip * 6: one slice of one mip of the graph faces (empty without them)
		std::vector<org::DeclaredViewToken> targets;  // face + mip * 6: one face of one mip of the occlusion cubemap
	};

	struct CopyFrame
	{
		std::shared_ptr<const Impl> impl;
	};

	/**
	 * @brief Each face of the occlusion cubemap from the source that drew it last, every mip, as the D3D11 path's CopyResource of the
	 * last layer. One gated dispatch a source, face and mip: the commit opens one source's dispatches for each face.
	 */
	class OcclusionCopyPass final : public org::TypedRenderGraphPass<OcclusionCopyPass, CopyFrame, CopyBindings>
	{
	public:
		// At the build, on the render thread: the textures as they are now (GraphResources' rule).
		explicit OcclusionCopyPass(std::shared_ptr<const Impl> a_impl) :
			impl(std::move(a_impl)), from(impl->lastLayer), faces(impl->faces ? impl->faces->Graph() : nullptr), to(impl->occlusion->Graph()) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// Single face views of each source (the import is a cube with views per face and mip, the graph faces a 6-slice array) and
			// of the cubemap (the default UAV of a slice and mip is a one-slice array view, so a group's z of 0 writes that face).
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
			if (faces)
				bindings.faces = a_builder.ShaderResource(faces, std::span<const org::SrvView>(faceViews)).views;
			bindings.targets = a_builder.UnorderedAccess(to, std::span<const org::UavView>(targetViews)).views;
			return bindings;
		}

		// The recording depends only on what the pass was made with: which source a face reads is the gate's data.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(impl.get()));
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { impl }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& impl = *a_frame.impl;
			for (std::uint32_t s = 0; s < kSources; ++s) {
				const auto source = static_cast<ORGCloudShadows::FaceSource>(s);
				const auto& views = source == ORGCloudShadows::FaceSource::Layer ? a_bindings.layer : a_bindings.faces;
				if (views.empty())
					continue;
				for (std::uint32_t mip = 0; mip < impl.mips; ++mip) {
					for (std::uint32_t face = 0; face < kFaces; ++face) {
						// One face a dispatch (z = 0): the shader reads Face0 and writes the target view's one slice.
						OcclusionCopyBlock block{};
						block.faces.fill(a_recording.Index(views[mip * kFaces + face]));
						block.occlusion = a_recording.Index(a_bindings.targets[mip * kFaces + face]);
						impl.gate->Dispatch(a_recording, *impl.program, ShaderBindings::Data(block), impl.Entry(source, face, mip));
					}
				}
			}
		}

	private:
		std::shared_ptr<const Impl> impl;
		std::shared_ptr<org::Resource> from;
		std::shared_ptr<org::Resource> faces;
		std::shared_ptr<org::Resource> to;
	};

	/**
	 * @brief The client's commit: on the run that follows the frame's first reflections prepass, each face copied from the source that
	 * drew it last, every mip.
	 */
	void Commit(Impl& a_impl)
	{
		if (!std::exchange(a_impl.pending, false))
			return;
		for (std::uint32_t face = 0; face < kFaces; ++face) {
			const auto source = a_impl.faces ? a_impl.sources[face] : ORGCloudShadows::FaceSource::Layer;
			for (std::uint32_t mip = 0; mip < a_impl.mips; ++mip) {
				const std::uint32_t width = (std::max)(a_impl.width >> mip, 1u);
				const std::uint32_t height = (std::max)(a_impl.height >> mip, 1u);
				a_impl.gate->OpenDispatch(a_impl.Entry(source, face, mip), (width + kGroupSize - 1) / kGroupSize, (height + kGroupSize - 1) / kGroupSize, 1);
			}
		}
	}
}

void ORGCloudShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::ReflectionsPrepass))
		logger::warn("[ORG] Cloud Shadows stays on D3D11: the ReflectionsPrepass anchor could not be reserved");
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
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kOcclusionId);
	if (impl && impl->faces)
		PublishedResources::Withdraw(kFacesId);
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
	next->gate = RenderGraph::RunGate::Create("CloudShadows::OcclusionCopyGate", kSources * kFaces * next->mips);
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
	// Engine draws and D3D11 copies write the layers, so the last one stays a D3D11 texture. It lives as long as the feature.
	next->lastLayer = FrameAnchors::D3D11Texture("CloudShadows::Layer[31]", lastLayer->resource.get(), lastLayer->srv.get(), nullptr);
	if (!next->lastLayer) {
		logger::error("[ORG] Cloud Shadows: the last cloud layer could not be imported; it stays on D3D11");
		return false;
	}
	// The sky family's faces: the last layer's shape as a plain 6-slice array (a slice per face), drawn as a render target. Faces the
	// reflection does not draw in a frame keep their occlusion, so it lives across frames. Made and published here, before the copy's
	// first build, so the copy declares it from the start and never rebuilds for it.
	if (s_graphFacesRequested) {
		const D3D11_TEXTURE2D_DESC facesDesc{ .Width = layerDesc.Width,
			.Height = layerDesc.Height,
			.MipLevels = layerDesc.MipLevels,
			.ArraySize = kFaces,
			.Format = layerDesc.Format,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS };
		next->faces = std::make_unique<GraphResources::Texture2D>(facesDesc, std::string(kFacesId), GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
		if (!*next->faces || !PublishedResources::Publish(*next->faces)) {
			// The copy reads the last layer alone, and the sky family leaves the faces' clouds to the engine.
			logger::error("[ORG] Cloud Shadows: the graph faces ({}x{}) could not be made or published; the faces' clouds stay the engine's", facesDesc.Width,
				facesDesc.Height);
			next->faces.reset();
		}
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes.push_back({ .name = std::string(kClientId) + ".occlusion-copy",
		.anchor = FrameAnchors::Anchor::ReflectionsPrepass,
		.order = 0,
		.kind = FrameAnchors::PassKind::Compute,
		.make = [next]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<OcclusionCopyPass>(next); } });
	client.registerResources = [next](org::RenderGraph& a_graph) {
		next->occlusion->Register(a_graph);
		if (next->faces)
			next->faces->Register(a_graph);
	};
	client.commit = [next](const FrameAnchors::Point&) { Commit(*next); };
	// The anchor runs at every cubemap render; only the run after the frame's first reflections prepass copies, as on D3D11.
	client.active = [next] { return next->pending; };
	client.gates = { next->gate };
	client.onFailed = [] {
		logger::error("[ORG] Cloud Shadows: the render graph failed; back to D3D11");
		auto& self = Get();
		PublishedResources::Withdraw(kOcclusionId);
		if (self.impl && self.impl->faces)
			PublishedResources::Withdraw(kFacesId);
		auto onFailed = self.impl ? std::move(self.impl->onFailed) : nullptr;
		self.impl.reset();
		if (onFailed)
			onFailed();
	};
	impl = next;
	if (!FrameAnchors::Register(std::move(client))) {
		if (next->faces)
			PublishedResources::Withdraw(kFacesId);
		impl.reset();
		return false;
	}
	// Drawcall Limit Fix's colour passes read it by name, ordered after the copy (Integrations/CloudShadows).
	if (!PublishedResources::Publish(*next->occlusion)) {
		logger::error("[ORG] Cloud Shadows: the occlusion cubemap could not be published; it stays on D3D11");
		Stop();
		return false;
	}
	logger::info("[ORG] Cloud Shadows: the occlusion copy runs on the render graph ({}x{}, {} mips){}", desc.Width, desc.Height, desc.MipLevels,
		next->faces ? fmt::format("; each face from its last writer, the D3D11 layer or {}", kFacesId) : std::string());
	return true;
}

GraphResources::Texture2D* ORGCloudShadows::GraphFaces() const
{
	return IsActive() ? impl->faces.get() : nullptr;
}

void ORGCloudShadows::NoteFace(std::uint32_t a_face, FaceSource a_source)
{
	if (!IsActive() || a_face >= kFaces)
		return;
	impl->sources[a_face] = impl->faces ? a_source : FaceSource::Layer;
}

void ORGCloudShadows::NoteAllFaces(FaceSource a_source)
{
	for (std::uint32_t face = 0; face < kFaces; ++face)
		NoteFace(face, a_source);
}

bool ORGCloudShadows::QueueOcclusionCopy()
{
	if (!IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::ReflectionsPrepass))
		return false;
	impl->pending = true;
	return true;
}

ORGCloudShadows::~ORGCloudShadows() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Cloud Shadows always copies on D3D11.
struct ORGCloudShadows::Impl
{};

void ORGCloudShadows::Reserve() {}
bool ORGCloudShadows::Setup(std::function<void()>) { return false; }
bool ORGCloudShadows::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGCloudShadows::OcclusionSRV() const { return nullptr; }
void ORGCloudShadows::Stop() {}
bool ORGCloudShadows::QueueOcclusionCopy() { return false; }
GraphResources::Texture2D* ORGCloudShadows::GraphFaces() const { return nullptr; }
void ORGCloudShadows::NoteFace(std::uint32_t, FaceSource) {}
void ORGCloudShadows::NoteAllFaces(FaceSource) {}
ORGCloudShadows::~ORGCloudShadows() = default;

#endif
