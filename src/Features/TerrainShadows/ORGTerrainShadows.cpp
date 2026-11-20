#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGTerrainShadows::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/Frame/Variables.h"
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
#	include <Resources/VersionedTexture.h>

#	include <algorithm>
#	include <iterator>
#	include <map>
#	include <optional>
#	include <string>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.terrain-shadows";
	constexpr const char* kShader = "TerrainShadows/ShadowUpdate.cs.hlsl";
	// ShadowUpdate.cs.hlsl's NTHREADS, the pixels of one stripe along the light.
	constexpr std::uint32_t kStripeLength = 128;
	// Other heightmap formats are converted to R16_UNORM on the CPU.
	constexpr DXGI_FORMAT kHeightFormat = DXGI_FORMAT_R16_UNORM;
	constexpr DXGI_FORMAT kHeightBlockFormat = DXGI_FORMAT_BC4_UNORM;

	using Constants = TerrainShadows::ShadowUpdateCB;
	static_assert(sizeof(Constants) == 48, "ShadowUpdateConstants in ShadowUpdate.cs.hlsl is 48 bytes");

	// In SHADOW_UPDATE_RESOURCES order.
	struct ShadowUpdateBlock
	{
		ShaderBindings::Handle heightMap;
		ShaderBindings::Handle shadowHeights;
		ShaderBindings::Latch constants;
	};

	/** @brief Immutable. Each heightmap's client gets its own. */
	struct UpdateSpec
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::shared_ptr<org::Resource> heightMap;
		std::shared_ptr<org::VersionedTexture> shadowHeights;
		std::uint32_t maxDispatches = 0;
	};

	[[noreturn]] void Fail(std::string_view a_why)
	{
		const auto message = fmt::format("Community Shaders: Terrain Shadows on the render graph failed: {}", a_why);
		logger::critical("[ORG] Terrain Shadows: {}", a_why);
		spdlog::default_logger()->flush();
		stl::report_and_fail(message);
	}
}

/** @brief One heightmap, from SetHeightmap on. Its client's registration holds it while that client is attached. */
struct ORGTerrainShadows::Heightmap
{
	std::uint64_t id = 0;  // its value of the HeightMap variable
	std::uint32_t width = 0, height = 0;
	int format = 0;  // for the log
	std::uint32_t maxDispatches = 0;
	std::unique_ptr<GraphResources::Texture2D> texture;
	std::shared_ptr<const GraphResources::StagedUpload> staged;

	// Render thread. Held until the commit latches them.
	std::vector<Constants> pending;
	std::uint32_t pendingGroups = 0;
	bool pendingRefresh = false;
	// Render thread. A full refresh ran in a graph holding this heightmap's client and its upload.
	bool refreshed = false;
};

struct ORGTerrainShadows::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	// Declared at Setup on the HeightMap variable and published once. It is 1x1 until the first heightmap. Each heightmap's size
	// is a new version made by the revision that carries it, which DCLF's graphs follow with no build.
	RenderGraph::Frame::DeclaredTexture2D shadowHeights;
	// Declared at Setup on the HeightMap variable. A new heightmap registers it again in the revision that carries it.
	RenderGraph::Frame::DeclaredClient client;
	// Render thread. The newest heightmap. Submit fills its stripes.
	std::shared_ptr<Heightmap> current;
	// Render thread. Heightmaps no client was made for yet, by id. The client's make takes the one its key names.
	std::map<std::uint64_t, std::shared_ptr<Heightmap>> staged;
	std::uint64_t lastId = 0;
};

namespace
{
	using Impl = ORGTerrainShadows::Impl;
	using Heightmap = ORGTerrainShadows::Heightmap;
	using HeightMapIdentity = RenderGraph::Frame::Vars::HeightMapIdentity;

	struct UpdateBindings
	{
		org::DeclaredViewToken heightMap, shadowHeights;
	};

	struct UpdateFrame
	{
		std::shared_ptr<const UpdateSpec> spec;
	};

