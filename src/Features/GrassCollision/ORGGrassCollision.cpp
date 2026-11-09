#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "ORGGrassCollision.h"

#include <algorithm>
#include <cstring>
#include <vector>

ORGGrassCollision& ORGGrassCollision::Get()
{
	static ORGGrassCollision s_instance;
	return s_instance;
}

bool ORGGrassCollision::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_GRASS_COLLISION", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>

namespace
{
	constexpr const char* kClientId = "cs.grass-collision";
	constexpr const char* kShader = "GrassCollision/CollisionUpdateCS.hlsl";
	// The anchor's keys (org-extension-api-notes.md, "Attaching passes to the frame"): the copy, then the update.
	constexpr std::int32_t kOrder = 200;
	// CollisionUpdateCS's thread groups are 8x8.
	constexpr std::uint32_t kGroupSize = 8;

	// CollisionUpdateCS's push block (COLLISION_UPDATE_RESOURCES, in list order).
	struct CollisionUpdateBlock
	{
		ShaderBindings::Handle boundingBoxes, instances, collision;
		ShaderBindings::Latch perFrame;
	};

	constexpr std::uint32_t AlignRegion(std::uint32_t a_bytes) { return (a_bytes + 255u) & ~255u; }

	// The latch block's layout in each slot: the per-frame values, the bounding boxes, the collision spheres.
	struct Layout
	{
		std::uint32_t boxesOffset = 0;
		std::uint32_t boxesBytes = 0;
		std::uint32_t instancesOffset = 0;
		std::uint32_t instancesBytes = 0;
		std::uint32_t slotBytes = 0;
	};

	struct Resources
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<org::LatchBlock> latch;
		Layout layout;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::unique_ptr<GraphResources::StructuredBuffer> boxes;
		std::unique_ptr<GraphResources::StructuredBuffer> instances;
		std::unique_ptr<GraphResources::Texture2D> collision;
	};

	// The copy: the latch's boxes and spheres into the graph buffers the update reads. Whole capacities: the recording is kept from
	// frame to frame, and the counts that bound the reads are latched.
	struct CopyBindings
	{
		org::ResourceBindingToken boxes, instances;
	};

	struct CopyFrame
	{
		std::shared_ptr<const org::LatchBlock> latch;
		Layout layout;
	};

	class CopyCollidersPass final : public org::TypedRenderGraphPass<CopyCollidersPass, CopyFrame, CopyBindings>
	{
	public:
		explicit CopyCollidersPass(const Resources& a_resources) :
			latch(a_resources.latch), layout(a_resources.layout), boxes(a_resources.boxes->Graph()), instances(a_resources.instances->Graph()) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(boxes), a_builder.CopyDestination(instances) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(boxes.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(instances.get()));
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { latch, layout }; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.latch)
				return;
			auto& commands = a_recording.Commands();
			const auto source = a_frame.latch->Resource()->GetAPIResource().GetHandle();
			const auto base = a_frame.latch->Offset(a_recording.FrameSlot());
			commands.CopyBufferRegion(a_recording.Resolve(a_bindings.boxes).GetHandle(), 0, source, base + a_frame.layout.boxesOffset, a_frame.layout.boxesBytes);
			commands.CopyBufferRegion(a_recording.Resolve(a_bindings.instances).GetHandle(), 0, source, base + a_frame.layout.instancesOffset,
				a_frame.layout.instancesBytes);
		}

	private:
		std::shared_ptr<const org::LatchBlock> latch;
		Layout layout;
		std::shared_ptr<org::Resource> boxes;
		std::shared_ptr<org::Resource> instances;
	};

	// The update: CollisionUpdateCS over the whole texture, which it reads (the last frame's) and writes in place.
	struct UpdateBindings
	{
		org::DeclaredViewToken boxes, instances, collision;
	};

	struct UpdateFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	class UpdateCollisionPass final : public org::TypedRenderGraphPass<UpdateCollisionPass, UpdateFrame, UpdateBindings>
	{
	public:
		explicit UpdateCollisionPass(const Resources& a_resources) :
			program(a_resources.program), latch(a_resources.latch), boxes(a_resources.boxes->Graph()), instances(a_resources.instances->Graph()),
			collision(a_resources.collision->Graph()), width(a_resources.width), height(a_resources.height) {}

		UpdateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.ShaderResource(boxes).View(), a_builder.ShaderResource(instances).View(), a_builder.UnorderedAccess(collision).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(collision.get()));
		}

		UpdateFrame Prepare(const UpdateBindings&, const org::PassPrepareContext&) const { return { program, latch, width, height }; }

		static void Record(const UpdateBindings& a_bindings, const UpdateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			const CollisionUpdateBlock block{ a_recording.Index(a_bindings.boxes), a_recording.Index(a_bindings.instances),
				a_recording.Index(a_bindings.collision), ShaderBindings::LatchOf(*a_frame.latch, a_recording.FrameSlot()) };
			a_frame.program->Dispatch(a_recording, ShaderBindings::Data(block), a_frame.width / kGroupSize, a_frame.height / kGroupSize, 1);
		}

	private:
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<org::Resource> boxes;
		std::shared_ptr<org::Resource> instances;
		std::shared_ptr<org::Resource> collision;
		std::uint32_t width;
		std::uint32_t height;
	};

	D3D11_BUFFER_DESC StructuredDesc(std::uint32_t a_count, std::uint32_t a_stride)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = a_count * a_stride;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = a_stride;
		return desc;
	}
}

