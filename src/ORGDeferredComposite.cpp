#include "ORGDeferredComposite.h"

#include "RenderGraph/Switches.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RenderGraphRuntime.h"

bool ORGDeferredComposite::Requested()
{
	return Switches::On("CS_ORG_COMPOSITE");
}

ORGDeferredComposite& ORGDeferredComposite::Get()
{
	static ORGDeferredComposite s_composite;
	return s_composite;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/DynamicCubemaps.h"
#	include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#	include "Features/IBL.h"
#	include "Features/IBL/ORGIBL.h"
#	include "Features/ScreenSpaceGI.h"
#	include "Features/ScreenSpaceGI/ORGScreenSpaceGI.h"
#	include "Features/Skylighting.h"
#	include "Features/Skylighting/ORGSkylighting.h"
#	include "Features/TerrainBlending.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <atomic>
#	include <cstring>
#	include <optional>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.deferred";
	constexpr const char* kShader = "DeferredCompositeCS.hlsl";

	using ShaderBindings::Handle;

	// DeferredCompositeCS's push block: its DEFERRED_COMPOSITE_RESOURCES list, in order.
	struct CompositeBlock
	{
		SharedConstants::Handles shared;
		Handle specular, albedo, normalRoughness, masks, masks2;
		Handle mainRW, normalsRW, motionVectorsRW;
		Handle depth, reflectance, env, envReflections;
		Handle linearSampler;
		Handle skylightingProbes;
		Handle ssgiAo, ssgiY, ssgiCoCg, ssgiSpecular;
		Handle envIBL, skyIBL;
	};
	static_assert(sizeof(CompositeBlock) == 96, "matches the shader's push block (check-dual-target-shaders reports 96 bytes)");

	// The D3D11 composite's slots of what other features produce.
	constexpr std::array<std::uint32_t, 9> kViewSlots{ 6, 7, 8, 10, 11, 12, 13, 14, 15 };

	/** @brief The ids a slot's producer publishes its texture under, and whether the shader reads a cube. kViewSlots order. */
	struct SlotSource
	{
		std::array<std::string_view, 2> ids;
		bool cube = false;
	};
	constexpr std::array<SlotSource, kViewSlots.size()> kSlotSources{ {
		{ { ORGDynamicCubemaps::kEnvId, ORGDynamicCubemaps::kEnvBC6HId }, true },
		{ { ORGDynamicCubemaps::kEnvReflectionsId, ORGDynamicCubemaps::kEnvReflectionsBC6HId }, true },
		{ { ORGSkylighting::kProbeArrayId, {} }, false },
		{ { ORGScreenSpaceGI::kAoId, {} }, false },
		{ { ORGScreenSpaceGI::kYId, {} }, false },
		{ { ORGScreenSpaceGI::kCoCgId, {} }, false },
		{ { ORGScreenSpaceGI::kSpecularId, {} }, false },
		{ { ORGIBL::kEnvId, {} }, false },
		{ { ORGIBL::kSkyId, {} }, false },
	} };

	/**
	 * @brief What the composite reads at one of kViewSlots: the producer's published resource by its id (ordered after its writes
	 * in any epoch). Without one: the null descriptor, as D3D11 binds nothing there.
	 */
	struct SlotInput
	{
		std::string_view published;
		bool cube = false;

		bool operator==(const SlotInput&) const = default;
	};

	// The engine swaps the normals target with its twin every frame (forwardRenderTargets[2] alternates): both are imported and
	// declared, and each frame's execution picks one.
	constexpr std::array<std::uint32_t, 2> kNormalsTargets{ RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK,
		RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP };

	/** @brief The engine targets the pass declares, imported once (FrameAnchors::EngineTarget). Null reflectance and depth: not read. */
	struct Targets
	{
		FrameAnchors::Texture specular, albedo, normalRoughness, masks, masks2, reflectance, depth;
		FrameAnchors::Texture main, motionVectors;
		std::array<FrameAnchors::Texture, 2> normals;  // kNormalsTargets
		std::array<SlotInput, kViewSlots.size()> inputs;

		bool operator==(const Targets&) const = default;
	};

	/**
	 * @brief One frame's values that are not declared views: immutable once published, replaced only when they change. The
	 * recordings that read one keep it, with its null descriptor, until their GPU work retires (RecordingContext::Retain).
	 */
	struct Execution
	{
		std::uint64_t serial = 0;  // the pass's revision: a new one for every execution made
		std::shared_ptr<const ComputeProgram> program;
		std::uint32_t groupsX = 0, groupsY = 0;
		std::uint32_t normals = 0;  // which of kNormalsTargets the composite writes
		Handle null = 0;            // the null descriptor, for a slot with no input
		Handle sampler = 0;
		std::shared_ptr<const void> nullOwner;  // the null descriptor's ownership, through GPU completion
	};

	/** @brief What the passes share with the render thread. */
	struct Shared
	{
		Targets targets;
		std::atomic<std::shared_ptr<const Execution>> execution;
		// The execution each commit chose, by the epoch's run: what the pass recorded at execution reads. It may record on the
		// submission thread after the render thread chose the next frame's.
		::RenderGraph::RunSlots<Execution> executions;
		// One entry, opened by the commit that chose an execution: the pass is gated, so it can run in a frame phase.
		std::shared_ptr<const RenderGraph::RunGate> gate;
	};

	struct CompositeBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken specular, albedo, normalRoughness, masks, masks2, reflectance, depth;
		org::DeclaredViewToken main, motionVectors;
		std::array<org::DeclaredViewToken, 2> normals;
		std::array<org::DeclaredViewToken, kViewSlots.size()> inputs;  // where declared is set
		std::array<bool, kViewSlots.size()> declared{};
	};

	struct CompositeFrame
	{
		std::shared_ptr<const Execution> execution;  // null when the pass records at execution: Record reads shared's then
		std::shared_ptr<const Shared> shared;
		bool reflectance = false, depth = false;
	};

	class CompositePass final : public org::TypedRenderGraphPass<CompositePass, CompositeFrame, CompositeBindings>
	{
	public:
		CompositePass(std::shared_ptr<Shared> a_shared, bool a_atExecution) :
			shared(std::move(a_shared)), atExecution(a_atExecution) {}

		CompositeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& t = shared->targets;
			CompositeBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.specular = a_builder.ShaderResource(t.specular).View();
			bindings.albedo = a_builder.ShaderResource(t.albedo).View();
			bindings.normalRoughness = a_builder.ShaderResource(t.normalRoughness).View();
			bindings.masks = a_builder.ShaderResource(t.masks).View();
			bindings.masks2 = a_builder.ShaderResource(t.masks2).View();
			if (t.reflectance)
				bindings.reflectance = a_builder.ShaderResource(t.reflectance).View();
			if (t.depth)
				bindings.depth = a_builder.ShaderResource(t.depth).View();
			bindings.main = a_builder.UnorderedAccess(t.main).View();
			for (std::size_t i = 0; i < t.normals.size(); ++i)
				bindings.normals[i] = a_builder.UnorderedAccess(t.normals[i]).View();
			bindings.motionVectors = a_builder.UnorderedAccess(t.motionVectors).View();
			for (std::size_t i = 0; i < t.inputs.size(); ++i) {
				const auto& input = t.inputs[i];
				if (!input.published.empty() && PublishedResources::IsDeclarable(input.published)) {
					const org::SrvView view{ .variant = input.cube ? static_cast<std::uint32_t>(org::SRVViewType::TextureCube) : UINT32_MAX };
					bindings.inputs[i] = a_builder.ShaderResource(org::ResourceIdentifier(input.published), view).View();
					bindings.declared[i] = true;
				}
			}
			return bindings;
		}

		// The recorded dispatch depends on the execution only: the shared constants are uploaded each run. Recorded at execution, it
		// reads the execution the commit chose when it records, so the execution is not part of it: the normals target alternates
		// every frame, and a ticket prepared ahead would be prepared again each time.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			if (atExecution)
				return;
			const auto execution = shared->execution.load(std::memory_order_acquire);
			a_out.push_back(execution ? execution->serial : 0);
		}

		CompositeFrame Prepare(const CompositeBindings&, const org::PassPrepareContext&) const
		{
			return { atExecution ? nullptr : shared->execution.load(std::memory_order_acquire), shared, static_cast<bool>(shared->targets.reflectance),
				static_cast<bool>(shared->targets.depth) };
		}

		// Recorded at execution: a run whose commit did not open the gate (it chose no execution) records only the pass's barriers.
		static bool SkipsAtExecution(const CompositeBindings&, const CompositeFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return a_frame.shared && a_frame.shared->gate && !a_frame.shared->gate->IsOpen(a_recording.Run(), 0);
		}

		static void Record(const CompositeBindings& a_bindings, const CompositeFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto executionRef = a_frame.execution ? a_frame.execution :
			                          a_frame.shared    ? a_frame.shared->executions.Find(a_recording.Run()) :
			                                              nullptr;
			const auto* execution = executionRef.get();
			if (!execution || !execution->program)
				return;
			// The descriptors it names live as long as any submission of this recording.
			a_recording.Retain(executionRef);
			const Handle null = execution->null;
			// Each input's index comes from the view its declaration returned (kViewSlots order).
			std::array<Handle, kViewSlots.size()> inputs{};
			for (std::size_t i = 0; i < inputs.size(); ++i)
				inputs[i] = a_bindings.declared[i] ? a_recording.Index(a_bindings.inputs[i]) : null;
			CompositeBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.specular = a_recording.Index(a_bindings.specular);
			block.albedo = a_recording.Index(a_bindings.albedo);
			block.normalRoughness = a_recording.Index(a_bindings.normalRoughness);
			block.masks = a_recording.Index(a_bindings.masks);
			block.masks2 = a_recording.Index(a_bindings.masks2);
			block.mainRW = a_recording.Index(a_bindings.main);
			block.normalsRW = a_recording.Index(a_bindings.normals[execution->normals]);
			block.motionVectorsRW = a_recording.Index(a_bindings.motionVectors);
			block.depth = a_frame.depth ? a_recording.Index(a_bindings.depth) : null;
			block.reflectance = a_frame.reflectance ? a_recording.Index(a_bindings.reflectance) : null;
			block.env = inputs[0];
			block.envReflections = inputs[1];
			block.linearSampler = execution->sampler;
			block.skylightingProbes = inputs[2];
			block.ssgiAo = inputs[3];
			block.ssgiY = inputs[4];
			block.ssgiCoCg = inputs[5];
			block.ssgiSpecular = inputs[6];
			block.envIBL = inputs[7];
			block.skyIBL = inputs[8];
			execution->program->Dispatch(a_recording, ShaderBindings::Data(block), execution->groupsX, execution->groupsY, 1);
		}

	private:
		std::shared_ptr<Shared> shared;
		bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
	};

	/** @brief Deferred::CompositeDefines as the program takes them; ComputeProgram adds the common ones, as Util::CompileShader does. */
	std::vector<std::pair<std::wstring, std::wstring>> CompositeDefines(bool a_interior)
	{
		// Defines are ASCII.
		const auto wide = [](const char* a_text) { return a_text ? std::wstring(a_text, a_text + std::strlen(a_text)) : std::wstring(); };
		std::vector<std::pair<std::wstring, std::wstring>> defines;
		for (const auto& [name, value] : Deferred::CompositeDefines(a_interior))
			defines.emplace_back(wide(name), wide(value));
		return defines;
	}
}

