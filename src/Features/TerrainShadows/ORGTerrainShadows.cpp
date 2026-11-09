#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/TerrainShadows/ORGTerrainShadows.h"

#include <cstring>

namespace
{
	constexpr const char* kEnvSwitch = "CS_ORG_TERRAIN_SHADOWS";
}

bool ORGTerrainShadows::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA(kEnvSwitch, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGTerrainShadows& ORGTerrainShadows::Get()
{
	static ORGTerrainShadows s_shadows;
	return s_shadows;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <DirectXTex.h>
#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <atomic>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.terrain-shadows";
	constexpr const char* kShader = "TerrainShadows/ShadowUpdate.cs.hlsl";
	// The shadow height map's name, which it is published under: Drawcall Limit Fix's t60.
	constexpr const char* kShadowHeightName = "TerrainShadows::ShadowHeight";
	// ShadowUpdate.cs.hlsl's NTHREADS: the pixels of one stripe along the light.
	constexpr std::uint32_t kStripeLength = 128;

	using Constants = TerrainShadows::ShadowUpdateCB;
	static_assert(sizeof(Constants) == 48, "ShadowUpdateConstants in ShadowUpdate.cs.hlsl is 48 bytes");

	// ShadowUpdate.cs.hlsl's push block (SHADOW_UPDATE_RESOURCES), in list order.
	struct ShadowUpdateBlock
	{
		ShaderBindings::Handle heightMap;
		ShaderBindings::Handle shadowHeights;
		ShaderBindings::Latch constants;
	};

	// What the recorded dispatches depend on: how many there are and their groups. It changes only with a refresh or when the
	// light's major axis flips, so its identity is the pass's revision; the constants are latched. Immutable once published.
	struct Shape
	{
		std::uint32_t dispatches = 0;
		std::uint32_t groups = 0;

		bool operator==(const Shape&) const = default;
	};
}

struct ORGTerrainShadows::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::unique_ptr<GraphResources::Texture2D> heightMap;
	std::unique_ptr<GraphResources::Texture2D> shadowHeights;
	std::shared_ptr<org::LatchBlock> latch;
	std::uint32_t maxDispatches = 0;

	// The heightmap's bytes in an upload buffer, laid out for a buffer-to-texture copy, until the copy pass has run. Async epochs
	// record no texture uploads, so a copy pass of the graph fills the heightmap instead.
	std::shared_ptr<org::Buffer> staging;
	rhi::CopyableFootprint stagingFootprint{};
	enum class Copy : std::uint8_t
	{
		None,
		Needed,     // the copy pass is in the graph; no epoch with it has been submitted yet
		Submitted,  // an epoch with it was submitted: the next frame drops it
	} copy = Copy::None;

	// Render thread: this frame's constants, until the commit latches them.
	std::vector<Constants> pending;
	bool pendingFrame = false;

	std::atomic<std::shared_ptr<const Shape>> shape;
};

namespace
{
	using Impl = ORGTerrainShadows::Impl;

	struct UpdateBindings
	{
		org::DeclaredViewToken heightMap, shadowHeights;
	};

