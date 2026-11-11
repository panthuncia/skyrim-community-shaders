// The engine draw families' shared pieces (EngineDrawFamily.h).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"

#include "Globals.h"
#include "RenderGraph/Engine/MainTick.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "Utils/Logging.h"

#include <Render/PassBuilders.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace DCLF::Family
{
	namespace
	{
		struct Taker
		{
			std::string family;
			DrawTaker take = nullptr;
		};

		// Filled at install (main thread), read by the detour afterwards: never changed while the game draws.
		std::vector<Taker>& Takers()
		{
			static std::vector<Taker> takers;
			return takers;
		}

		/** @brief BSGraphics::Renderer::DrawTriShape (SE 75477, AE 77263): a family's open batch takes the draw instead. */
		struct DrawTriShape
		{
			static void thunk(void* a_renderer, TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
			{
				for (const auto& taker : Takers())
					if (taker.take(a_shape, a_start, a_triangles))
						return;
				func(a_renderer, a_shape, a_start, a_triangles);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The depth bias viewport offsets (BSGraphics::CreateDepthBiasViewportOffsets, SE 0x141794530, AE 0x1419db270): a bias mode takes
		// its entry off the viewport's maximum depth.
		constexpr std::array<float, 12> kBiasViewportOffsets{ 0.0f, 1.5e-5f, 0.0f, 0.0f, 0.0f, 0.0f, 2.6e-5f, 1.5e-5f, 2.8e-5f, 1.7e-5f, 3e-5f, 1.9e-5f };

		bool IsBufferView(ID3D11ShaderResourceView* a_view)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			return desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER || desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX;
		}
	}

	bool AddDrawTaker(std::string_view a_family, DrawTaker a_taker)
	{
		if (!Address::kDrawTriShape.Known()) {
			logger::warn("[DCLF] {}: this runtime has no address for DrawTriShape; its draws stay the engine's", a_family);
			return false;
		}
		auto& takers = Takers();
		if (takers.empty()) {
			stl::detour_thunk<DrawTriShape>(REL::Offset(Address::kDrawTriShape).address());
			HookSites::NoteDetour("BSGraphics::Renderer::DrawTriShape (the engine draw families)", DrawTriShape::func.address());
		}
		takers.push_back({ std::string(a_family), a_taker });
		logger::info("[DCLF] {}: takes its draws at the engine draw families' DrawTriShape detour ({} families)", a_family, takers.size());
		return true;
	}

	bool DrawTakersInstalled()
	{
		return !Takers().empty();
	}

	bool AddSetup(std::string_view a_family, std::function<bool()> a_setup)
	{
		auto done = std::make_shared<bool>(false);
		const bool added = ::RenderGraph::Engine::MainTick::Add(std::format("the {} family's setup", a_family), [done, setup = std::move(a_setup)] {
			if (!*done)
				*done = setup();
		});
		if (!added)
			logger::error("[DCLF] {}: the main-thread tick could not be installed; the family's clients are never made and its draws stay the engine's", a_family);
		return added;
	}

	void BeginBatch()
	{
		static std::uint32_t last = ~0u;
		const std::uint32_t frame = GameFrame();
		if (frame == last)
			return;
		last = frame;
		GpuResources::Frame().BeginFrame();
	}

	std::uint32_t GameFrame()
	{
		static REL::Relocation<std::uint32_t*> counter{ REL::Offset(Engine::Address::kFrameCounter) };
		return *counter;
	}

	bool Passes::OnSetup(const RE::BSRenderPass* a_pass)
	{
		++passes;
		if (pendingType != kNone)
			otherTypes |= 1u << (pendingType & 31);
		pendingType = a_pass && a_pass->geometry ? static_cast<std::uint32_t>(a_pass->geometry->GetType().underlying()) : 0xFE;
		// R1's dispatcher draws a TriShape through DrawTriShape (+0x1A2); every other type through another draw function.
		return pendingType == static_cast<std::uint32_t>(RE::BSGeometry::Type::kTriShape);
	}

	bool Passes::End()
	{
		if (pendingType != kNone)
			otherTypes |= 1u << (pendingType & 31);
		pendingType = kNone;
		return drawn >= passes && !otherTypes;
	}

	std::string Passes::Describe(std::string_view a_what) const
	{
		return std::format("{} of {} {} passes drew without DrawTriShape (geometry types {:#x}, R1's dispatcher)", passes > drawn ? passes - drawn : 0, passes,
			a_what, otherTypes);
	}

	const char* ReasonOf(GeometryResult a_result)
	{
		switch (a_result) {
		case GeometryResult::Missing:
			return "no TriShape buffers";
		case GeometryResult::Topology:
			return "its topology is not a triangle list";
		case GeometryResult::Buffers:
			return "its vertex or index buffer could not be imported";
		case GeometryResult::Bounds:
			return "its index range or vertex stride is out of its buffers' bounds";
		default:
			return "none";
		}
	}

	GeometryResult LatchGeometry(const TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::uint64_t a_shaderVertexDesc,
		std::uint32_t a_topology, Geometry& a_out, Owners& a_owners, std::string* a_detail)
	{
		if (!a_shape || !a_shape->vertexBuffer || !a_shape->indexBuffer)
			return GeometryResult::Missing;
		if (a_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
			if (a_detail)
				*a_detail = std::format("topology {}", a_topology);
			return GeometryResult::Topology;
		}
		auto& buffers = GpuResources::Frame();
		auto vertices = buffers.Acquire(a_shape->vertexBuffer);
		auto indices = buffers.Acquire(a_shape->indexBuffer);
		if (!vertices || !indices || !vertices->buffer.address || !indices->buffer.address)
			return GeometryResult::Buffers;
		// The draw's 16-bit indices must lie in the index buffer, and a vertex must hold at least the position the stage pulls. The index
		// stage loads the 4-byte word holding each index, so an odd range's last word reaches 2 bytes past the buffer's end: inside its
		// allocation, which DXVK pads to 4 bytes at least (its buffer slices are aligned; a dedicated allocation to 256 bytes). So the
		// range is checked to its last byte, not its last word: a mesh with an odd index count (the cloud layers have them) is drawable.
		const std::uint32_t stride = static_cast<std::uint32_t>(a_shape->vertexDesc & 0xF) * 4;
		const std::uint64_t endIndexByte = (std::uint64_t(a_start) + std::uint64_t(a_triangles) * 3) * 2;
		if (!a_triangles || stride < 16 || endIndexByte > indices->buffer.size || vertices->buffer.size < stride) {
			if (a_detail)
				*a_detail = std::format("first index {}, {} indices ({} bytes to its end) of a {}-byte index buffer; stride {} (VertexDesc {:#x}) in a {}-byte "
										"vertex buffer ({} vertices)",
					a_start, std::uint64_t(a_triangles) * 3, endIndexByte, indices->buffer.size, stride, a_shape->vertexDesc, vertices->buffer.size,
					stride ? vertices->buffer.size / stride : 0);
			return GeometryResult::Bounds;
		}
		a_out.vertexAddress = vertices->buffer.address;
		a_out.indexAddress = indices->buffer.address;
		a_out.layout = a_shaderVertexDesc & a_shape->vertexDesc;
		a_out.stride = stride;
		a_out.firstIndex = a_start;
		a_out.indexCount = a_triangles * 3;
		a_owners.push_back(std::move(vertices->owner));
		a_owners.push_back(std::move(indices->owner));
		return GeometryResult::Ok;
	}

	Viewport LatchViewport(std::uint32_t a_depthBias)
	{
		static REL::Relocation<const float*> cameraDepthRange{ REL::Offset(Engine::Address::kCameraDepthRange) };
		const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		Viewport viewport;
		viewport.rect = { static_cast<std::uint32_t>(rss.viewPort.TopLeftX), static_cast<std::uint32_t>(rss.viewPort.TopLeftY),
			static_cast<std::uint32_t>(rss.viewPort.Width), static_cast<std::uint32_t>(rss.viewPort.Height) };
		viewport.depthRange[0] = cameraDepthRange.get()[0];
		viewport.depthRange[1] = cameraDepthRange.get()[1] - kBiasViewportOffsets[(std::min)(a_depthBias, 11u)];
		return viewport;
	}

	Constants::UseId CaptureConstants()
	{
		return Constants::Active() ? Constants::Capture(Constants::kDrawStages) : Constants::UseId{};
	}

	std::uint32_t ConstantBytes(Constants::UseId a_use, Constants::Stage a_stage, std::uint32_t a_slot)
	{
		const auto* slot = a_use.Valid() ? Constants::Find(a_use, a_stage, a_slot) : nullptr;
		return slot ? slot->size : 0;
	}

	bool CopyConstants(Constants::UseId a_use, Constants::Stage a_stage, std::uint32_t a_slot, std::span<std::byte> a_out)
	{
		std::memset(a_out.data(), 0, a_out.size());
		if (!a_use.Valid())
			return false;
		const auto* slot = Constants::Find(a_use, a_stage, a_slot);
		if (!slot)
			return true;
		const auto bytes = Constants::Bytes(*slot);
		std::memcpy(a_out.data(), bytes.data(), (std::min)(bytes.size(), a_out.size()));
		return true;
	}

	SlotResult SlotTexture(ID3D11ShaderResourceView* a_view, GpuTextures::ViewKind a_kind, std::uint32_t a_tag, std::uint32_t& a_index, Owners& a_owners,
		GpuTextures::ResolveFailure* a_failure)
	{
		auto& textures = GpuTextures::Get();
		if (a_view && !PublishedResources::ExportedId(a_view).empty())
			return SlotResult::Published;
		auto binding = a_view ? textures.ResolveBinding(a_view, a_tag, a_failure) : textures.NullOf(a_kind);
		if (binding.index == GpuTextures::kInvalid)
			return SlotResult::Failed;
		a_index = binding.index;
		if (binding.owner)
			a_owners.push_back(std::move(binding.owner));
		return SlotResult::Ok;
	}

	std::string EngineTargetId(RE::RENDER_TARGET a_target)
	{
		return std::format("Engine::{}", magic_enum::enum_name(a_target));
	}

	std::string EngineTargetId(RE::RENDER_TARGET_DEPTHSTENCIL a_target)
	{
		return std::format("Engine::Depth::{}", magic_enum::enum_name(a_target));
	}

	std::string EngineTargetId(RE::RENDER_TARGET_CUBEMAP a_target)
	{
		return std::format("Engine::Cubemap::{}", magic_enum::enum_name(a_target));
	}

	std::vector<std::string> RegisteredIdsAt(std::span<const std::uint32_t> a_registers)
	{
		std::vector<std::string> ids;
		const auto add = [&](std::string_view a_id) {
			if (!a_id.empty() && std::ranges::find(ids, a_id) == ids.end())
				ids.emplace_back(a_id);
		};
		for (const auto& texture : Extensions::RegisteredFrameTextures())
			if (std::ranges::find(a_registers, texture.textureRegister) != a_registers.end())
				for (const auto id : texture.graphResources)
					add(id);
		for (const auto& buffers : Extensions::RegisteredFrameGraphBuffers())
			for (std::uint32_t i = 0; i < buffers.count; ++i)
				if (std::ranges::find(a_registers, buffers.firstRegister + i) != a_registers.end())
					add(buffers.resources[i]);
		return ids;
	}

	const char* ReasonOf(InputResult a_result)
	{
		switch (a_result) {
		case InputResult::NotListed:
			return "it is not among the view's published inputs";
		case InputResult::PublishedLate:
			return "it was first published after its client's graph was built (its producer publishes during play)";
		case InputResult::NoView:
			return "it has no whole-resource shader resource view";
		default:
			return "none";
		}
	}

	void PublishedInputs::Add(std::string_view a_id)
	{
		if (!a_id.empty() && std::ranges::find(ids, a_id) == ids.end())
			ids.emplace_back(a_id);
	}

	void PublishedInputs::Add(std::span<const std::string> a_ids)
	{
		for (const auto& id : a_ids)
			Add(id);
	}

	void PublishedInputs::AddEngine(RE::RENDER_TARGET a_target)
	{
		FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_target));
		Add(EngineTargetId(a_target));
	}

	void PublishedInputs::AddEngine(RE::RENDER_TARGET_DEPTHSTENCIL a_target)
	{
		FrameAnchors::EngineDepth(static_cast<std::uint32_t>(a_target));
		Add(EngineTargetId(a_target));
	}

	void PublishedInputs::AddEngine(RE::RENDER_TARGET_CUBEMAP a_target)
	{
		FrameAnchors::EngineCubemap(static_cast<std::uint32_t>(a_target));
		Add(EngineTargetId(a_target));
	}

	void PublishedInputs::Declare(org::RenderPassBuilder& a_builder)
	{
		const std::span<const org::SrvView> noViews{};
		const auto known = declared.load(std::memory_order_acquire);
		std::vector<std::string_view> added;
		for (const auto& id : ids) {
			// Only ids every graph registers: declaring another would fail the build.
			if (!PublishedResources::IsRegistered(id))
				continue;
			a_builder.ShaderResource(org::ResourceIdentifier(id), noViews);
			if (!known || !known->contains(id))
				added.push_back(id);
		}
		if (added.empty())
			return;
		auto next = known ? std::make_shared<PublishedIds>(*known) : std::make_shared<PublishedIds>();
		for (const auto id : added)
			next->emplace(id);
		declared.store(std::move(next), std::memory_order_release);
	}

	InputResult PublishedInputs::Resolve(std::string_view a_id, std::uint32_t& a_index) const
	{
		if (a_id.empty() || std::ranges::find(ids, a_id) == ids.end())
			return InputResult::NotListed;
		// The declared check comes first: an undeclared id must never be read (nothing orders the read after its writes).
		const auto known = declared.load(std::memory_order_acquire);
		if (!known || !known->contains(a_id))
			return InputResult::PublishedLate;
		return PublishedResources::ShaderResourceIndex(a_id, a_index) ? InputResult::Graph : InputResult::NoView;
	}

	std::string PublishedInputs::Describe() const
	{
		std::string out;
		for (const auto& id : ids)
			out += std::format("{}{}", out.empty() ? "" : ", ", id);
		return out.empty() ? std::string("none") : out;
	}

	std::string PublishedInputs::Unregistered() const
	{
		std::string out;
		for (const auto& id : ids)
			if (!PublishedResources::IsRegistered(id))
				out += std::format("{}{}", out.empty() ? "" : ", ", id);
		return out;
	}

	std::uint32_t SlotSampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode, Owners& a_owners)
	{
		auto sampler = GpuTextures::Get().SamplerBinding(a_addressMode, a_filterMode);
		if (sampler.owner)
			a_owners.push_back(std::move(sampler.owner));
		return sampler.index;
	}

	HeaderResult ResolveHeader(std::uint32_t a_register, GpuTextures::ViewKind a_kind, std::uint32_t a_tag, Header& a_out, Owners& a_owners,
		GpuTextures::ResolveFailure* a_failure, ID3D11ShaderResourceView** a_view)
	{
		a_out = {};
		// Only CS's registers: the engine's t0-t15 are latched under the flush latch and read from the shadow state (SlotTexture).
		if (a_register < 16)
			return HeaderResult::Texture;
		ID3D11ShaderResourceView* view = nullptr;
		globals::d3d::context->PSGetShaderResources(a_register, 1, &view);
		if (a_view)
			*a_view = view;
		if (!view) {
			a_out.index = GpuTextures::Get().NullOf(a_kind).index;
			return a_out.index != GpuTextures::kInvalid ? HeaderResult::Ok : HeaderResult::Texture;
		}
		// The binding keeps the view alive for the batch; the import keeps what the run reads.
		view->Release();
		a_out.publishedId = PublishedResources::ExportedId(view);
		if (!a_out.publishedId.empty())
			return HeaderResult::Ok;
		if (IsBufferView(view))
			return HeaderResult::Buffer;
		auto binding = GpuTextures::Get().ResolveBinding(view, a_tag, a_failure);
		if (binding.index == GpuTextures::kInvalid)
			return HeaderResult::Texture;
		a_out.index = binding.index;
		if (binding.owner)
			a_owners.push_back(std::move(binding.owner));
		return HeaderResult::Ok;
	}

	std::uint32_t KeptFrames()
	{
		return RenderGraphRuntime::kGameFramesInFlight + 2;
	}

	bool RunWithheld(std::string_view a_family, std::string_view a_client, FrameAnchors::Anchor a_anchor, bool& a_pending)
	{
		a_pending = true;
		const auto result = FrameAnchors::Run(a_anchor);
		a_pending = false;
		if (result)
			return true;
		const char* why = "ran";
		switch (result.status) {
		case FrameAnchors::RunStatus::Idle:
			why = "the anchor had nothing to run";
			break;
		case FrameAnchors::RunStatus::Refused:
			why = "the anchor refused the run (not the render owner, or no runtime)";
			break;
		case FrameAnchors::RunStatus::RingFull:
			why = "the anchor's ring was full";
			break;
		case FrameAnchors::RunStatus::Failed:
			why = "the anchor's epoch failed";
			break;
		default:
			break;
		}
		static Util::ReasonLog log("[DCLF] engine draw families: latched draws were not drawn this frame (their anchor's run did not submit)", spdlog::level::err);
		log.Report(std::format("{} {}: {}", a_family, a_client, why));
		return false;
	}

	void KeptOwners::Keep(Owners&& a_owners)
	{
		const std::uint32_t frame = GameFrame();
		while (!kept.empty() && frame - kept.front().first > KeptFrames())
			kept.pop_front();
		if (!a_owners.empty())
			kept.emplace_back(frame, std::move(a_owners));
	}
}
