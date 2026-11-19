#include "ORGSubsurfaceScattering.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGSubsurfaceScattering::Requested()
{
#if defined(CS_HAS_RENDER_GRAPH)
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
#else
	return false;
#endif
}

ORGSubsurfaceScattering& ORGSubsurfaceScattering::Get()
{
	static ORGSubsurfaceScattering s_scattering;
	return s_scattering;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Features/SubsurfaceScattering.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "RenderGraph/UavClearPass.h"
#	include "Utils/D3D.h"
#	include "Utils/Game.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <cstddef>
#	include <functional>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.subsurface";
	constexpr const char* kExtractionShader = "SubsurfaceScattering/DiffuseExtractionCS.hlsl";
	constexpr const char* kBlurShader = "SubsurfaceScattering/SeparableSSSCS.hlsl";

	using ShaderBindings::Handle;

	// In DIFFUSE_EXTRACTION_RESOURCES order.
	struct ExtractionBlock
	{
		SharedConstants::Handles shared;
		Handle output, color, mask, albedo;
		ShaderBindings::Latch perFrame;
	};
	static_assert(sizeof(ExtractionBlock) == 40, "matches the shader's push block (check-dual-target-shaders reports 40 bytes)");

	// In SEPARABLE_SSS_RESOURCES order.
	struct BlurBlock
	{
		SharedConstants::Handles shared;
		Handle output, color, depth, mask, albedo, normal, pointSampler;
		ShaderBindings::Latch perFrame;
	};
	static_assert(sizeof(BlurBlock) == 52, "matches the shader's push block (check-dual-target-shaders reports 52 bytes)");

	// SSSConstants per run. The group counts are in the gate's one entry, open only in a frame with skin.
	using LatchData = SubsurfaceScattering::BlurCB;
	static_assert(sizeof(LatchData) % 16 == 0);
	constexpr std::uint32_t kGateEntry = 0;

	enum Mode : int
	{
		kAnyMode = -1,  // a pass of both modes
		kSeparable = 0,
		kBurley = 1,
	};

	using Program = std::shared_ptr<const ComputeProgram>;

	/** @brief As SubsurfaceScattering's GetComputeShader* compile them. */
	struct Programs
	{
		Program extraction, extractionBurley, horizontal, vertical, burley;

		explicit operator bool() const { return extraction && extractionBurley && horizontal && vertical && burley; }
	};

	/** @brief Adds the defines every D3D11 build gets. */
	template <class Block>
	Program LoadProgram(rhi::Device a_device, const char* a_source, std::vector<std::pair<std::wstring, std::wstring>> a_defines)
	{
		return ComputeProgram::Load(a_device, { .source = a_source, .constantWords = ShaderBindings::Words<Block>(), .defines = std::move(a_defines) });
	}

	std::shared_ptr<const Programs> LoadPrograms(rhi::Device a_device)
	{
		auto programs = std::make_shared<Programs>();
		programs->extraction = LoadProgram<ExtractionBlock>(a_device, kExtractionShader, {});
		programs->extractionBurley = LoadProgram<ExtractionBlock>(a_device, kExtractionShader, { { L"BURLEY", L"" } });
		programs->horizontal = LoadProgram<BlurBlock>(a_device, kBlurShader, { { L"HORIZONTAL", L"" } });
		programs->vertical = LoadProgram<BlurBlock>(a_device, kBlurShader, {});
		programs->burley = LoadProgram<BlurBlock>(a_device, kBlurShader, { { L"BURLEY", L"" } });
		if (!*programs)
			return nullptr;
		return programs;
	}

	/** @brief Imported once. */
	struct Targets
	{
		FrameAnchors::Texture main, mask, albedo, normal, depth;

		bool operator==(const Targets&) const = default;
		explicit operator bool() const { return main && mask && albedo && normal && depth; }
	};

	/** @brief kMAIN, the GBuffer masks, albedo and normals, and the final depth copy. */
	Targets ImportTargets()
	{
		return { FrameAnchors::EngineTarget(RE::RENDER_TARGETS::kMAIN), FrameAnchors::EngineTarget(MASKS), FrameAnchors::EngineTarget(ALBEDO),
			FrameAnchors::EngineTarget(NORMALROUGHNESS), FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY) };
	}

	/** @brief Changed on the render thread only, between builds. */
	struct ScatterState
	{
		int mode = kSeparable;
		std::shared_ptr<const Programs> programs;
		Targets targets;
		std::unique_ptr<GraphResources::Texture2D> diffuseNoAlbedo, blurTemp;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		org::OwnedDescriptorBinding pointSampler;
	};

	/** @brief Kept for the build, so the recording can be submitted again. */
	struct PassFrame
	{
		std::shared_ptr<const Programs> programs;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		const ComputeProgram* program = nullptr;
		std::uint32_t pointSampler = 0;
	};

	// The main target without albedo, into the albedo-free copy.
	struct ExtractionBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken output, color, mask, albedo;
	};

	class ExtractionPass final : public org::TypedRenderGraphPass<ExtractionPass, PassFrame, ExtractionBindings>
	{
	public:
		explicit ExtractionPass(const ScatterState& a_state) :
			programs(a_state.programs),
			program(a_state.mode == kBurley ? a_state.programs->extractionBurley.get() : a_state.programs->extraction.get()),
			latch(a_state.latch),
			gate(a_state.gate),
			output(a_state.diffuseNoAlbedo->Graph()),
			targets(a_state.targets) {}

		ExtractionBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ExtractionBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.output = a_builder.UnorderedAccess(output).View();
			bindings.color = a_builder.ShaderResource(targets.main).View();
			bindings.mask = a_builder.ShaderResource(targets.mask).View();
			bindings.albedo = a_builder.ShaderResource(targets.albedo).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.get()));
		}

		PassFrame Prepare(const ExtractionBindings&, const org::PassPrepareContext&) const { return { programs, latch, gate, program, 0 }; }

		static void Record(const ExtractionBindings& a_bindings, const PassFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.gate)
				return;
			const ExtractionBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.output),
				a_recording.Index(a_bindings.color), a_recording.Index(a_bindings.mask), a_recording.Index(a_bindings.albedo),
				ShaderBindings::LatchOf(*a_frame.latch, a_recording.Run(), 0) };
			a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), kGateEntry);
		}

	private:
		std::shared_ptr<const Programs> programs;
		const ComputeProgram* program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::shared_ptr<org::Resource> output;
		Targets targets;
	};

	// Horizontal into the temporary, vertical into the main target, or Burley's from the albedo-free copy into the main target.
	struct BlurBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken output, color, depth, mask, albedo, normal;
	};

	class BlurPass final : public org::TypedRenderGraphPass<BlurPass, PassFrame, BlurBindings>
	{
	public:
		BlurPass(const ScatterState& a_state, const Program& a_program, FrameAnchors::Texture a_color, FrameAnchors::Texture a_output) :
			programs(a_state.programs),
			program(a_program.get()),
			latch(a_state.latch),
			gate(a_state.gate),
			pointSampler(a_state.pointSampler.Index()),
			color(std::move(a_color)),
			output(std::move(a_output)),
			targets(a_state.targets) {}

		BlurBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			BlurBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.output = a_builder.UnorderedAccess(output).View();
			bindings.color = a_builder.ShaderResource(color).View();
			bindings.depth = a_builder.ShaderResource(targets.depth).View();
			bindings.mask = a_builder.ShaderResource(targets.mask).View();
			bindings.albedo = a_builder.ShaderResource(targets.albedo).View();
			bindings.normal = a_builder.ShaderResource(targets.normal).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.get()));
			a_out.push_back(pointSampler);
		}

		PassFrame Prepare(const BlurBindings&, const org::PassPrepareContext&) const { return { programs, latch, gate, program, pointSampler }; }

		static void Record(const BlurBindings& a_bindings, const PassFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.gate)
				return;
			const BlurBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.output),
				a_recording.Index(a_bindings.color), a_recording.Index(a_bindings.depth), a_recording.Index(a_bindings.mask),
				a_recording.Index(a_bindings.albedo), a_recording.Index(a_bindings.normal), a_frame.pointSampler,
				ShaderBindings::LatchOf(*a_frame.latch, a_recording.Run(), 0) };
			a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), kGateEntry);
		}

	private:
		std::shared_ptr<const Programs> programs;
		const ComputeProgram* program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t pointSampler;
		FrameAnchors::Texture color;
		FrameAnchors::Texture output;  // the temporary, or the engine's main target slot
		Targets targets;
	};

	using MakePass = std::function<std::shared_ptr<org::RenderPass>()>;

	/** @brief Null in builds without the engine targets or in the other mode. */
	MakePass InMode(const std::shared_ptr<ScatterState>& a_state, int a_mode, std::function<std::shared_ptr<org::RenderPass>(const ScatterState&)> a_make)
	{
		return [a_state, a_mode, make = std::move(a_make)]() -> std::shared_ptr<org::RenderPass> {
			return a_state->targets && (a_mode == kAnyMode || a_state->mode == a_mode) ? make(*a_state) : nullptr;
		};
	}

	const char* ModeName(int a_mode) { return a_mode == kBurley ? "Burley" : "separable"; }
}