	struct UpdateFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		Shape shape;
	};

	class ShadowUpdatePass final : public org::TypedRenderGraphPass<ShadowUpdatePass, UpdateFrame, UpdateBindings>
	{
	public:
		// At the build, on the render thread: the textures as they are now (GraphResources' rule).
		explicit ShadowUpdatePass(std::shared_ptr<Impl> a_impl) :
			impl(std::move(a_impl)), heightMap(impl->heightMap->Graph()), shadowHeights(impl->shadowHeights->Graph()) {}

		UpdateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.ShaderResource(heightMap).View(), a_builder.UnorderedAccess(shadowHeights).View() };
		}

		// The shape it was prepared with, while it is the one published. A ticket prepared before the commit published another one,
		// or while it did, takes a value no later check repeats: it is prepared again.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			const auto live = impl->shape.load(std::memory_order_acquire);
			const auto used = prepared.load(std::memory_order_acquire);
			a_out.push_back(reinterpret_cast<std::uintptr_t>(impl->program.get()));
			a_out.push_back(used ? (std::uint64_t{ used->dispatches } << 32 | used->groups) : 0);
			if (used != live)
				a_out.push_back(mismatch.fetch_add(1, std::memory_order_relaxed) + 1);
		}

		UpdateFrame Prepare(const UpdateBindings&, const org::PassPrepareContext&)
		{
			const auto shape = impl->shape.load(std::memory_order_acquire);
			prepared.store(shape, std::memory_order_release);
			if (!shape)
				return {};
			return { impl->program, impl->latch, *shape };
		}

		static void Record(const UpdateBindings& a_bindings, const UpdateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.shape.groups)
				return;
			const std::uint32_t slot = a_recording.FrameSlot();
			for (std::uint32_t i = 0; i < a_frame.shape.dispatches; ++i) {
				// Each stripe reads the one before it.
				if (i) {
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					a_recording.Commands().Barriers(rhi::BarrierBatch{ {}, {}, { &global, 1 } });
				}
				const ShadowUpdateBlock block{ a_recording.Index(a_bindings.heightMap), a_recording.Index(a_bindings.shadowHeights),
					ShaderBindings::LatchOf(*a_frame.latch, slot, i * static_cast<std::uint32_t>(sizeof(Constants))) };
				a_frame.program->Dispatch(a_recording, ShaderBindings::Data(block), a_frame.shape.groups, 1, 1);
			}
		}

	private:
		std::shared_ptr<Impl> impl;
		std::shared_ptr<org::Resource> heightMap;
		std::shared_ptr<org::Resource> shadowHeights;
		std::atomic<std::shared_ptr<const Shape>> prepared;
		mutable std::atomic<std::uint64_t> mismatch = 0;
	};

	struct CopyBindings
	{
		org::ResourceBindingToken heightMap;
	};

	struct CopyFrame
	{
		std::shared_ptr<org::Buffer> staging;
		rhi::CopyableFootprint footprint{};
	};

	/** @brief Fills the heightmap from the upload buffer. It is in the graph only until an epoch with it was submitted. */
	class HeightMapCopyPass final : public org::TypedRenderGraphPass<HeightMapCopyPass, CopyFrame, CopyBindings>
	{
	public:
		explicit HeightMapCopyPass(const Impl& a_impl) :
			heightMap(a_impl.heightMap->Graph()), staging(a_impl.staging), footprint(a_impl.stagingFootprint) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(heightMap) };
		}

		// The copy depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(staging.get())); }

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { staging, footprint }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.staging)
				return;
			rhi::BufferTextureCopyFootprint copy{};
			copy.texture = a_recording.Resolve(a_bindings.heightMap).GetHandle();
			copy.buffer = a_frame.staging->GetAPIResource().GetHandle();
			copy.mip = 0;
			copy.arraySlice = 0;
			copy.footprint = a_frame.footprint;
			a_recording.Commands().CopyBufferToTexture(copy);
		}

	private:
		std::shared_ptr<org::Resource> heightMap;
		std::shared_ptr<org::Buffer> staging;
		rhi::CopyableFootprint footprint;
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

	/** @brief Unbinds t60 before the export goes: its texture is freed once D3D11 is past the commands issued until then. */
	void UnbindShadowHeight()
	{
		ID3D11ShaderResourceView* none = nullptr;
		globals::d3d::context->PSSetShaderResources(60, 1, &none);
		globals::d3d::context->CSSetShaderResources(60, 1, &none);
	}

	/** @brief The client's commit: this frame's constants at the frame slot. The epoch it precedes carries the heightmap's copy. */
	void Commit(Impl& a_impl)
	{
		if (!std::exchange(a_impl.pendingFrame, false))
			return;
		if (a_impl.copy == Impl::Copy::Needed)
			a_impl.copy = Impl::Copy::Submitted;
		const std::uint32_t slot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		a_impl.latch->Write(slot, 0, std::as_bytes(std::span(a_impl.pending)));
	}
}

void ORGTerrainShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Terrain Shadows stays on D3D11: the EarlyPrepass anchor could not be reserved");
}

bool ORGTerrainShadows::Available() const
{
	return Requested() && !failed && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass);
}

bool ORGTerrainShadows::Active() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

void ORGTerrainShadows::Fail()
{
	failed = true;
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kShadowHeightName);
	if (impl)
		UnbindShadowHeight();
	impl.reset();
}