struct ORGDeferredComposite::Impl
{
	std::shared_ptr<Shared> shared;
	std::shared_ptr<const ComputeProgram> exterior, interior;
	bool programsDirty = false;
	org::OwnedDescriptorBinding sampler;
	DCLF::GpuTextures::Binding null;
	bool registered = false;
	bool failed = false;

	// This frame's, from SetFrame: what the commit resolves.
	Frame frame{};
	std::uint32_t normals = 0;  // the frame's kNormalsTargets entry
	bool frameReady = false;
	// The last execution for each normals target: the engine alternates them, so each frame reuses one.
	std::array<std::shared_ptr<const Execution>, 2> byNormals;
	bool composited = false;
	std::uint64_t executions = 0;  // the serial of the last execution made
	std::shared_ptr<RenderGraph::RunGate> gate;  // Shared::gate

	bool LoadPrograms()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		const auto load = [&](bool a_interior) {
			return ComputeProgram::Load(host->Device(),
				{ .source = kShader, .constantWords = ShaderBindings::Words<CompositeBlock>(), .defines = CompositeDefines(a_interior) });
		};
		exterior = load(false);
		interior = load(true);
		programsDirty = false;
		return exterior && interior;
	}

	/** @brief The engine targets of a_frame, or nothing when one cannot be imported. */
	std::optional<Targets> ImportTargets(const Frame& a_frame) const
	{
		Targets t;
		t.specular = FrameAnchors::EngineTarget(SPECULAR);
		t.albedo = FrameAnchors::EngineTarget(ALBEDO);
		t.normalRoughness = FrameAnchors::EngineTarget(NORMALROUGHNESS);
		t.masks = FrameAnchors::EngineTarget(MASKS);
		t.masks2 = FrameAnchors::EngineTarget(MASKS2);
		t.main = FrameAnchors::EngineTarget(a_frame.main);
		for (std::size_t i = 0; i < kNormalsTargets.size(); ++i)
			t.normals[i] = FrameAnchors::EngineTarget(kNormalsTargets[i]);
		t.motionVectors = FrameAnchors::EngineTarget(RE::RENDER_TARGETS::kMOTION_VECTOR);
		// As the D3D11 composite: depth and reflectance only with Dynamic Cubemaps (null otherwise).
		if (globals::features::dynamicCubemaps.loaded) {
			t.reflectance = FrameAnchors::EngineTarget(REFLECTANCE);
			t.depth = FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
			if (!t.reflectance || !t.depth)
				return std::nullopt;
		}
		if (!t.specular || !t.albedo || !t.normalRoughness || !t.masks || !t.masks2 || !t.main || !t.normals[0] || !t.normals[1] || !t.motionVectors)
			return std::nullopt;
		for (std::size_t i = 0; i < kViewSlots.size(); ++i)
			t.inputs[i] = InputOf(i, a_frame.views[kViewSlots[i]]);
		return t;
	}

	/**
	 * @brief The input of kViewSlots[a_slot] for a_view, the view the D3D11 composite binds there: its producer's published id
	 * while a_view is that id's export. Otherwise the producer is not on the graph, and the slot binds the null descriptor: an
	 * import of its D3D11 texture is not ordered after its producer.
	 */
	static SlotInput InputOf(std::size_t a_slot, ID3D11ShaderResourceView* a_view)
	{
		const auto& source = kSlotSources[a_slot];
		SlotInput input{ .cube = source.cube };
		if (!a_view)
			return input;
		for (const auto id : source.ids)
			if (!id.empty() && PublishedResources::IsExportOf(a_view, id)) {
				input.published = id;
				return input;
			}
		return input;
	}

	bool Register(const Targets& a_targets)
	{
		if (!gate)
			gate = RenderGraph::RunGate::Create(kClientId, 1);
		if (!gate) {
			logger::warn("[ORG] Deferred composite: no run gate; the composite stays on D3D11");
			return false;
		}
		auto next = std::make_shared<Shared>();
		next->targets = a_targets;
		next->gate = gate;
		auto previous = shared;
		shared = next;
		registered = FrameAnchors::Register({
			.id = kClientId,
			.passes = { { .name = "cs.deferred.composite",
				.anchor = FrameAnchors::Anchor::DeferredComposite,
				.order = 0,
				.make = [next] {
					return std::static_pointer_cast<org::RenderPass>(
						std::make_shared<CompositePass>(next, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::DeferredComposite)));
				},
				// Its execution is chosen by the commit: recorded then, the pass reads it without a ticket prepared again.
				.atExecution = true } },
			.commit = [this](const FrameAnchors::Point&) {
				Commit();
				// The chosen execution, by this run, for the pass recorded at execution (null: none chosen).
				if (shared)
					if (auto* host = RenderGraphRuntime::Get().Host())
						shared->executions.Store(host->CurrentRun(), composited ? shared->execution.load(std::memory_order_acquire) : nullptr);
				if (composited)
					gate->OpenDispatch(0, 1);
			},
			.active = [this] { return frameReady; },
			.onFailed = [this] {
				logger::error("[ORG] Deferred composite: the render graph failed; the composite returns to D3D11");
				failed = true;
				registered = false; },
			.sharedConstants = true,
			.gates = { gate },
		});
		// The recordings that read the old executions keep them (CompositePass::Record).
		if (previous)
			byNormals = {};
		return registered;
	}

	/** @brief In the epoch: this frame's execution, resolved when its inputs changed. */
	void Commit()
	{
		composited = false;
		if (!frameReady || !shared)
			return;
		// Made inside the first epoch, where the graph's descriptor service is there.
		if (!sampler) {
			// Deferred::linearSampler's description.
			rhi::SamplerDesc samplerDesc{};
			samplerDesc.minFilter = rhi::Filter::Linear;
			samplerDesc.magFilter = rhi::Filter::Linear;
			samplerDesc.mipFilter = rhi::MipFilter::Linear;
			samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = rhi::AddressMode::Clamp;
			samplerDesc.minLod = 0.0f;
			samplerDesc.maxLod = D3D11_FLOAT32_MAX;
			sampler = ShaderBindings::CreateSampler(samplerDesc);
			null = DCLF::GpuTextures::Get().NullBinding();
			if (!sampler || null.index == DCLF::GpuTextures::kInvalid) {
				logger::error("[ORG] Deferred composite: no sampler or null descriptor; the composite returns to D3D11");
				failed = true;
				frameReady = false;
				shared->execution.store(nullptr, std::memory_order_release);
				return;
			}
		}
		const auto& program = frame.interior ? interior : exterior;
		auto& cached = byNormals[normals];
		if (cached && cached->program == program && cached->groupsX == frame.groupsX && cached->groupsY == frame.groupsY) {
			shared->execution.store(cached, std::memory_order_release);
			composited = true;
			return;
		}
		auto next = std::make_shared<Execution>();
		next->serial = ++executions;
		next->program = program;
		next->groupsX = frame.groupsX;
		next->groupsY = frame.groupsY;
		next->normals = normals;
		next->null = null.index;
		next->sampler = sampler.Index();
		next->nullOwner = null.owner;
		std::shared_ptr<const Execution> published = std::move(next);
		shared->execution.store(published, std::memory_order_release);
		cached = published;
		composited = true;
	}
};

