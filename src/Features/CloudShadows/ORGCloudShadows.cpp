#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/CloudShadows/ORGCloudShadows.h"

#include <cstring>

namespace
{
	constexpr const char* kEnvSwitch = "CS_ORG_CLOUD_SHADOWS";
}

bool ORGCloudShadows::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA(kEnvSwitch, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
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
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <algorithm>
#	include <atomic>
#	include <utility>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.cloud-shadows";
	constexpr const char* kOcclusionName = "CloudShadows::Occlusion";
	constexpr int kLastLayer = CloudShadows::kMaxCloudLayers - 1;
	// No value prepared yet: differs from both copy states.
	constexpr std::uint8_t kNotPrepared = 0xFF;
}

struct ORGCloudShadows::Impl
{
	std::unique_ptr<GraphResources::Texture2D> occlusion;
	std::shared_ptr<org::Resource> lastLayer;
	UINT width = 0;
	UINT height = 0;
	UINT mips = 0;

	// Render thread: ReflectionsPrepass queued this frame's copy.
	bool pending = false;
	// Published at the hook (the client's active): whether this run of the anchor copies.
	std::atomic<bool> copyNow = false;
};

namespace
{
	using Impl = ORGCloudShadows::Impl;

	struct CopyBindings
	{
		org::ResourceBindingToken from, to;
	};

	struct CopyFrame
	{
		UINT width = 0;
		UINT height = 0;
		UINT mips = 0;
	};

	/** @brief The last layer into the occlusion cubemap, every face and mip, as the D3D11 path's CopyResource. */
	class OcclusionCopyPass final : public org::TypedRenderGraphPass<OcclusionCopyPass, CopyFrame, CopyBindings>
	{
	public:
		// At the build, on the render thread: the textures as they are now (GraphResources' rule).
		explicit OcclusionCopyPass(std::shared_ptr<const Impl> a_impl) :
			impl(std::move(a_impl)), from(impl->lastLayer), to(impl->occlusion->Graph()) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopySource(from), a_builder.CopyDestination(to) };
		}

		// The copy state it was prepared with, while it is the one published. A ticket prepared before the hook published another
		// one takes a value no later check repeats: it is prepared again.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			const auto live = static_cast<std::uint8_t>(impl->copyNow.load(std::memory_order_acquire) ? 1 : 0);
			const std::uint8_t used = prepared.load(std::memory_order_acquire);
			a_out.push_back(used);
			if (used != live)
				a_out.push_back(mismatch.fetch_add(1, std::memory_order_relaxed) + 1);
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&)
		{
			const bool copy = impl->copyNow.load(std::memory_order_acquire);
			prepared.store(static_cast<std::uint8_t>(copy ? 1 : 0), std::memory_order_release);
			if (!copy)
				return {};
			return { impl->width, impl->height, impl->mips };
		}

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.width)
				return;
			auto& commands = a_recording.Commands();
			const auto sourceImage = a_recording.Resolve(a_bindings.from).GetHandle();
			const auto targetImage = a_recording.Resolve(a_bindings.to).GetHandle();
			for (std::uint32_t face = 0; face < 6; ++face) {
				for (std::uint32_t mip = 0; mip < a_frame.mips; ++mip) {
					const std::uint32_t width = (std::max)(a_frame.width >> mip, 1u);
					const std::uint32_t height = (std::max)(a_frame.height >> mip, 1u);
					const rhi::TextureCopyRegion target{ .texture = targetImage, .mip = mip, .arraySlice = face, .width = width, .height = height };
					const rhi::TextureCopyRegion source{ .texture = sourceImage, .mip = mip, .arraySlice = face, .width = width, .height = height };
					commands.CopyTextureRegion(target, source);
				}
			}
		}

	private:
		std::shared_ptr<const Impl> impl;
		std::shared_ptr<org::Resource> from;
		std::shared_ptr<org::Resource> to;
		std::atomic<std::uint8_t> prepared = kNotPrepared;
		mutable std::atomic<std::uint64_t> mismatch = 0;
	};

	/** @brief Binds the D3D11 occlusion copy at t25 again, filled from the last layer, before the export goes. */
	void ReturnToD3D11()
	{
		auto& cloudShadows = globals::features::cloudShadows;
		auto* copy = cloudShadows.texCubemapCloudOccCopy;
		auto* lastLayer = cloudShadows.texCloudShadowLayers[kLastLayer];
		if (!copy || !lastLayer)
			return;
		auto* context = globals::d3d::context;
		context->CopyResource(copy->resource.get(), lastLayer->resource.get());
		ID3D11ShaderResourceView* srv = copy->srv.get();
		context->PSSetShaderResources(25, 1, &srv);
		context->CSSetShaderResources(25, 1, &srv);
	}
}

void ORGCloudShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::ReflectionsPrepass))
		logger::warn("[ORG] Cloud Shadows stays on D3D11: the ReflectionsPrepass anchor could not be reserved");
}

bool ORGCloudShadows::Active() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

ID3D11ShaderResourceView* ORGCloudShadows::OcclusionSRV() const
{
	return Active() ? impl->occlusion->SRV() : nullptr;
}

void ORGCloudShadows::Fail()
{
	failed = true;
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kOcclusionName);
	if (impl)
		ReturnToD3D11();
	impl.reset();
}

bool ORGCloudShadows::Setup()
{
	auto& cloudShadows = globals::features::cloudShadows;
	auto* lastLayer = cloudShadows.texCloudShadowLayers[kLastLayer];
	if (!lastLayer || !lastLayer->srv) {
		logger::error("[ORG] Cloud Shadows: its layer textures are missing; back to D3D11");
		Fail();
		return false;
	}

	D3D11_TEXTURE2D_DESC layerDesc{};
	lastLayer->resource->GetDesc(&layerDesc);
	auto next = std::make_shared<Impl>();
	next->width = layerDesc.Width;
	next->height = layerDesc.Height;
	next->mips = layerDesc.MipLevels;

	// The last layer's shape (the reflections cubemap's, in R8), sampled only.
	const D3D11_TEXTURE2D_DESC desc{ .Width = layerDesc.Width,
		.Height = layerDesc.Height,
		.MipLevels = layerDesc.MipLevels,
		.ArraySize = layerDesc.ArraySize,
		.Format = layerDesc.Format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE,
		.MiscFlags = layerDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE };
	// Engine draws read it at t25 and t26 (and Drawcall Limit Fix through that view) until they are ported. A frame without the
	// full sky keeps the last copy, so it lives across frames.
	next->occlusion = std::make_unique<GraphResources::Texture2D>(desc, kOcclusionName,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
	if (!*next->occlusion || !next->occlusion->SRV()) {
		logger::error("[ORG] Cloud Shadows: the {}x{} occlusion cubemap could not be created; back to D3D11", desc.Width, desc.Height);
		Fail();
		return false;
	}
	// Engine draws and D3D11 copies write the layers, so the last one stays a D3D11 texture.
	next->lastLayer = FrameAnchors::D3D11Texture("CloudShadows::Layer[31]", lastLayer->resource.get(), lastLayer->srv.get(), nullptr);
	if (!next->lastLayer) {
		logger::error("[ORG] Cloud Shadows: the last cloud layer could not be imported; back to D3D11");
		Fail();
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes.push_back({ .name = std::string(kClientId) + ".occlusion-copy",
		.anchor = FrameAnchors::Anchor::ReflectionsPrepass,
		.order = 0,
		.kind = FrameAnchors::PassKind::Copy,
		.make = [next]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<OcclusionCopyPass>(next); } });
	client.registerResources = [next](org::RenderGraph& a_graph) { next->occlusion->Register(a_graph); };
	// The anchor runs at every cubemap render; only the frame's first copies, as on D3D11.
	client.active = [next] {
		const bool copy = std::exchange(next->pending, false);
		next->copyNow.store(copy, std::memory_order_release);
		return copy;
	};
	client.onFailed = [] {
		logger::error("[ORG] Cloud Shadows: the render graph failed; back to D3D11");
		auto& self = Get();
		PublishedResources::Withdraw(kOcclusionName);
		ReturnToD3D11();
		self.failed = true;
		self.impl.reset();
	};
	if (!FrameAnchors::Register(std::move(client))) {
		Fail();
		return false;
	}
	// Drawcall Limit Fix's colour passes read it by name, ordered after the copy (Integrations/CloudShadows).
	if (!PublishedResources::Publish(*next->occlusion)) {
		logger::error("[ORG] Cloud Shadows: the occlusion cubemap could not be published; back to D3D11");
		Fail();
		return false;
	}
	impl = std::move(next);
	logger::info("[ORG] Cloud Shadows: the occlusion copy runs on the render graph ({}x{}, {} mips)", desc.Width, desc.Height, desc.MipLevels);
	return true;
}

bool ORGCloudShadows::QueueOcclusionCopy()
{
	if (!Requested() || failed)
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::ReflectionsPrepass))
		return false;
	if (!impl && !Setup())
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
bool ORGCloudShadows::Active() const { return false; }
ID3D11ShaderResourceView* ORGCloudShadows::OcclusionSRV() const { return nullptr; }
void ORGCloudShadows::Fail() { failed = true; }
bool ORGCloudShadows::Setup() { return false; }
bool ORGCloudShadows::QueueOcclusionCopy() { return false; }
ORGCloudShadows::~ORGCloudShadows() = default;

#endif