bool ORGTerrainShadows::SetHeightmap(const DirectX::Image& a_image)
{
	if (!Available())
		return false;
	auto* host = RenderGraphRuntime::Get().Host();
	auto next = std::make_shared<Impl>();
	next->program = impl ? impl->program : ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<ShadowUpdateBlock>() });
	if (!next->program) {
		logger::error("[ORG] Terrain Shadows: {} could not be compiled; back to D3D11", kShader);
		Fail();
		return false;
	}

	const auto width = static_cast<UINT>(a_image.width);
	const auto height = static_cast<UINT>(a_image.height);
	D3D11_TEXTURE2D_DESC desc{ .Width = width,
		.Height = height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = a_image.format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE };
	next->heightMap = std::make_unique<GraphResources::Texture2D>(desc, "TerrainShadows::HeightMap",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
	desc.Format = DXGI_FORMAT_R16G16_UNORM;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	// Engine draws read it at t60 (and Drawcall Limit Fix through that view) until they are ported.
	next->shadowHeights = std::make_unique<GraphResources::Texture2D>(desc, kShadowHeightName,
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!*next->heightMap || !*next->shadowHeights || !next->shadowHeights->SRV()) {
		logger::error("[ORG] Terrain Shadows: its {}x{} graph textures could not be created; back to D3D11", width, height);
		Fail();
		return false;
	}

	next->maxDispatches = ((std::max)(width, height) + kStripeLength - 1) / kStripeLength;
	try {
		next->latch = std::make_shared<org::LatchBlock>("TerrainShadows::UpdateLatch", next->maxDispatches * static_cast<std::uint32_t>(sizeof(Constants)), host->FrameSlots());
	} catch (const std::exception& e) {
		logger::error("[ORG] Terrain Shadows: its latch block could not be created ({}); back to D3D11", e.what());
		Fail();
		return false;
	}
	next->pending.reserve(next->maxDispatches);
	next->staging = StageHeightMap(a_image, next->stagingFootprint);
	if (!next->staging) {
		Fail();
		return false;
	}
	next->copy = Impl::Copy::Needed;

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	// Before the update, which reads the heightmap. Made only while the copy has not been submitted (Submit drops it).
	client.passes.push_back({ .name = std::string(kClientId) + ".heightmap-copy",
		.anchor = FrameAnchors::Anchor::EarlyPrepass,
		.order = 90,
		.kind = FrameAnchors::PassKind::Copy,
		.make = [next]() -> std::shared_ptr<org::RenderPass> {
			if (!next->staging)
				return nullptr;
			return std::make_shared<HeightMapCopyPass>(*next);
		} });
	client.passes.push_back({ .name = std::string(kClientId) + ".update",
		.anchor = FrameAnchors::Anchor::EarlyPrepass,
		.order = 100,
		.kind = FrameAnchors::PassKind::Compute,
		.make = [next]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowUpdatePass>(next); } });
	client.registerResources = [next](org::RenderGraph& a_graph) {
		next->heightMap->Register(a_graph);
		next->shadowHeights->Register(a_graph);
	};
	client.commit = [next](const FrameAnchors::Point&) { Commit(*next); };
	// The copy rides with the first frame's update, so the client is active only with an update to run.
	client.active = [next] { return next->pendingFrame; };
	client.onFailed = [] {
		logger::error("[ORG] Terrain Shadows: the render graph failed; back to D3D11");
		UnbindShadowHeight();
		PublishedResources::Withdraw(kShadowHeightName);
		auto& self = Get();
		self.failed = true;
		self.impl.reset();
	};
	if (!FrameAnchors::Register(std::move(client))) {
		Fail();
		return false;
	}
	// Drawcall Limit Fix's colour passes read it by name, ordered after the update (Integrations/TerrainShadows).
	if (!PublishedResources::Publish(*next->shadowHeights)) {
		logger::error("[ORG] Terrain Shadows: the shadow height map could not be published; back to D3D11");
		Fail();
		return false;
	}
	// Replacing the client dropped the previous textures: they go once D3D11 is past its last use of the old view.
	impl = std::move(next);
	logger::info("[ORG] Terrain Shadows: {}x{} heightmap on the render graph, up to {} stripes a refresh", width, height, impl->maxDispatches);
	return true;
}

bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB> a_dispatches, std::uint32_t a_groups)
{
	if (!Active() || a_dispatches.empty() || !a_groups)
		return false;
	auto& state = *impl;
	// An epoch with the heightmap's copy was submitted: the graph is built again without the copy pass, and the upload buffer goes
	// with the last build that holds it.
	if (state.copy == Impl::Copy::Submitted) {
		state.copy = Impl::Copy::None;
		state.staging.reset();
		FrameAnchors::Refresh(kClientId);
	}
	const auto count = (std::min)(static_cast<std::uint32_t>(a_dispatches.size()), state.maxDispatches);
	state.pending.assign(a_dispatches.begin(), a_dispatches.begin() + count);
	state.pendingFrame = true;
	const Shape shape{ count, a_groups };
	const auto current = state.shape.load(std::memory_order_acquire);
	if (!current || !(*current == shape))
		state.shape.store(std::make_shared<const Shape>(shape), std::memory_order_release);
	return true;
}

ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const
{
	return Active() ? impl->shadowHeights->SRV() : nullptr;
}

ORGTerrainShadows::~ORGTerrainShadows() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Terrain Shadows always runs on D3D11.
struct ORGTerrainShadows::Impl
{};

void ORGTerrainShadows::Reserve() {}
bool ORGTerrainShadows::Available() const { return false; }
bool ORGTerrainShadows::Active() const { return false; }
void ORGTerrainShadows::Fail() { failed = true; }
bool ORGTerrainShadows::SetHeightmap(const DirectX::Image&) { return false; }
bool ORGTerrainShadows::Submit(std::span<const TerrainShadows::ShadowUpdateCB>, std::uint32_t) { return false; }
ID3D11ShaderResourceView* ORGTerrainShadows::ShadowHeightSRV() const { return nullptr; }
ORGTerrainShadows::~ORGTerrainShadows() = default;

#endif
