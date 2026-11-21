// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGLocalShadows.h"

#include "Features/LightLimitFix.h"
#include "Globals.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/Switches.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/VersionedTexture.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace
{
	constexpr const char* kClientId = "cs.llf.local-shadows";
	constexpr const char* kShader = "LightLimitFix/LocalShadowCopyCS.hlsl";
	// LocalShadowCopyCS's GROUP_SIZE.
	constexpr std::uint32_t kGroupSize = 8;
	constexpr std::uint32_t kShadowMaps = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS);

	// One per copy in the latch's first kMaxCopies rows. The gate entry of the same index says whether it runs.
	struct CopyConstants
	{
		std::uint32_t sourceSlice;
		std::uint32_t targetSlice;
		std::uint32_t scale;
		std::uint32_t targetSize;
	};
	static_assert(sizeof(CopyConstants) == 16);

	// The copies, then the slots' data.
	constexpr std::uint32_t kDataOffset = ORGLocalShadows::kMaxCopies * sizeof(CopyConstants);

	// In list order.
	struct CopyBlock
	{
		ShaderBindings::Handle source, cache;
		ShaderBindings::Latch copy;
	};

	/** @brief Made once at Setup and kept for the session, whatever the cache's shape. */
	struct LocalShadowShared
	{
		RenderGraph::Programs::Fixed<ComputeProgram> program;
		// Every slot the setting allows, so a new slot count changes nothing here.
		std::unique_ptr<GraphResources::StructuredBuffer> data;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		std::uint32_t capacity = 0;
		std::uint32_t dataStride = 0;
		// Written into the latch at the commit. staged means SetFrame ran since the last commit.
		std::vector<CopyConstants> copies = std::vector<CopyConstants>(ORGLocalShadows::kMaxCopies);
		std::uint32_t copyCount = 0;
		std::uint32_t groups = 0;
		std::vector<std::byte> slotData;
		bool staged = false;
	};

	/** @brief The cache, declared once. A new resolution or slot count is a version of it (ORG array-size versions for the slots). */
	struct LocalShadowCache
	{
		RenderGraph::Frame::DeclaredTexture2D texture;
	};

	/** @brief What the client's registration depends on: only whether the cache is on. A new resolution or slot count is a version. */
	struct LocalShadowClientKey
	{
		friend bool operator==(const LocalShadowClientKey&, const LocalShadowClientKey&) = default;
	};

	const LightLimitFix::Settings& LocalShadowSettings()
	{
		return globals::features::lightLimitFix.settings;
	}

	/** @brief Whether the cache is on for one revision's values: the setting, and an engine shadow map array to copy from. */
	template <class Values>
	bool LocalShadowCacheOn(const Values& a_values)
	{
		const auto engine = a_values.Get(RenderGraph::Frame::Vars::EngineShadowMap());
		return a_values.Get(LocalShadowSettings().EnableLocalShadows) && engine.size.width != 0 && engine.slices > 1;
	}

	/** @brief The cache's resolution for one revision's values, while it is on (the declaration's existence rule). */
	template <class Values>
	std::uint32_t LocalShadowCacheResolution(const Values& a_values)
	{
		const auto engine = a_values.Get(RenderGraph::Frame::Vars::EngineShadowMap());
		return LightLimitFix::LocalShadowResolutionFor(a_values.Get(LocalShadowSettings().LocalShadowResolution), engine.size.width);
	}

	/** @brief The cache's slot count for one revision's values: its slice count. */
	template <class Values>
	std::uint32_t LocalShadowCacheSlots(const Values& a_values)
	{
		return LightLimitFix::LocalShadowSlotsFor(a_values.Get(LocalShadowSettings().LocalShadowSlots));
	}

	/** @brief The graph's copy writes the cache as a 32-bit float storage image. */
	D3D11_TEXTURE2D_DESC LocalShadowCacheDesc(std::uint32_t a_resolution, std::uint32_t a_slots)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_resolution;
		desc.Height = a_resolution;
		desc.MipLevels = 1;
		desc.ArraySize = a_slots;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	/**
	* @brief Declares the cache once. Its resolution and its slot count follow the settings by versions: a new slot count is a new
	* slice count, which switches in with graphs built for it (ORG array-size versions). Fatal when it cannot be made.
	*/
	std::shared_ptr<const LocalShadowCache> DeclareLocalShadowCache()
	{
		using namespace RenderGraph::Frame;
		const auto& settings = LocalShadowSettings();
		auto made = std::make_shared<LocalShadowCache>();
		// Never aliased, and exported for the engine's pixel shaders at t103.
		const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV };
		// One texel per slot while the cache is off (the existence rule). Named apart from the published id, as before.
		made->texture = DeclareTexture2D(fmt::format("{} (cache)", ORGLocalShadows::kCacheId),
			DependsOn(settings.LocalShadowResolution, settings.LocalShadowSlots, Vars::EngineShadowMap()),
			[](const Inputs& a_in) { return LocalShadowCacheDesc(LocalShadowCacheResolution(a_in), LocalShadowCacheSlots(a_in)); },
			exported, ExistsWhen(DependsOn(settings.EnableLocalShadows, Vars::EngineShadowMap()), [](const Inputs& a_in) { return LocalShadowCacheOn(a_in); }));
		if (!made->texture->SRV() || !made->texture->Versioned())
			RenderGraph::Fatal(ORGLocalShadows::kName, "the cache could not be made and exported");
		return made;
	}

	// A gated dispatch per possible copy, each reading its latch row.
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
		ShadowCopyPass(const LocalShadowShared& a_shared, const LocalShadowCache& a_cache, FrameAnchors::Texture a_source) :
			program(a_shared.program.Get()), latch(a_shared.latch), gate(a_shared.gate), source(std::move(a_source)), cache(a_cache.texture->Versioned()) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// The shader reads and writes both arrays whole, so use the whole-array views.
			const org::SrvView sourceView{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			const org::UavView cacheView{ .variant = static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull) };
			// The cache is declared through its slot, so a new resolution is a new version of the same declaration. A new slot count
			// is one too, and this graph is built again for its slice count.
			return { a_builder.ShaderResource(source, sourceView).View(), a_builder.UnorderedAccess(org::ResourceResolverAndRange(*cache), cacheView).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(cache.get()));
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { program, latch, gate }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.gate)
				return;
			CopyBlock block{ a_recording.Index(a_bindings.source), a_recording.Index(a_bindings.cache), {} };
			// An unstaged copy has zero groups, so it does no work.
			for (std::uint32_t copy = 0; copy < ORGLocalShadows::kMaxCopies; ++copy) {
				block.copy = ShaderBindings::LatchOf(*a_frame.latch, a_recording.Run(), copy * sizeof(CopyConstants));
				a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), copy);
			}
		}

	private:
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		FrameAnchors::Texture source;  // the engine's shadow maps slot
		std::shared_ptr<org::VersionedTexture> cache;
	};

	// Into the buffer the engine's pixel shaders read at t102.
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
		// The whole capacity moves every run, so the pass does not follow the slot count.
		explicit ShadowDataPass(const LocalShadowShared& a_shared) :
			latch(a_shared.latch), data(a_shared.data->Graph()), bytes(a_shared.capacity * a_shared.dataStride) {}

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
			const auto region = a_frame.latch->At(a_recording.Run());
			a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.data).GetHandle(), 0,
				region.buffer->GetAPIResource().GetHandle(), region.offset + kDataOffset, a_frame.bytes);
		}

	private:
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<org::Resource> data;
		std::uint32_t bytes = 0;
	};

	/** @brief The registration while the cache is on: the copies of a_cache, at the start of the revision that adopts it. */
	void MakeLocalShadowClient(const std::shared_ptr<LocalShadowShared>& a_shared, const std::shared_ptr<const LocalShadowCache>& a_cache, FrameAnchors::ClientDesc& a_client)
	{
		a_client.id = kClientId;
		// The slots' data first. The copy does not depend on it.
		a_client.passes.push_back({ .name = "cs.llf.local-shadow-data",
			.anchor = FrameAnchors::Anchor::EarlyPrepass,
			.order = 100,
			.kind = FrameAnchors::PassKind::Copy,
			.make = [a_shared]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowDataPass>(*a_shared); } });
		a_client.passes.push_back({ .name = "cs.llf.local-shadow-copy",
			.anchor = FrameAnchors::Anchor::EarlyPrepass,
			.order = 101,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [a_shared, a_cache]() -> std::shared_ptr<org::RenderPass> {
				// A new engine texture is a new import, which rebuilds the anchors.
				auto shadowMaps = FrameAnchors::EngineTexture(FrameAnchors::Depth(kShadowMaps));
				if (!shadowMaps)
					return nullptr;
				return std::make_shared<ShadowCopyPass>(*a_shared, *a_cache, std::move(shadowMaps));
			} });
		a_client.registerResources = [a_shared, a_cache](org::RenderGraph& a_graph) {
			a_cache->texture->Register(a_graph);
			a_shared->data->Register(a_graph);
		};
		a_client.commit = [a_shared](const FrameAnchors::Point&) {
			auto* graphHost = RenderGraphRuntime::Get().Host();
			if (!graphHost)
				return;
			const org::RunRef run = graphHost->CurrentRun();
			// A frame that staged nothing opens no copy, since the engine's slices are not this frame's. The data stays as last staged.
			if (std::exchange(a_shared->staged, false)) {
				a_shared->latch->Write(run, 0, std::as_bytes(std::span(a_shared->copies)));
				for (std::uint32_t copy = 0; copy < a_shared->copyCount; ++copy)
					a_shared->gate->OpenDispatch(copy, a_shared->groups, a_shared->groups);
			}
			a_shared->latch->Write(run, kDataOffset, a_shared->slotData);
		};
		a_client.gates = { a_shared->gate };
	}
}