ORGDeferredComposite::~ORGDeferredComposite() = default;

void ORGDeferredComposite::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredComposite))
		logger::warn("[ORG] Deferred composite: the DeferredComposite anchor could not be reserved; the composite stays on D3D11");
}

void ORGDeferredComposite::Setup()
{
	if (!Requested() || impl)
		return;
	if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredComposite)) {
		logger::warn("[ORG] Deferred composite: CS_ORG_COMPOSITE=1 but the anchor has no epoch; the composite stays on D3D11");
		return;
	}
	auto state = std::make_unique<Impl>();
	if (!state->LoadPrograms()) {
		logger::warn("[ORG] Deferred composite: the SPIR-V programs could not be built; the composite stays on D3D11");
		return;
	}
	impl = std::move(state);
	logger::info("[ORG] Deferred composite runs on the render graph (CS_ORG_COMPOSITE=1)");
}

void ORGDeferredComposite::SetFrame(const Frame& a_frame)
{
	if (!impl)
		return;
	impl->frameReady = false;
	impl->composited = false;
	if (impl->failed || !RenderGraphRuntime::Get().IsActive())
		return;
	if (impl->programsDirty && !impl->LoadPrograms()) {
		logger::error("[ORG] Deferred composite: the programs failed to build again; the composite returns to D3D11");
		impl->failed = true;
		return;
	}
	// The skylighting probes without Dynamic Cubemaps: D3D11 reads a null 3D texture, which the graph has no descriptor for.
	if (globals::features::skylighting.loaded && !a_frame.views[8])
		return;
	const auto normals = std::ranges::find(kNormalsTargets, a_frame.normals);
	if (normals == kNormalsTargets.end()) {
		static bool warned = false;
		if (!std::exchange(warned, true))
			logger::warn("[ORG] Deferred composite: normals target {} is not one the pass declares; D3D11 composites", a_frame.normals);
		return;
	}
	const auto targets = impl->ImportTargets(a_frame);
	if (!targets)
		return;
	if (!impl->registered || !impl->shared || !(impl->shared->targets == *targets)) {
		if (!impl->Register(*targets))
			return;
		logger::info("[ORG] Deferred composite: registered on the DeferredComposite anchor (main target {}, normals {} and {})", a_frame.main,
			kNormalsTargets[0], kNormalsTargets[1]);
	}
	impl->frame = a_frame;
	impl->normals = static_cast<std::uint32_t>(normals - kNormalsTargets.begin());
	impl->frameReady = true;
}

bool ORGDeferredComposite::Composited() const
{
	return impl && impl->composited;
}

bool ORGDeferredComposite::Lagged() const
{
	return impl && !impl->failed && impl->registered && impl->frameReady && !impl->composited;
}

void ORGDeferredComposite::ClearShaderCache()
{
	if (impl)
		impl->programsDirty = true;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the composite is always D3D11's.
struct ORGDeferredComposite::Impl
{};

ORGDeferredComposite::~ORGDeferredComposite() = default;
void ORGDeferredComposite::Reserve() {}
void ORGDeferredComposite::Setup() {}
void ORGDeferredComposite::SetFrame(const Frame&) {}
bool ORGDeferredComposite::Composited() const { return false; }
bool ORGDeferredComposite::Lagged() const { return false; }
void ORGDeferredComposite::ClearShaderCache() {}

#endif