	/**
	* @brief One gated dispatch per stripe, so the recording stays the same. The steady update (stripe 0) is recorded ahead.
	* The refresh (stripes 1 and up) records at execution and skips unless stripe 1 is open, so steady frames skip its barriers.
	*/
	template <bool Refresh>
	class ShadowUpdatePass final : public org::TypedRenderGraphPass<ShadowUpdatePass<Refresh>, UpdateFrame, UpdateBindings>
	{
	public:
		explicit ShadowUpdatePass(std::shared_ptr<const UpdateSpec> a_spec) :
			spec(std::move(a_spec)) {}

		UpdateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			// A new worldspace's size is a new version of the slot.
			return { a_builder.ShaderResource(spec->heightMap).View(),
				a_builder.UnorderedAccess(org::ResourceResolverAndRange(*spec->shadowHeights)).View() };
		}

		// The recording depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

		UpdateFrame Prepare(const UpdateBindings&, const org::PassPrepareContext&) const { return { spec }; }

		// A kept late batch keeps its list while SkipsAtExecution's answer holds.
		static constexpr bool kRecordKeyedBySkip = true;

		// A run that did not open stripe 1 is not a refresh.
		static bool SkipsAtExecution(const UpdateBindings&, const UpdateFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return Refresh && !a_frame.spec->gate->IsOpen(a_recording.Run(), 1);
		}

		static void Record(const UpdateBindings& a_bindings, const UpdateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& spec = *a_frame.spec;
			const org::RunRef run = a_recording.Run();
			const std::uint32_t first = Refresh ? 1 : 0;
			const std::uint32_t end = Refresh ? spec.maxDispatches : (std::min)(1u, spec.maxDispatches);
			for (std::uint32_t i = first; i < end; ++i) {
				// Each stripe reads the one before it.
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
	};

	/** @brief BC4_UNORM and R16_UNORM pass through, others convert to R16_UNORM. False, logged, when DirectXTex cannot convert. */
	bool ToHeightFormat(const DirectX::Image& a_image, DirectX::ScratchImage& a_converted, const DirectX::Image*& a_out)
	{
		if (a_image.format == kHeightFormat || a_image.format == kHeightBlockFormat) {
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

	/** @brief Runs only in a graph holding this heightmap's client, so a committed refresh covers the heightmap it uploads. */
	void Commit(Heightmap& a_heightmap, org::LatchBlock& a_latch, RenderGraph::RunGate& a_gate)
	{
		const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
		if (a_heightmap.pending.empty())
			return;
		a_latch.Write(run, 0, std::as_bytes(std::span(a_heightmap.pending)));
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(a_heightmap.pending.size()); ++i)
			a_gate.OpenDispatch(i, a_heightmap.pendingGroups);
		a_heightmap.refreshed = a_heightmap.refreshed || a_heightmap.pendingRefresh;
		a_heightmap.pending.clear();
	}

	/**
	* @brief The client for the heightmap a_key names, when the revision that carries it starts. Its shadow height map version
	* is made in the same revision, so both switch in at one Begin. False, logged, is fatal.
	*/
	bool MakeClient(Impl& a_impl, const HeightMapIdentity& a_key, FrameAnchors::ClientDesc& a_client)
	{
		const auto found = a_impl.staged.find(a_key.id);
		if (found == a_impl.staged.end()) {
			logger::error("[ORG] Terrain Shadows: heightmap #{} was never staged", a_key.id);
			return false;
		}
		const auto heightmap = found->second;
		// Revisions carry heightmaps in the order they were set, so the older ones never get a client.
		a_impl.staged.erase(a_impl.staged.begin(), std::next(found));

		const std::uint32_t maxDispatches = heightmap->maxDispatches;
		std::shared_ptr<org::LatchBlock> latch;
		try {
			latch = std::make_shared<org::LatchBlock>("TerrainShadows::UpdateLatch", maxDispatches * static_cast<std::uint32_t>(sizeof(Constants)));
		} catch (const std::exception& e) {
			logger::error("[ORG] Terrain Shadows: its latch block could not be created ({})", e.what());
			return false;
		}
		auto gate = RenderGraph::RunGate::Create("TerrainShadows::UpdateGate", maxDispatches);
		if (!gate) {
			logger::error("[ORG] Terrain Shadows: its run gate could not be created");
			return false;
		}
		const auto spec = std::make_shared<const UpdateSpec>(
			UpdateSpec{ a_impl.program, latch, gate, heightmap->texture->Graph(), a_impl.shadowHeights->Versioned(), maxDispatches });

		// Key 99 is the one-shot heightmap copy, 100 the update that reads it.
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 99);
		builder.Add(
			"heightmap-upload", FrameAnchors::PassKind::Copy,
			[staged = heightmap->staged]() -> std::shared_ptr<org::RenderPass> { return GraphResources::MakeUploadPass({ staged }); }, true);
		builder.Add(
			"update", FrameAnchors::PassKind::Compute,
			[spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass<false>>(spec); });
		// Records nothing in a run that is not a refresh.
		if (maxDispatches > 1)
			builder.Add(
				"refresh", FrameAnchors::PassKind::Compute,
				[spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass<true>>(spec); }, false, true);
		if (builder.Failed()) {
			logger::error("[ORG] Terrain Shadows: {}", builder.Failure());
			return false;
		}

		a_client.id = kClientId;
		a_client.passes = builder.Take();
		// The declaration holds the shadow height map's twin.
		a_client.registerResources = [heightmap, shadowHeights = a_impl.shadowHeights](org::RenderGraph& a_graph) {
			heightmap->texture->Register(a_graph);
			shadowHeights->Register(a_graph);
		};
		a_client.commit = [heightmap, latch, gate](const FrameAnchors::Point&) { Commit(*heightmap, *latch, *gate); };
		a_client.active = [heightmap] { return !heightmap->pending.empty(); };
		a_client.gates = { gate };
		logger::info("[ORG] Terrain Shadows: {}x{} heightmap #{} (format {}) made its client, up to {} stripes a refresh", heightmap->width, heightmap->height,
			heightmap->id, heightmap->format, maxDispatches);
		return true;
	}
}

void ORGTerrainShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Terrain Shadows stays on D3D11: the EarlyPrepass anchor could not be reserved");
}