struct ORGLocalShadows::Impl
{
	std::shared_ptr<LocalShadowShared> shared;
	// The session's cache: kCacheId publishes it and every registration copies into it. Its slot count follows its versions.
	std::shared_ptr<const LocalShadowCache> cache;
	RenderGraph::Frame::DeclaredClient client;
	// Whether a registration switched in. Before the first, the frame has no cache.
	bool clientAdopted = false;
	// Adopted() once per frame.
	mutable std::uint64_t shapeFrame = UINT64_MAX;
	mutable Shape shape;

	/** @brief Inside the adoption point of the revision that switched in the registration. */
	void Adopt()
	{
		clientAdopted = true;
		shapeFrame = UINT64_MAX;
	}
};

ORGLocalShadows::ORGLocalShadows() = default;
ORGLocalShadows::~ORGLocalShadows() = default;

bool ORGLocalShadows::Setup(std::uint32_t a_dataStride, std::uint32_t a_capacity)
{
	// Once a session. The cache and its data are published, so they are never torn down.
	if (!BeginSetup())
		return IsActive();
	if (!a_dataStride || !a_capacity)
		Fatal("the cache needs slots and slot data");

	auto shared = std::make_shared<LocalShadowShared>();
	shared->capacity = a_capacity;
	shared->dataStride = a_dataStride;
	shared->program = RenderGraph::Programs::Queue("LightLimitFix", ComputeProgram::Desc{ .source = kShader, .constantWords = ShaderBindings::Words<CopyBlock>() });
	const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV };
	D3D11_BUFFER_DESC dataDesc{};
	dataDesc.Usage = D3D11_USAGE_DEFAULT;
	dataDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	dataDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	dataDesc.StructureByteStride = a_dataStride;
	dataDesc.ByteWidth = a_capacity * a_dataStride;
	shared->data = std::make_unique<GraphResources::StructuredBuffer>(dataDesc, a_capacity, std::string(kDataId), exported);
	if (!*shared->data || !shared->data->SRV())
		Fatal("the slot data could not be created and exported");
	try {
		shared->latch = org::LatchBlock::Create("LightLimitFix::LocalShadowLatch", kDataOffset + a_capacity * a_dataStride);
	} catch (const std::exception& e) {
		Fatal(fmt::format("the latch could not be created: {}", e.what()));
	}
	shared->gate = RenderGraph::RunGate::Create("LightLimitFix::LocalShadowCopyGate", kMaxCopies);
	shared->slotData.assign(static_cast<std::size_t>(a_capacity) * a_dataStride, std::byte{ 0 });

	auto made = std::make_unique<Impl>();
	made->shared = shared;
	made->cache = DeclareLocalShadowCache();
	// Published once. A new resolution or slot count is a version readers follow.
	// DCLF's colour passes and the fog read them by name, ordered after the copies.
	if (!PublishedResources::Publish(*shared->data) || !PublishedResources::Publish(kCacheId, *made->cache->texture))
		Fatal("the cache or its data could not be published");

	using namespace RenderGraph::Frame;
	const auto& settings = LocalShadowSettings();
	auto* state = made.get();
	made->client = DeclareClient<LocalShadowClientKey>(
		kClientId, DependsOn(settings.EnableLocalShadows, Vars::EngineShadowMap()),
		[](const Inputs& a_in) -> std::optional<LocalShadowClientKey> {
			if (!LocalShadowCacheOn(a_in))
				return std::nullopt;
			return LocalShadowClientKey{};
		},
		[state](const LocalShadowClientKey&, FrameAnchors::ClientDesc& a_client) {
			MakeLocalShadowClient(state->shared, state->cache, a_client);
			logger::info("[ORG] LLF local shadows: a client for the next revision, copying into the cache");
			return true;
		},
		[state](const LocalShadowClientKey&) { state->Adopt(); });
	impl = std::move(made);
	logger::info("[ORG] LLF local shadows run on the render graph at EarlyPrepass; the cache and its client follow the frame revisions");
	return true;
}