struct ORGSubsurfaceScattering::Impl
{
	std::shared_ptr<ScatterState> state;
	bool programsDirty = false;
	// Whether this frame's DeferredPasses epoch ran the passes.
	bool ranThisFrame = false;
	// Logged once.
	bool targetsLogged = false;

	/** @brief A build without the engine targets has no passes and opens nothing, so the D3D11 path draws those frames. */
	static FrameAnchors::ClientDesc MakeClient(const std::shared_ptr<ScatterState>& state)
	{
		const auto main = [](const ScatterState& a_state) { return a_state.targets.main; };
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.sharedConstants = true;
		client.gates = { state->gate };
		// After SSGI's passes, below 200, and before the composite, as the D3D11 path runs.
		const auto pass = [](const char* a_name, std::int32_t a_order, MakePass a_make, bool a_atExecution = false) {
			// Every pass waits on the gate entry, so the anchor can run in a frame phase.
			return FrameAnchors::PassDesc{ .name = a_name,
				.anchor = FrameAnchors::Anchor::DeferredPasses,
				.order = a_order,
				.kind = FrameAnchors::PassKind::Compute,
				.make = std::move(a_make),
				.atExecution = a_atExecution };
		};
		client.passes.push_back(pass("cs.subsurface.extract", 200, InMode(state, kAnyMode, [](const ScatterState& a_state) -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<ExtractionPass>(a_state);
		})));
		client.passes.push_back(pass("cs.subsurface.horizontal", 201, InMode(state, kSeparable, [](const ScatterState& a_state) -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<BlurPass>(a_state, a_state.programs->horizontal, a_state.diffuseNoAlbedo->Graph(), a_state.blurTemp->Graph());
		})));
		client.passes.push_back(pass("cs.subsurface.vertical", 202, InMode(state, kSeparable, [main](const ScatterState& a_state) -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<BlurPass>(a_state, a_state.programs->vertical, a_state.blurTemp->Graph(), main(a_state));
		})));
		// Burley's extraction writes only masked pixels. D3D11 reads stale values elsewhere, but the graph clears to zero, which also keeps it aliasable.
		client.passes.push_back(pass("cs.subsurface.clear", 199, InMode(state, kBurley, [](const ScatterState& a_state) {
			return GraphPasses::MakeUavClear(a_state.diffuseNoAlbedo->Graph(), { 0.0f, 0.0f, 0.0f, 0.0f }, { a_state.gate, kGateEntry });
		}), true));
		client.passes.push_back(pass("cs.subsurface.burley", 201, InMode(state, kBurley, [main](const ScatterState& a_state) -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<BlurPass>(a_state, a_state.programs->burley, a_state.diffuseNoAlbedo->Graph(), main(a_state));
		})));
		client.registerResources = [state](org::RenderGraph& a_graph) {
			state->diffuseNoAlbedo->Register(a_graph);
			state->blurTemp->Register(a_graph);
		};
		client.commit = [state](const FrameAnchors::Point&) {
			// The targets were set before this run's build, so it has no passes either.
			if (!state->targets)
				return;
			auto& feature = globals::features::subsurfaceScattering;
			state->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, feature.BuildBlurConstants(state->mode));
			// Only when the frame drew skin, as DrawSSS checks.
			if (feature.validMaterials) {
				const auto groups = Util::GetScreenDispatchCount();
				state->gate->OpenDispatch(kGateEntry, groups.x, groups.y, 1);
			}
			if (auto& self = Get(); self.impl)
				self.impl->ranThisFrame = true;
		};
		return client;
	}

	/** @brief Null on failure, each logged by name. The feature then stays on D3D11. */
	static std::shared_ptr<ScatterState> Build(const D3D11_TEXTURE2D_DESC& a_desc)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto next = std::make_shared<ScatterState>();
		next->mode = CurrentMode();
		bool ok = true;
		const auto fail = [&](const char* a_what) {
			logger::warn("[ORG] Subsurface Scattering stays on D3D11: {} could not be made", a_what);
			ok = false;
		};
		next->programs = LoadPrograms(host->Device());
		if (!next->programs)
			fail("its programs");
		// Deferred::pointSampler's description.
		next->pointSampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest,
			.addressU = rhi::AddressMode::Clamp,
			.addressV = rhi::AddressMode::Clamp,
			.addressW = rhi::AddressMode::Clamp });
		if (!next->pointSampler)
			fail("its point sampler");
		// Transient and aliasable. CS_ORG_SSGI_DEBUG=16 keeps it out of the pool to test whether it overwrites SSGI's memory.
		const bool keepApart = Switches::Number("CS_ORG_SSGI_DEBUG", 0) == 16;
		next->diffuseNoAlbedo = std::make_unique<GraphResources::Texture2D>(a_desc, "SSS::DiffuseNoAlbedo",
			GraphResources::Options{ .lifetime = keepApart ? GraphResources::Lifetime::History : GraphResources::Lifetime::Transient });
		if (!*next->diffuseNoAlbedo)
			fail("its albedo-free texture");
		next->blurTemp = std::make_unique<GraphResources::Texture2D>(a_desc, "SSS::BlurHorizontalTemp");
		if (!*next->blurTemp)
			fail("its blur texture");
		next->latch = std::make_shared<org::LatchBlock>("cs.subsurface.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
		next->gate = RenderGraph::RunGate::Create("cs.subsurface.dispatch");
		if (!next->gate)
			fail("its run gate");
		return ok ? next : nullptr;
	}

	static int CurrentMode() { return globals::features::subsurfaceScattering.settings.SSMode == kBurley ? kBurley : kSeparable; }
};

