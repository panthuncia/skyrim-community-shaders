#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGLocalShadows.h"

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>

#	include <algorithm>
#	include <cstring>
#	include <utility>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.llf.local-shadows";
	constexpr std::string_view kSwitch = "CS_ORG_LIGHT_LIMIT_FIX_SHADOWS";
	// The switch's earlier name, which older scenario files still set.
	constexpr std::string_view kDeprecatedSwitch = "CS_ORG_LLF_SHADOWS";
	constexpr const char* kShader = "LightLimitFix/LocalShadowCopyCS.hlsl";
	// LocalShadowCopyCS's group size (GROUP_SIZE).
	constexpr std::uint32_t kGroupSize = 8;
	// RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS.
	constexpr std::uint32_t kShadowMaps = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS);

	// LocalShadowCopyCS's CopyConstants: one per copy, the latch's first kMaxCopies rows. The run gate's entry of the same
	// index says whether that copy runs this frame.
	struct CopyConstants
	{
		std::uint32_t sourceSlice;
		std::uint32_t targetSlice;
		std::uint32_t scale;
		std::uint32_t targetSize;
	};
	static_assert(sizeof(CopyConstants) == 16);

	/** @brief The graph no longer writes the cache: its published readers return to their D3D11 views. */
	void WithdrawCache()
	{
		PublishedResources::Withdraw(ORGLocalShadows::kDataId);
		PublishedResources::Withdraw(ORGLocalShadows::kCacheId);
	}

	// The latch slot: the copies, then the slots' data.
	constexpr std::uint32_t kDataOffset = ORGLocalShadows::kMaxCopies * sizeof(CopyConstants);

	// LocalShadowCopyCS's push block, in list order.
	struct CopyBlock
	{
		ShaderBindings::Handle source, cache;
		ShaderBindings::Latch copy;
	};

	struct Resources
	{
		ORGLocalShadows::Desc desc;
		std::shared_ptr<const ComputeProgram> program;
		std::unique_ptr<GraphResources::Texture2D> cache;
		std::unique_ptr<GraphResources::StructuredBuffer> data;
		std::shared_ptr<org::LatchBlock> latch;
		// One entry per possible copy: the commit opens those of the copies the frame staged.
		std::shared_ptr<RenderGraph::RunGate> gate;
		// Each copy's groups along x and y.
		std::uint32_t groups = 0;
		// The frame SetFrame gave, written into the latch at the anchor's commit. staged: SetFrame ran since the last commit.
		std::vector<CopyConstants> copies = std::vector<CopyConstants>(ORGLocalShadows::kMaxCopies);
		std::uint32_t copyCount = 0;
		std::vector<std::byte> slotData;
		bool staged = false;
	};

	// Copies the frame's fresh engine slices into the cache: a gated dispatch per possible copy, each reading its row of the latch.
	struct CopyBindings
	{
		org::DeclaredViewToken source, cache;
	};

	struct CopyFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
	};

	class ShadowCopyPass final : public org::TypedRenderGraphPass<ShadowCopyPass, CopyFrame, CopyBindings>
	{
	public:
		// At the build, on the render thread: the resources as they are now (GraphResources' rule).
		ShadowCopyPass(const Resources& a_resources, std::shared_ptr<org::Resource> a_source) :
			program(a_resources.program), latch(a_resources.latch), gate(a_resources.gate), source(std::move(a_source)), cache(a_resources.cache->Graph()) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// Both are texture arrays the shader reads and writes whole: their whole-array views, not the default first slice.
			const org::SrvView sourceView{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			const org::UavView cacheView{ .variant = static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull) };
			return { a_builder.ShaderResource(source, sourceView).View(), a_builder.UnorderedAccess(cache, cacheView).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.get()));
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { program, latch, gate }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.gate)
				return;
			CopyBlock block{ a_recording.Index(a_bindings.source), a_recording.Index(a_bindings.cache), {} };
			// A copy the frame did not stage has zero groups in its gate entry, so it does no work.
			for (std::uint32_t copy = 0; copy < ORGLocalShadows::kMaxCopies; ++copy) {
				block.copy = ShaderBindings::LatchOf(*a_frame.latch, a_recording.FrameSlot(), copy * sizeof(CopyConstants));
				a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), copy);
			}
		}

	private:
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::shared_ptr<org::Resource> source;
		std::shared_ptr<org::Resource> cache;
	};

	// Moves the slots' data from the latch into the buffer the engine's pixel shaders read at t102.
	struct DataBindings
	{
		org::ResourceBindingToken data;
	};

	struct DataFrame
	{
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t bytes = 0;
	};

	class ShadowDataPass final : public org::TypedRenderGraphPass<ShadowDataPass, DataFrame, DataBindings>
	{
	public:
		explicit ShadowDataPass(const Resources& a_resources) :
			latch(a_resources.latch), data(a_resources.data->Graph()), bytes(a_resources.desc.slots * a_resources.desc.dataStride) {}

		DataBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(data) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(bytes);
		}

		DataFrame Prepare(const DataBindings&, const org::PassPrepareContext&) const { return { latch, bytes }; }

		static void Record(const DataBindings& a_bindings, const DataFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.latch || !a_frame.bytes)
				return;
			a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.data).GetHandle(), 0,
				a_frame.latch->Resource()->GetAPIResource().GetHandle(), a_frame.latch->Offset(a_recording.FrameSlot()) + kDataOffset, a_frame.bytes);
		}

	private:
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<org::Resource> data;
		std::uint32_t bytes = 0;
	};

	bool SameShape(const ORGLocalShadows::Desc& a_left, const ORGLocalShadows::Desc& a_right)
	{
		return a_left.slots == a_right.slots && a_left.resolution == a_right.resolution && a_left.format == a_right.format &&
		       a_left.scale == a_right.scale && a_left.dataStride == a_right.dataStride;
	}
}