ORGLocalShadows::Shape ORGLocalShadows::Adopted() const
{
	if (!IsActive())
		return {};
	const auto frame = RenderGraph::Frame::Driver::Current().frame;
	if (impl->shapeFrame == frame)
		return impl->shape;
	impl->shapeFrame = frame;
	impl->shape = {};
	const auto& adopted = RenderGraph::Frame::Revision::Adopted();
	if (!impl->clientAdopted || !impl->cache || !LocalShadowCacheOn(adopted))
		return impl->shape;
	const auto engine = adopted.Get(RenderGraph::Frame::Vars::EngineShadowMap());
	// The bound version's, which the copies write: its resolution, and its slice count as the slots.
	const auto extent = adopted.Extent(impl->cache->texture);
	Shape shape;
	shape.slots = extent[2];
	shape.resolution = extent[0];
	shape.engineResolution = engine.size.width;
	shape.engineSlices = engine.slices;
	// The copy tree-reduces the engine's texels, so the ratio must be a power of two. A cache made for newer values waits for them.
	const bool copyable = shape.resolution > 1 && shape.engineResolution % shape.resolution == 0 && std::has_single_bit(shape.engineResolution / shape.resolution);
	if (copyable)
		impl->shape = shape;
	return impl->shape;
}

void ORGLocalShadows::SetFrame(std::span<const Copy> a_copies, std::span<const std::byte> a_data)
{
	const auto shape = Adopted();
	if (!shape)
		return;
	auto& shared = *impl->shared;
	const std::uint32_t scale = shape.engineResolution / shape.resolution;
	shared.copyCount = static_cast<std::uint32_t>((std::min)(a_copies.size(), static_cast<std::size_t>(kMaxCopies)));
	for (std::uint32_t i = 0; i < kMaxCopies; ++i) {
		if (i < shared.copyCount)
			shared.copies[i] = { a_copies[i].sourceSlice, a_copies[i].targetSlot, scale, shape.resolution };
		else
			shared.copies[i] = {};
	}
	const std::uint32_t threadsPerTexel = (std::min)(scale, kGroupSize);
	shared.groups = (shape.resolution * threadsPerTexel + kGroupSize - 1) / kGroupSize;
	// Slots past the adopted count stay zero, so no reader finds an old slot's data there.
	const std::size_t bytes = (std::min)(a_data.size(), shared.slotData.size());
	std::memcpy(shared.slotData.data(), a_data.data(), bytes);
	std::fill(shared.slotData.begin() + static_cast<std::ptrdiff_t>(bytes), shared.slotData.end(), std::byte{ 0 });
	shared.staged = true;
}

ID3D11ShaderResourceView* ORGLocalShadows::GetCacheSRV() const
{
	return Adopted() ? impl->cache->texture->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGLocalShadows::GetDataSRV() const
{
	return Adopted() ? impl->shared->data->SRV() : nullptr;
}