ORGSubsurfaceScattering::~ORGSubsurfaceScattering() = default;

void ORGSubsurfaceScattering::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredPasses))
		logger::warn("[ORG] Subsurface Scattering stays on D3D11: the DeferredPasses anchor could not be reserved");
}

bool ORGSubsurfaceScattering::Setup(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (impl)
		Stop();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.Host() || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses)) {
		logger::warn("[ORG] Subsurface Scattering stays on D3D11: the render graph or its DeferredPasses anchor is not available");
		return false;
	}
	std::shared_ptr<ScatterState> state;
	try {
		state = Impl::Build(a_desc);
	} catch (const std::exception& e) {
		logger::error("[ORG] Subsurface Scattering setup failed, staying on D3D11: {}", e.what());
	}
	if (!state)
		return false;
	// The engine targets may not exist yet. The passes then join at the first frame they can be imported.
	if (auto targets = ImportTargets())
		state->targets = std::move(targets);
	// The anchors hold the client until the shared constants first upload. D3D11 draws until the commit runs.
	if (!FrameAnchors::Register(Impl::MakeClient(state))) {
		logger::warn("[ORG] Subsurface Scattering stays on D3D11: the anchor client could not be registered");
		return false;
	}
	impl = std::make_unique<Impl>();
	impl->state = std::move(state);
	logger::info("[ORG] Subsurface Scattering runs on the render graph ({}x{}, {} blur)", a_desc.Width, a_desc.Height, ModeName(impl->state->mode));
	return true;
}