struct ORGLocalShadows::Impl
{
	std::shared_ptr<Resources> resources;
	std::shared_ptr<const ComputeProgram> program;  // kept across shapes: it does not depend on them
};

ORGLocalShadows& ORGLocalShadows::Get()
{
	static ORGLocalShadows s_shadows;
	return s_shadows;
}

ORGLocalShadows::~ORGLocalShadows() = default;

bool ORGLocalShadows::Requested()
{
	static const bool requested = [] {
		if (Switches::IsSet(kSwitch) || !Switches::IsSet(kDeprecatedSwitch))
			return Switches::On(kSwitch);
		logger::warn("[ORG] {} is deprecated; set {} instead", kDeprecatedSwitch, kSwitch);
		return Switches::On(kDeprecatedSwitch);
	}();
	return requested;
}

void ORGLocalShadows::Reserve()
{
	if (Requested())
		FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass);
}

bool ORGLocalShadows::Setup(const Desc& a_desc, std::function<void()> a_onFailed)
{
	if (impl && impl->resources && SameShape(impl->resources->desc, a_desc) && IsActive())
		return true;
	Release();

	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!Requested())
		return false;
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the render graph is not active or EarlyPrepass has no epoch");
		return false;
	}
	// The graph's copy writes the cache as a 32-bit float storage image (LocalShadowCopyCS's RWTexture2DArray<float>).
	if (a_desc.format != DXGI_FORMAT_R32_FLOAT || !a_desc.slots || !a_desc.resolution || !a_desc.dataStride) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the cache must be R32_FLOAT with slots, a resolution and data");
		return false;
	}
	// The engine's shadow maps as a graph texture; the copy reads their depth view.
	auto source = FrameAnchors::EngineDepth(kShadowMaps);
	if (!source) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the engine's shadow maps could not be imported");
		return false;
	}

	auto state = std::make_unique<Impl>();
	if (impl)
		state->program = impl->program;
	if (!state->program)
		state->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<CopyBlock>() });
	if (!state->program) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the SPIR-V program could not be created");
		return false;
	}

	auto resources = std::make_shared<Resources>();
	resources->desc = a_desc;
	resources->program = state->program;
	const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV };

	D3D11_TEXTURE2D_DESC cacheDesc{};
	cacheDesc.Width = a_desc.resolution;
	cacheDesc.Height = a_desc.resolution;
	cacheDesc.MipLevels = 1;
	cacheDesc.ArraySize = a_desc.slots;
	cacheDesc.Format = a_desc.format;
	cacheDesc.SampleDesc.Count = 1;
	cacheDesc.Usage = D3D11_USAGE_DEFAULT;
	cacheDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	resources->cache = std::make_unique<GraphResources::Texture2D>(cacheDesc, std::string(kCacheId), exported);

	D3D11_BUFFER_DESC dataDesc{};
	dataDesc.Usage = D3D11_USAGE_DEFAULT;
	dataDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	dataDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	dataDesc.StructureByteStride = a_desc.dataStride;
	dataDesc.ByteWidth = a_desc.slots * a_desc.dataStride;
	resources->data = std::make_unique<GraphResources::StructuredBuffer>(dataDesc, a_desc.slots, std::string(kDataId), exported);

	if (!*resources->cache || !resources->cache->SRV() || !*resources->data || !resources->data->SRV()) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the cache or its data could not be created and exported");
		return false;
	}
	try {
		resources->latch = org::LatchBlock::Create("LightLimitFix::LocalShadowLatch", kDataOffset + a_desc.slots * a_desc.dataStride, host->FrameSlots());
	} catch (const std::exception& e) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the latch could not be created: {}", e.what());
		return false;
	}
	resources->gate = RenderGraph::RunGate::Create("LightLimitFix::LocalShadowCopyGate", kMaxCopies);
	if (!resources->gate) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the copies' run gate could not be created");
		return false;
	}
	const std::uint32_t threadsPerTexel = (std::min)(a_desc.scale, kGroupSize);
	resources->groups = (a_desc.resolution * threadsPerTexel + kGroupSize - 1) / kGroupSize;
	resources->slotData.assign(static_cast<std::size_t>(a_desc.slots) * a_desc.dataStride, std::byte{ 0 });

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	// The slots' data first; the copy does not depend on it.
	client.passes.push_back({ .name = "cs.llf.local-shadow-data",
		.anchor = FrameAnchors::Anchor::EarlyPrepass,
		.order = 100,
		.kind = FrameAnchors::PassKind::Copy,
		.make = [resources]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowDataPass>(*resources); } });
	client.passes.push_back({ .name = "cs.llf.local-shadow-copy",
		.anchor = FrameAnchors::Anchor::EarlyPrepass,
		.order = 101,
		.kind = FrameAnchors::PassKind::Compute,
		.make = [resources]() -> std::shared_ptr<org::RenderPass> {
			// The import as it is at this build: a new engine texture is a new import, which rebuilds the anchors.
			auto shadowMaps = FrameAnchors::EngineDepth(kShadowMaps);
			if (!shadowMaps)
				return nullptr;
			return std::make_shared<ShadowCopyPass>(*resources, std::move(shadowMaps));
		} });
	client.registerResources = [resources](org::RenderGraph& a_graph) {
		resources->cache->Register(a_graph);
		resources->data->Register(a_graph);
	};
	client.commit = [resources](const FrameAnchors::Point&) {
		auto* graphHost = RenderGraphRuntime::Get().Host();
		if (!graphHost)
			return;
		const std::uint32_t slot = graphHost->CurrentFrameSlot();
		// A frame that staged nothing opens no copy (the run closed the gate): the engine's slices are not this frame's. The
		// data stays as last staged.
		if (std::exchange(resources->staged, false)) {
			resources->latch->Write(slot, 0, std::as_bytes(std::span(resources->copies)));
			for (std::uint32_t copy = 0; copy < resources->copyCount; ++copy)
				resources->gate->OpenDispatch(copy, resources->groups, resources->groups);
		}
		resources->latch->Write(slot, kDataOffset, resources->slotData);
	};
	client.gates = { resources->gate };
	client.onFailed = [onFailed = std::move(a_onFailed)] {
		// LightLimitFix makes its D3D11 cache at its next EarlyPrepass.
		logger::error("[ORG] LLF local shadows: the render graph failed; back to D3D11");
		WithdrawCache();
		Get().impl.reset();
		if (onFailed)
			onFailed();
	};
	if (!FrameAnchors::Register(std::move(client))) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the EarlyPrepass client could not be registered");
		return false;
	}
	// Drawcall Limit Fix's colour passes read them by name, ordered after the copies (Integrations/LightLimitFix).
	if (!PublishedResources::Publish(*resources->data) || !PublishedResources::Publish(*resources->cache)) {
		logger::warn("[ORG] LLF local shadows stay on D3D11: the cache or its data could not be published");
		FrameAnchors::Unregister(kClientId);
		WithdrawCache();
		return false;
	}
	state->resources = std::move(resources);
	impl = std::move(state);
	logger::info("[ORG] LLF local shadows run on the render graph: {} slots at {}x{}, scale {}, at EarlyPrepass", a_desc.slots, a_desc.resolution,
		a_desc.resolution, a_desc.scale);
	return true;
}