bool ORGTerrainShadows::Setup()
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
	using namespace RenderGraph::Frame;
	// Engine draws read it at t60 until they are ported. The heightmap's size, 1x1 before the first one.
	next->shadowHeights = DeclareTexture2D(
		std::string(kShadowHeightId), DependsOn(Vars::HeightMap()),
		[](const Inputs& a_in) {
			const auto size = a_in.Get(Vars::HeightMap()).size;
			return D3D11_TEXTURE2D_DESC{ .Width = (std::max)(size.width, 1u),
				.Height = (std::max)(size.height, 1u),
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R16G16_UNORM,
				.SampleDesc = { .Count = 1 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS };
		},
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!next->shadowHeights || !next->shadowHeights->SRV() || !next->shadowHeights->Versioned()) {
		logger::error("[ORG] Terrain Shadows: its shadow height map could not be created; it stays on D3D11");
		return false;
	}
	// DCLF's colour passes read it by name, ordered after the update. Published once.
	if (!PublishedResources::Publish(*next->shadowHeights)) {
		logger::error("[ORG] Terrain Shadows: the shadow height map could not be published; it stays on D3D11");
		return false;
	}
	const std::weak_ptr<Impl> state = next;
	next->client = DeclareClient<HeightMapIdentity>(
		kClientId, DependsOn(Vars::HeightMap()),
		[](const Inputs& a_in) -> std::optional<HeightMapIdentity> {
			const auto heightMap = a_in.Get(Vars::HeightMap());
			if (!heightMap.id)
				return std::nullopt;
			return heightMap;
		},
		[state](const HeightMapIdentity& a_key, FrameAnchors::ClientDesc& a_client) {
			const auto current = state.lock();
			return current && MakeClient(*current, a_key, a_client);
		});
	if (!next->client) {
		logger::error("[ORG] Terrain Shadows: its client could not be declared; it stays on D3D11");
		PublishedResources::Withdraw(kShadowHeightId);
		return false;
	}
	impl = next;
	logger::info("[ORG] Terrain Shadows: heightmaps go to the render graph, each in textures of its own size");
	return true;
}