struct ORGGrassCollision::Impl
{
	std::shared_ptr<Resources> resources;
	Limits limits;
	// The staged frame, laid out as a latch slot: the commit writes the regions in use.
	std::vector<std::byte> staged;
	std::uint32_t perFrameBytes = 0;
	std::uint32_t boxesBytes = 0;
	std::uint32_t instancesBytes = 0;
	std::function<void()> onFailed;
};

ORGGrassCollision::~ORGGrassCollision() = default;

void ORGGrassCollision::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Grass Collision: the EarlyPrepass anchor could not be reserved; Grass Collision stays on D3D11");
}

bool ORGGrassCollision::Setup(const Limits& a_limits, std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::warn("[ORG] Grass Collision: the render graph or its EarlyPrepass anchor is not available; Grass Collision stays on D3D11");
		return false;
	}
	auto resources = std::make_shared<Resources>();
	auto& layout = resources->layout;
	layout.boxesOffset = AlignRegion(a_limits.perFrameBytes);
	layout.boxesBytes = a_limits.maxBoxes * a_limits.boxStride;
	layout.instancesOffset = layout.boxesOffset + AlignRegion(layout.boxesBytes);
	layout.instancesBytes = a_limits.maxInstances * a_limits.instanceStride;
	layout.slotBytes = layout.instancesOffset + layout.instancesBytes;
	resources->width = a_limits.texture.Width;
	resources->height = a_limits.texture.Height;
	resources->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<CollisionUpdateBlock>() });
	try {
		resources->latch = std::make_shared<org::LatchBlock>("GrassCollision::Latch", layout.slotBytes, host->FrameSlots());
	} catch (const std::exception& e) {
		logger::error("[ORG] Grass Collision: the latch block could not be made ({})", e.what());
	}
	// Written by the copy before the update reads them, every frame: transient.
	resources->boxes = std::make_unique<GraphResources::StructuredBuffer>(StructuredDesc(a_limits.maxBoxes, a_limits.boxStride), a_limits.maxBoxes,
		"GrassCollision::BoundingBoxes");
	resources->instances = std::make_unique<GraphResources::StructuredBuffer>(StructuredDesc(a_limits.maxInstances, a_limits.instanceStride),
		a_limits.maxInstances, "GrassCollision::Instances");
	// Read back by the next frame's update, and by D3D11 (the engine's grass, DCLF's grass) until those draws are the graph's.
	resources->collision = std::make_unique<GraphResources::Texture2D>(a_limits.texture, "GrassCollision::Collision",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
	if (!resources->program || !resources->latch || !*resources->boxes || !*resources->instances || !*resources->collision ||
		!resources->collision->SRV()) {
		logger::error("[ORG] Grass Collision: its program, latch block or graph resources could not be made; Grass Collision stays on D3D11");
		return false;
	}

	auto state = std::make_unique<Impl>();
	state->resources = resources;
	state->limits = a_limits;
	state->staged.resize(layout.slotBytes);
	state->onFailed = std::move(a_onFailed);

	auto* self = this;
	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes.push_back({ .name = "cs.grass-collision.copy-colliders",
		.anchor = FrameAnchors::Anchor::EarlyPrepass,
		.order = kOrder,
		.kind = FrameAnchors::PassKind::Copy,
		.make = [resources]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<CopyCollidersPass>(*resources); } });
	client.passes.push_back({ .name = "cs.grass-collision.update",
		.anchor = FrameAnchors::Anchor::EarlyPrepass,
		.order = kOrder + 1,
		.kind = FrameAnchors::PassKind::Compute,
		.make = [resources]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<UpdateCollisionPass>(*resources); } });
	client.registerResources = [resources](org::RenderGraph& a_graph) {
		resources->boxes->Register(a_graph);
		resources->instances->Register(a_graph);
		resources->collision->Register(a_graph);
	};
	// The staged frame into the execution's latch region: the regions in use only, as the counts in the per-frame values bound the reads.
	client.commit = [self](const FrameAnchors::Point&) {
		const auto& current = self->impl;
		auto* graphHost = RenderGraphRuntime::Get().Host();
		if (!current || !graphHost)
			return;
		const auto& latch = *current->resources->latch;
		const auto& slotLayout = current->resources->layout;
		const std::uint32_t slot = graphHost->CurrentFrameSlot();
		const std::span<const std::byte> staged(current->staged);
		latch.Write(slot, 0, staged.subspan(0, current->perFrameBytes));
		if (current->boxesBytes)
			latch.Write(slot, slotLayout.boxesOffset, staged.subspan(slotLayout.boxesOffset, current->boxesBytes));
		if (current->instancesBytes)
			latch.Write(slot, slotLayout.instancesOffset, staged.subspan(slotLayout.instancesOffset, current->instancesBytes));
	};
	client.onFailed = [self] {
		logger::error("[ORG] Grass Collision: the render graph failed; Grass Collision returns to D3D11");
		auto failed = std::move(self->impl);
		if (failed && failed->onFailed)
			failed->onFailed();
	};
	impl = std::move(state);
	if (!FrameAnchors::Register(std::move(client))) {
		impl.reset();
		return false;
	}
	logger::info("[ORG] Grass Collision: the collision update runs on the render graph (EarlyPrepass anchor, {}x{}, {} latch bytes a slot)",
		resources->width, resources->height, layout.slotBytes);
	return true;
}