bool ORGSubsurfaceScattering::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

void ORGSubsurfaceScattering::Stop()
{
	FrameAnchors::Unregister(kClientId);
	impl.reset();
}

bool ORGSubsurfaceScattering::AfterDeferredPasses()
{
	if (!IsActive())
		return false;
	const auto giveUp = [this](const char* a_why) {
		logger::error("[ORG] Subsurface Scattering returns to D3D11: {}", a_why);
		Stop();
	};
	auto& state = *impl->state;
	const bool ran = std::exchange(impl->ranThisFrame, false);
	bool changed = false;
	if (const int mode = Impl::CurrentMode(); mode != state.mode) {
		state.mode = mode;
		logger::info("[ORG] Subsurface Scattering: {} blur", ModeName(mode));
		changed = true;
	}
	if (impl->programsDirty) {
		impl->programsDirty = false;
		auto programs = LoadPrograms(RenderGraphRuntime::Get().Host()->Device());
		if (!programs) {
			giveUp("its programs could not be built again");
			return ran;
		}
		state.programs = std::move(programs);
		changed = true;
	}
	if (auto targets = ImportTargets(); !(targets == state.targets)) {
		if (targets) {
			if (!state.targets)
				logger::info("[ORG] Subsurface Scattering: the engine targets are imported; the passes join the graph");
			state.targets = std::move(targets);
			changed = true;
		} else if (state.targets) {
			giveUp("an engine target could not be imported");
			return ran;
		} else if (!std::exchange(impl->targetsLogged, true)) {
			logger::info("[ORG] Subsurface Scattering: waiting for the engine targets (main {}, masks {}, albedo {}, normals {}, depth {})",
				targets.main != nullptr, targets.mask != nullptr, targets.albedo != nullptr, targets.normal != nullptr, targets.depth != nullptr);
		}
	}
	if (changed)
		FrameAnchors::Refresh(kClientId);
	return ran;
}

void ORGSubsurfaceScattering::ClearShaderCache()
{
	if (impl)
		impl->programsDirty = true;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Subsurface Scattering always draws with D3D11.
struct ORGSubsurfaceScattering::Impl
{};

ORGSubsurfaceScattering::~ORGSubsurfaceScattering() = default;
void ORGSubsurfaceScattering::Reserve() {}
bool ORGSubsurfaceScattering::Setup(const D3D11_TEXTURE2D_DESC&) { return false; }
bool ORGSubsurfaceScattering::IsActive() const { return false; }
bool ORGSubsurfaceScattering::AfterDeferredPasses() { return false; }
void ORGSubsurfaceScattering::ClearShaderCache() {}
void ORGSubsurfaceScattering::Stop() {}

#endif
