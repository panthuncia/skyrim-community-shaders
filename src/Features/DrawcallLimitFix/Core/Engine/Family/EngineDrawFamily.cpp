// The engine draw families' shared pieces (EngineDrawFamily.h).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"

#include "Globals.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"

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

	void Passes::OnSetup(const RE::BSRenderPass* a_pass)
	{
		++passes;
		if (pendingType != kNone)
			otherTypes |= 1u << (pendingType & 31);
		pendingType = a_pass && a_pass->geometry ? static_cast<std::uint32_t>(a_pass->geometry->GetType().underlying()) : 0xFE;
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
		std::uint32_t a_topology, Geometry& a_out, Owners& a_owners)
	{
		if (!a_shape || !a_shape->vertexBuffer || !a_shape->indexBuffer)
			return GeometryResult::Missing;
		if (a_topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
			return GeometryResult::Topology;
		auto& buffers = GpuResources::Frame();
		auto vertices = buffers.Acquire(a_shape->vertexBuffer);
		auto indices = buffers.Acquire(a_shape->indexBuffer);
		if (!vertices || !indices || !vertices->buffer.address || !indices->buffer.address)
			return GeometryResult::Buffers;
		// The draw's 16-bit indices must lie in the index buffer, and a vertex must hold at least the position the stage pulls. The
		// index stage loads 4-byte words, so the last one may reach 2 bytes past the range.
		const std::uint32_t stride = static_cast<std::uint32_t>(a_shape->vertexDesc & 0xF) * 4;
		const std::uint64_t lastIndexByte = (std::uint64_t(a_start) + std::uint64_t(a_triangles) * 3) * 2;
		if (!a_triangles || stride < 16 || ((lastIndexByte + 3) & ~std::uint64_t(3)) > indices->buffer.size || vertices->buffer.size < stride)
			return GeometryResult::Bounds;
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

	bool CopyConstants(const ID3D11Buffer* a_buffer, std::span<std::byte> a_out)
	{
		std::memset(a_out.data(), 0, a_out.size());
		if (!a_buffer)
			return true;
		const auto bytes = ConstantMirror::Get().Contents(a_buffer);
		if (bytes.empty())
			return false;
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

	void KeptOwners::Keep(Owners&& a_owners)
	{
		const std::uint32_t frame = GameFrame();
		while (!kept.empty() && frame - kept.front().first > KeptFrames())
			kept.pop_front();
		if (!a_owners.empty())
			kept.emplace_back(frame, std::move(a_owners));
	}
}