bool ORGGrassCollision::IsActive() const
{
	return impl != nullptr;
}

void ORGGrassCollision::Stage(const Frame& a_frame)
{
	if (!impl)
		return;
	auto& state = *impl;
	const auto& layout = state.resources->layout;
	const auto copy = [&](std::span<const std::byte> a_bytes, std::uint32_t a_offset, std::uint32_t a_capacity) {
		const auto bytes = static_cast<std::uint32_t>((std::min)(a_bytes.size(), static_cast<std::size_t>(a_capacity)));
		if (bytes)
			std::memcpy(state.staged.data() + a_offset, a_bytes.data(), bytes);
		return bytes;
	};
	state.perFrameBytes = copy(a_frame.perFrame, 0, state.limits.perFrameBytes);
	state.boxesBytes = copy(a_frame.boxes, layout.boxesOffset, layout.boxesBytes);
	state.instancesBytes = copy(a_frame.instances, layout.instancesOffset, layout.instancesBytes);
}

ID3D11ShaderResourceView* ORGGrassCollision::CollisionSRV() const
{
	return impl ? impl->resources->collision->SRV() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Grass Collision always updates with D3D11.
struct ORGGrassCollision::Impl
{};

ORGGrassCollision::~ORGGrassCollision() = default;
void ORGGrassCollision::Reserve() {}
bool ORGGrassCollision::Setup(const Limits&, std::function<void()>) { return false; }
bool ORGGrassCollision::IsActive() const { return false; }
void ORGGrassCollision::Stage(const Frame&) {}
ID3D11ShaderResourceView* ORGGrassCollision::CollisionSRV() const { return nullptr; }

#endif
