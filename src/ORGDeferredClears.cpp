#include "ORGDeferredClears.h"

#include "RenderGraph/Switches.h"

bool ORGDeferredClears::Requested()
{
	return !Switches::Off("CS_ORG_DEFERRED_CLEARS");
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Globals.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <Render/PassBuilders.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <cstdint>
#	include <cstring>
#	include <limits>
#	include <memory>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.deferred.clears";

	// The targets StartDeferred binds with SRTM_CLEAR at render target slots 2-7.
	constexpr std::array<RE::RENDER_TARGET, 6> kTargets{ NORMALROUGHNESS, ALBEDO, SPECULAR, REFLECTANCE, MASKS, MASKS2 };

	// Last at the anchor: on D3D11 the clears come at the first state flush after every feature's Prepass.
	constexpr std::int32_t kOrder = std::numeric_limits<std::int32_t>::max();

	struct Shared
	{
		bool setUp = false;
		// The renderer's clear colour at the last commit (render thread writes, the host's thread reads), as four float bit patterns.
		std::array<std::atomic<std::uint32_t>, 4> colour{};
	};

	Shared& Get()
	{
		static Shared s_shared;
		return s_shared;
	}

	std::array<float, 4> CurrentColour()
	{
		std::array<float, 4> colour{};
		for (std::size_t i = 0; i < colour.size(); ++i) {
			const std::uint32_t bits = Get().colour[i].load(std::memory_order_relaxed);
			std::memcpy(&colour[i], &bits, sizeof(bits));
		}
		return colour;
	}

	struct ClearBindings
	{
		std::array<org::DeclaredViewToken, kTargets.size()> targets{};
	};

	struct ClearFrame
	{
		std::array<float, 4> colour{};
		std::uint32_t width = 0, height = 0;
	};

	/** @brief One render pass over the six targets, each loaded as a clear and stored; it draws nothing. */
	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
	{
	public:
		ClearPass(std::array<std::shared_ptr<org::ExternalTextureResource>, kTargets.size()> a_targets, std::uint32_t a_width, std::uint32_t a_height) :
			targets(std::move(a_targets)), width(a_width), height(a_height) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			ClearBindings bindings{};
			for (std::size_t i = 0; i < targets.size(); ++i)
				bindings.targets[i] = a_builder.RenderTarget(targets[i]).View();
			return bindings;
		}

		// The clear colour is recorded into the pass: a new one records it again.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			for (const auto& component : Get().colour)
				a_out.push_back(component.load(std::memory_order_relaxed));
		}

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { CurrentColour(), width, height }; }

		static void Record(const ClearBindings& a_bindings, const ClearFrame& a_frame, org::PassRecordContext& a_recording)
		{
			std::array<rhi::ColorAttachment, kTargets.size()> colours{};
			for (std::size_t i = 0; i < colours.size(); ++i) {
				auto& attachment = colours[i];
				attachment.rtv = a_recording.View(a_bindings.targets[i]);
				attachment.resource = a_recording.Resolve(a_bindings.targets[i]).GetHandle();
				attachment.loadOp = rhi::LoadOp::Clear;
				attachment.storeOp = rhi::StoreOp::Store;
				attachment.clear.type = rhi::ClearValueType::Color;
				std::copy(a_frame.colour.begin(), a_frame.colour.end(), attachment.clear.rgba);
			}
			rhi::PassBeginInfo begin{};
			begin.colors = { colours.data(), static_cast<std::uint32_t>(colours.size()) };
			begin.width = a_frame.width;
			begin.height = a_frame.height;
			begin.debugName = "Deferred GBuffer clears";
			auto& commands = a_recording.Commands();
			commands.BeginPass(begin);
			commands.EndPass();
		}

	private:
		std::array<std::shared_ptr<org::ExternalTextureResource>, kTargets.size()> targets;
		std::uint32_t width = 0, height = 0;
	};
}

namespace ORGDeferredClears
{
	void Reserve()
	{
		if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass))
			logger::warn("[ORG] Deferred clears: the Prepass anchor could not be reserved; the engine clears the GBuffer");
	}

	void Setup()
	{
		auto& shared = Get();
		if (!Requested() || shared.setUp)
			return;
		if (!RenderGraphRuntime::Get().IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
			logger::info("[ORG] Deferred clears: the render graph or its Prepass anchor is not available; the engine clears the GBuffer");
			return;
		}
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes.push_back({ .name = std::string(kClientId) + ".gbuffer",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = kOrder,
			.kind = FrameAnchors::PassKind::Render,
			.make = []() -> std::shared_ptr<org::RenderPass> {
				// The targets as they are at this build: a resize makes new imports, which build the graph again.
				std::array<std::shared_ptr<org::ExternalTextureResource>, kTargets.size()> targets;
				for (std::size_t i = 0; i < kTargets.size(); ++i) {
					targets[i] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(kTargets[i]));
					if (!targets[i]) {
						logger::warn("[ORG] Deferred clears: GBuffer target {} cannot be imported; the engine clears the GBuffer", static_cast<std::uint32_t>(kTargets[i]));
						return nullptr;
					}
					if (targets[i]->GetWidth() != targets[0]->GetWidth() || targets[i]->GetHeight() != targets[0]->GetHeight()) {
						logger::warn("[ORG] Deferred clears: the GBuffer targets differ in size; the engine clears the GBuffer");
						return nullptr;
					}
				}
				const std::uint32_t width = targets[0]->GetWidth(), height = targets[0]->GetHeight();
				return std::make_shared<ClearPass>(std::move(targets), width, height);
			} });
		client.commit = [](const FrameAnchors::Point&) {
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return;
			const auto& colour = renderer->GetRendererData().clearColor;
			for (std::size_t i = 0; i < 4; ++i) {
				std::uint32_t bits = 0;
				std::memcpy(&bits, &colour[i], sizeof(bits));
				Get().colour[i].store(bits, std::memory_order_relaxed);
			}
		};
		client.onFailed = [] {
			Get().setUp = false;
			logger::error("[ORG] Deferred clears: the render graph failed with the clears in it; the engine clears the GBuffer");
		};
		if (!FrameAnchors::Register(std::move(client))) {
			logger::warn("[ORG] Deferred clears: the client could not be registered; the engine clears the GBuffer");
			return;
		}
		shared.setUp = true;
		FrameAnchors::RequestJoin(kClientId, FrameAnchors::Anchor::Prepass);
		logger::info("[ORG] Deferred clears: the GBuffer targets are cleared at the Prepass anchor (CS_ORG_DEFERRED_CLEARS=0 leaves them to the engine)");
	}

	bool TakeClears()
	{
		return Get().setUp && FrameAnchors::CanRun(kClientId, FrameAnchors::Anchor::Prepass);
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace ORGDeferredClears
{
	void Reserve() {}
	void Setup() {}
	bool TakeClears() { return false; }
}

#endif