void ORGLocalShadows::Release()
{
	if (!impl)
		return;
	if (impl->resources) {
		FrameAnchors::Unregister(kClientId);
		WithdrawCache();
		impl->resources.reset();
	}
}

bool ORGLocalShadows::IsActive() const
{
	return impl && impl->resources && RenderGraphRuntime::Get().IsActive();
}

void ORGLocalShadows::SetFrame(std::span<const Copy> a_copies, std::span<const std::byte> a_data)
{
	if (!IsActive())
		return;
	auto& resources = *impl->resources;
	const auto& desc = resources.desc;
	resources.copyCount = static_cast<std::uint32_t>((std::min)(a_copies.size(), static_cast<std::size_t>(kMaxCopies)));
	for (std::uint32_t i = 0; i < kMaxCopies; ++i) {
		if (i < resources.copyCount)
			resources.copies[i] = { a_copies[i].sourceSlice, a_copies[i].targetSlot, desc.scale, desc.resolution };
		else
			resources.copies[i] = {};
	}
	const std::size_t bytes = (std::min)(a_data.size(), resources.slotData.size());
	std::memcpy(resources.slotData.data(), a_data.data(), bytes);
	resources.staged = true;
}

ID3D11ShaderResourceView* ORGLocalShadows::GetCacheSRV() const
{
	return IsActive() ? impl->resources->cache->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGLocalShadows::GetDataSRV() const
{
	return IsActive() ? impl->resources->data->SRV() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the local shadow cache always stays on D3D11.
#	include "ORGLocalShadows.h"

struct ORGLocalShadows::Impl
{};

ORGLocalShadows& ORGLocalShadows::Get()
{
	static ORGLocalShadows s_shadows;
	return s_shadows;
}

ORGLocalShadows::~ORGLocalShadows() = default;
bool ORGLocalShadows::Requested() { return false; }
void ORGLocalShadows::Reserve() {}
bool ORGLocalShadows::Setup(const Desc&, std::function<void()>) { return false; }

void ORGLocalShadows::Release() {}
bool ORGLocalShadows::IsActive() const { return false; }
void ORGLocalShadows::SetFrame(std::span<const Copy>, std::span<const std::byte>) {}
ID3D11ShaderResourceView* ORGLocalShadows::GetCacheSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGLocalShadows::GetDataSRV() const { return nullptr; }
#endif