bool ORGTerrainShadows::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

bool ORGTerrainShadows::SetHeightmap(const DirectX::Image& a_image)
{
	if (!IsActive())
		return false;
	if (a_image.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_image.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		Fail(fmt::format("a {}x{} heightmap is larger than a texture can be", a_image.width, a_image.height));
	DirectX::ScratchImage converted;
	const DirectX::Image* image = nullptr;
	if (!ToHeightFormat(a_image, converted, image))
		Fail(fmt::format("the heightmap's format {} cannot be converted to R16_UNORM", static_cast<int>(a_image.format)));
	auto next = std::make_shared<Heightmap>();
	next->id = ++impl->lastId;
	next->width = static_cast<std::uint32_t>(image->width);
	next->height = static_cast<std::uint32_t>(image->height);
	next->format = static_cast<int>(image->format);
	next->maxDispatches = ((std::max)(next->width, next->height) + kStripeLength - 1) / kStripeLength;
	const D3D11_TEXTURE2D_DESC desc{ .Width = next->width,
		.Height = next->height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = image->format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE };
	// Nothing outside the graph reads it, so no D3D11 view. Only its client's graphs hold it.
	next->texture = std::make_unique<GraphResources::Texture2D>(desc, "TerrainShadows::HeightMap",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
	if (!*next->texture)
		Fail(fmt::format("its {}x{} heightmap texture could not be created", next->width, next->height));
	const GraphResources::SubresourceData bytes{ image->pixels, static_cast<UINT>(image->rowPitch), static_cast<UINT>(image->slicePitch) };
	next->staged = next->texture->Stage(std::span<const GraphResources::SubresourceData>(&bytes, 1));
	if (!next->staged)
		Fail(fmt::format("its {}x{} heightmap could not be staged", next->width, next->height));
	next->pending.reserve(next->maxDispatches);
	// The revision that carries this value makes the shadow height map's version and the client. No stripes run until then.
	impl->staged[next->id] = next;
	impl->current = next;
	RenderGraph::Frame::Vars::SetHeightMap({ next->id, { next->width, next->height } }, "Terrain Shadows");
	logger::info("[ORG] Terrain Shadows: {}x{} heightmap #{} (format {}) staged; it switches in with the frame revision that carries it", next->width,
		next->height, next->id, next->format);
	return true;
}

bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups, bool a_refresh)
{
	if (!IsActive() || !impl->current || a_dispatches.empty() || !a_groups)
		return false;
	auto& heightmap = *impl->current;
	const auto count = (std::min)(static_cast<std::uint32_t>(a_dispatches.size()), heightmap.maxDispatches);
	heightmap.pending.assign(a_dispatches.begin(), a_dispatches.begin() + count);
	heightmap.pendingGroups = a_groups;
	heightmap.pendingRefresh = a_refresh;
	return true;
}

bool ORGTerrainShadows::NeedsRefresh() const
{
	return IsActive() && impl->current && !impl->current->refreshed;
}

ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const
{
	return IsActive() && impl->current ? impl->shadowHeights->SRV() : nullptr;
}

ORGTerrainShadows::~ORGTerrainShadows() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Terrain Shadows always runs on D3D11.
struct ORGTerrainShadows::Impl
{};

void ORGTerrainShadows::Reserve() {}
bool ORGTerrainShadows::Setup() { return false; }
bool ORGTerrainShadows::IsActive() const { return false; }
bool ORGTerrainShadows::SetHeightmap(const DirectX::Image&) { return false; }
bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB>, std::uint32_t, bool) { return false; }
bool ORGTerrainShadows::NeedsRefresh() const { return false; }
ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const { return nullptr; }
ORGTerrainShadows::~ORGTerrainShadows() = default;

#endif
