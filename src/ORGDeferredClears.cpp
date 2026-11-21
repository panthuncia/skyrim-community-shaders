#include "ORGDeferredClears.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Deferred.h"
#include "Globals.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"

#include <Render/PassBuilders.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace
{
	constexpr const char* kClientId = "cs.deferred.clears";

	// StartDeferred binds these with SRTM_CLEAR at slots 2-7.
	constexpr std::array<RE::RENDER_TARGET, 6> kTargets{ NORMALROUGHNESS, ALBEDO, SPECULAR, REFLECTANCE, MASKS, MASKS2 };

	// Last, since on D3D11 the clears come at the first state flush after every feature's Prepass.
	constexpr std::int32_t kOrder = std::numeric_limits<std::int32_t>::max();

	// The passes read this state, so Impl only marks that setup finished.
	struct Shared
	{
		// As float bit patterns. The render thread writes and the host's thread reads.
		std::array<std::atomic<std::uint32_t>, 4> colour{};
		// Recorded at execution, so a run that left it closed skips.
		std::shared_ptr<RenderGraph::RunGate> gate;
	};

	Shared& SharedState()
	{
		static Shared s_shared;
		return s_shared;
	}

	std::array<float, 4> CurrentColour()
	{
		std::array<float, 4> colour{};
		for (std::size_t i = 0; i < colour.size(); ++i) {
			const std::uint32_t bits = SharedState().colour[i].load(std::memory_order_relaxed);
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

	/** @brief Loads each target as a clear and draws nothing. */
	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
	{
	public:
		explicit ClearPass(std::array<FrameAnchors::Texture, kTargets.size()> a_targets) :
			targets(std::move(a_targets)) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			ClearBindings bindings{};
			for (std::size_t i = 0; i < targets.size(); ++i)
				bindings.targets[i] = a_builder.RenderTarget(targets[i]).View();
			return bindings;
		}

		// The colour is recorded into the pass, so a new one re-records it.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			for (const auto& component : SharedState().colour)
				a_out.push_back(component.load(std::memory_order_relaxed));
		}

		// The size of the versions the preparation binds: a resize is a new version of each target, and a revision may be prepared
		// while the old ones are adopted. The extent follows the bound versions, which a kept recording checks itself.
		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext& a_context) const
		{
			const auto size = targets[0].PreparedSize(a_context);
			for (std::size_t i = 1; i < targets.size(); ++i) {
				const auto other = targets[i].PreparedSize(a_context);
				if (other.width != size.width || other.height != size.height)
					RenderGraph::Fatal(ORGDeferredClears::kName, "the GBuffer targets differ in size");
			}
			return { CurrentColour(), size.width, size.height };
		}

		// A kept late batch keeps its list while SkipsAtExecution's answer holds.
		static constexpr bool kRecordKeyedBySkip = true;

		// A run that left the gate closed records only barriers.
		static bool SkipsAtExecution(const ClearBindings&, const ClearFrame&, const org::PassRecordContext& a_recording)
		{
			const auto& gate = SharedState().gate;
			return gate && !gate->IsOpen(a_recording.Run(), 0);
		}

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
		std::array<FrameAnchors::Texture, kTargets.size()> targets;
	};
}

struct ORGDeferredClears::Impl
{
	// Registered at the first revision start, which waits for the stage builds that hold it.
	RenderGraph::Frame::DeclaredClient client;
};

ORGDeferredClears::ORGDeferredClears() = default;
ORGDeferredClears::~ORGDeferredClears() = default;

void ORGDeferredClears::Setup()
{
	if (!BeginSetup())
		return;
	auto& shared = SharedState();
	if (!shared.gate)
		shared.gate = RenderGraph::RunGate::Create(kClientId, 1);
	const auto make = [](FrameAnchors::ClientDesc& client) {  // fills the registration
		client.passes.push_back({ .name = std::string(kClientId) + ".gbuffer",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = kOrder,
			.kind = FrameAnchors::PassKind::Render,
			.make = []() -> std::shared_ptr<org::RenderPass> {
				// The targets' slots. Their sizes are read in Prepare, so a resize needs no new pass.
				std::array<FrameAnchors::Texture, kTargets.size()> targets;
				for (std::size_t i = 0; i < kTargets.size(); ++i) {
					targets[i] = FrameAnchors::EngineTexture(FrameAnchors::Colour(kTargets[i]));
					if (!targets[i])
						Fatal(fmt::format("GBuffer target {} cannot be imported", static_cast<std::uint32_t>(kTargets[i])));
				}
				return std::make_shared<ClearPass>(std::move(targets));
			},
			.atExecution = true });
		client.gates = { SharedState().gate };
		client.commit = [](const FrameAnchors::Point&) {
			SharedState().gate->OpenDispatch(0, 1);
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return;
			const auto& colour = renderer->GetRendererData().clearColor;
			for (std::size_t i = 0; i < 4; ++i) {
				std::uint32_t bits = 0;
				std::memcpy(&bits, &colour[i], sizeof(bits));
				SharedState().colour[i].store(bits, std::memory_order_relaxed);
			}
		};
	};
	auto next = std::make_unique<Impl>();
	// The client never changes, so its key is constant.
	next->client = RenderGraph::Frame::DeclareClient<bool>(
		std::string(kClientId), {}, [](const RenderGraph::Frame::Inputs&) -> std::optional<bool> { return true; },
		[make](const bool&, FrameAnchors::ClientDesc& a_out) {
			make(a_out);
			return true;
		});
	impl = std::move(next);

	logger::info("[ORG] Deferred clears: the GBuffer targets are cleared at the Prepass anchor");
}

bool ORGDeferredClears::TakeClears() const
{
	return impl && FrameAnchors::Place(kClientId, FrameAnchors::Anchor::Prepass).draws;
}

