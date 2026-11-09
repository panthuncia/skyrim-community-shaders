#include "ORGSubsurfaceScattering.h"

#include <cstring>

bool ORGSubsurfaceScattering::Requested()
{
#if defined(CS_HAS_RENDER_GRAPH)
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_SUBSURFACE_SCATTERING", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
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
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "RenderGraph/UavClearPass.h"
#	include "Utils/D3D.h"
#	include "Utils/Game.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>

#	include <cstddef>
#	include <functional>
#	include <span>
#	include <string_view>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.subsurface";
	constexpr const char* kExtractionShader = "SubsurfaceScattering/DiffuseExtractionCS.hlsl";
	constexpr const char* kBlurShader = "SubsurfaceScattering/SeparableSSSCS.hlsl";

	using ShaderBindings::Handle;

	// DiffuseExtractionCS's push block: its DIFFUSE_EXTRACTION_RESOURCES list, in order.
	struct ExtractionBlock
	{
		SharedConstants::Handles shared;
		Handle output, color, mask, albedo;
		ShaderBindings::Latch perFrame;
	};
	static_assert(sizeof(ExtractionBlock) == 40, "matches the shader's push block (check-dual-target-shaders reports 40 bytes)");

	// SeparableSSSCS's push block: its SEPARABLE_SSS_RESOURCES list, in order.
	struct BlurBlock
	{
		SharedConstants::Handles shared;
		Handle output, color, depth, mask, albedo, normal, pointSampler;
		ShaderBindings::Latch perFrame;
	};
	static_assert(sizeof(BlurBlock) == 52, "matches the shader's push block (check-dual-target-shaders reports 52 bytes)");

	// One execution's values, in its slot of the latch block: the blur constants (SSSConstants in the shaders) and the
	// thread-group counts every pass dispatches with, zero in a frame without skin.
	struct LatchData
	{
		SubsurfaceScattering::BlurCB constants;
		std::uint32_t groups[4];  // x, y, z, padding
	};
	static_assert(offsetof(LatchData, constants) == 0 && sizeof(SubsurfaceScattering::BlurCB) % 16 == 0);
	static_assert(offsetof(LatchData, groups) % 16 == 0);

	enum Mode : int
	{
		kSeparable = 0,
		kBurley = 1,
	};

	/** @brief A program, with the indirect dispatch signature made against its layout. */
	struct Program
	{
		std::shared_ptr<const ComputeProgram> compute;
		rhi::CommandSignaturePtr signature;

		explicit operator bool() const { return compute && signature; }
	};

	/** @brief Every pass's program, as SubsurfaceScattering's GetComputeShader* compile them. */
	struct Programs
	{
		Program extraction, extractionBurley, horizontal, vertical, burley;

		explicit operator bool() const { return extraction && extractionBurley && horizontal && vertical && burley; }
	};

	/** @brief The defines a_own plus those Util::CompileShader gives every D3D11 build, so both paths build one permutation. */
	std::vector<std::pair<std::wstring, std::wstring>> ProgramDefines(std::vector<std::pair<std::string, std::string>> a_own)
	{
		std::vector<std::string_view> names;
		for (const auto& define : a_own)
			names.push_back(define.first);
		for (auto& define : Util::GetCommonShaderDefines(names))
			a_own.push_back(std::move(define));
		for (const char* name : { "COMPUTESHADER", "WINPC", "DX11" })
			a_own.emplace_back(name, "");
		// Defines are ASCII.
		std::vector<std::pair<std::wstring, std::wstring>> wide;
		for (const auto& [name, value] : a_own)
			wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		return wide;
	}

	template <class Block>
	Program LoadProgram(rhi::Device a_device, const char* a_source, std::vector<std::pair<std::string, std::string>> a_defines)
	{
		Program program;
		program.compute = ComputeProgram::Load(a_device,
			{ .source = a_source, .constantWords = ShaderBindings::Words<Block>(), .defines = ProgramDefines(std::move(a_defines)) });
		if (!program.compute)
			return {};
		rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
		if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 },
				program.compute->layout->GetHandle(), program.signature) != rhi::Result::Ok)
			return {};
		return program;
	}

	std::shared_ptr<const Programs> LoadPrograms(rhi::Device a_device)
	{
		auto programs = std::make_shared<Programs>();
		programs->extraction = LoadProgram<ExtractionBlock>(a_device, kExtractionShader, {});
		programs->extractionBurley = LoadProgram<ExtractionBlock>(a_device, kExtractionShader, { { "BURLEY", "" } });
		programs->horizontal = LoadProgram<BlurBlock>(a_device, kBlurShader, { { "HORIZONTAL", "" } });
		programs->vertical = LoadProgram<BlurBlock>(a_device, kBlurShader, {});
		programs->burley = LoadProgram<BlurBlock>(a_device, kBlurShader, { { "BURLEY", "" } });
		if (!*programs)
			return nullptr;
		return programs;
	}

	/** @brief The engine targets the passes read and write, imported once (FrameAnchors::EngineTarget). */
	struct Targets
	{
		std::shared_ptr<org::ExternalTextureResource> main, mask, albedo, normal, depth;

		bool operator==(const Targets&) const = default;
		explicit operator bool() const { return main && mask && albedo && normal && depth; }
	};

	/** @brief The D3D11 path's inputs: kMAIN, the GBuffer's masks, albedo and normals, and the final depth copy. */
	Targets ImportTargets()
	{
		return { FrameAnchors::EngineTarget(RE::RENDER_TARGETS::kMAIN), FrameAnchors::EngineTarget(MASKS), FrameAnchors::EngineTarget(ALBEDO),
			FrameAnchors::EngineTarget(NORMALROUGHNESS), FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY) };
	}

	/** @brief What the passes are made from; changed on the render thread only, between builds (then FrameAnchors::Refresh). */
	struct ScatterState
	{
		int mode = kSeparable;
		std::shared_ptr<const Programs> programs;
		Targets targets;
		std::unique_ptr<GraphResources::Texture2D> diffuseNoAlbedo, blurTemp;
		std::shared_ptr<org::LatchBlock> latch;
		org::OwnedDescriptorBinding pointSampler;
	};

	/** @brief Record time: this execution's latch region and its group counts' place in it. */
	struct LatchRegion
	{
		ShaderBindings::Latch latch;
		rhi::ResourceHandle buffer;
		std::uint64_t groups = 0;
	};

	LatchRegion RegionOf(const org::LatchBlock& a_latch, std::uint32_t a_slot)
	{
		return { ShaderBindings::LatchOf(a_latch, a_slot, offsetof(LatchData, constants)), a_latch.Resource()->GetAPIResource().GetHandle(),
			a_latch.Offset(a_slot) + offsetof(LatchData, groups) };
	}

	void DispatchIndirect(org::PassRecordContext& a_recording, const Program& a_program, std::span<const std::uint32_t> a_block, const LatchRegion& a_region)
	{
		auto& commands = a_recording.Commands();
		commands.BindLayout(a_program.compute->layout->GetHandle());
		commands.BindPipeline(a_program.compute->Pipeline());
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(a_block.size()), a_block.data());
		commands.ExecuteIndirect(a_program.signature->GetHandle(), a_region.buffer, a_region.groups, {}, 0, 1);
	}

	/** @brief What a pass records from: kept for the build, so the recording can be submitted again. */
	struct PassFrame
	{
		std::shared_ptr<const Programs> programs;
		std::shared_ptr<const org::LatchBlock> latch;
		const Program* program = nullptr;  // in programs
		std::uint32_t pointSampler = 0;
	};

	// The diffuse extraction: the main target, without albedo, into the albedo-free copy.
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
			program(a_state.mode == kBurley ? &a_state.programs->extractionBurley : &a_state.programs->extraction),
			latch(a_state.latch),
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
		}

		PassFrame Prepare(const ExtractionBindings&, const org::PassPrepareContext&) const { return { programs, latch, program, 0 }; }

		static void Record(const ExtractionBindings& a_bindings, const PassFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			const auto region = RegionOf(*a_frame.latch, a_recording.FrameSlot());
			const ExtractionBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.output),
				a_recording.Index(a_bindings.color), a_recording.Index(a_bindings.mask), a_recording.Index(a_bindings.albedo), region.latch };
			DispatchIndirect(a_recording, *a_frame.program, ShaderBindings::Data(block), region);
		}

	private:
		std::shared_ptr<const Programs> programs;
		const Program* program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<org::Resource> output;
		Targets targets;
	};

	// One blur pass: the horizontal (albedo-free copy to the temporary), the vertical (temporary into the main target) or Burley's
	// (albedo-free copy into the main target).
	struct BlurBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken output, color, depth, mask, albedo, normal;
	};

	class BlurPass final : public org::TypedRenderGraphPass<BlurPass, PassFrame, BlurBindings>
	{
	public:
		BlurPass(const ScatterState& a_state, const Program& a_program, std::shared_ptr<org::Resource> a_color, std::shared_ptr<org::Resource> a_output) :
			programs(a_state.programs),
			program(&a_program),
			latch(a_state.latch),
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
			a_out.push_back(pointSampler);
		}

		PassFrame Prepare(const BlurBindings&, const org::PassPrepareContext&) const { return { programs, latch, program, pointSampler }; }

		static void Record(const BlurBindings& a_bindings, const PassFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			const auto region = RegionOf(*a_frame.latch, a_recording.FrameSlot());
			const BlurBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.output),
				a_recording.Index(a_bindings.color), a_recording.Index(a_bindings.depth), a_recording.Index(a_bindings.mask),
				a_recording.Index(a_bindings.albedo), a_recording.Index(a_bindings.normal), a_frame.pointSampler, region.latch };
			DispatchIndirect(a_recording, *a_frame.program, ShaderBindings::Data(block), region);
		}

	private:
		std::shared_ptr<const Programs> programs;
		const Program* program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t pointSampler;
		std::shared_ptr<org::Resource> color;
		std::shared_ptr<org::Resource> output;
		Targets targets;
	};

	using MakePass = std::function<std::shared_ptr<org::RenderPass>()>;

	/** @brief A pass that runs in one mode only: null in builds for the other. */
	MakePass InMode(const std::shared_ptr<ScatterState>& a_state, int a_mode, std::function<std::shared_ptr<org::RenderPass>(const ScatterState&)> a_make)
	{
		return [a_state, a_mode, make = std::move(a_make)]() -> std::shared_ptr<org::RenderPass> {
			return a_state->mode == a_mode ? make(*a_state) : nullptr;
		};
	}

	const char* ModeName(int a_mode) { return a_mode == kBurley ? "Burley" : "separable"; }
}

struct ORGSubsurfaceScattering::Impl
{
	// The main target's description, from Setup: the intermediate textures' size and format.
	D3D11_TEXTURE2D_DESC desc{};
	// Made at the first frame the DeferredPasses epoch exists (Build); null until then.
	std::shared_ptr<ScatterState> state;
	bool programsDirty = false;
	// Whether this frame's DeferredPasses epoch ran the passes (the commit was called).
	bool ranThisFrame = false;
	bool registered = false;
	// What the log has said about a wait, so each is said once.
	bool epochLogged = false, targetsLogged = false, registerLogged = false;

	/** @brief The anchor client: its passes, its resources and its commit. */
	static FrameAnchors::ClientDesc MakeClient(const std::shared_ptr<ScatterState>& state)
	{
		const auto main = [](const ScatterState& a_state) { return std::static_pointer_cast<org::Resource>(a_state.targets.main); };
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.sharedConstants = true;
		// After SSGI's passes (whose orders are below 200) and before the composite, as the D3D11 path runs.
		const auto pass = [](const char* a_name, std::int32_t a_order, MakePass a_make) {
			return FrameAnchors::PassDesc{ .name = a_name, .anchor = FrameAnchors::Anchor::DeferredPasses, .order = a_order, .kind = FrameAnchors::PassKind::Compute, .make = std::move(a_make) };
		};
		client.passes.push_back(pass("cs.subsurface.extract", 200, [state]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ExtractionPass>(*state); }));
		client.passes.push_back(pass("cs.subsurface.horizontal", 201, InMode(state, kSeparable, [](const ScatterState& a_state) -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<BlurPass>(a_state, a_state.programs->horizontal, a_state.diffuseNoAlbedo->Graph(), a_state.blurTemp->Graph());
		})));
		client.passes.push_back(pass("cs.subsurface.vertical", 202, InMode(state, kSeparable, [main](const ScatterState& a_state) -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<BlurPass>(a_state, a_state.programs->vertical, a_state.blurTemp->Graph(), main(a_state));
		})));
		// Burley's extraction writes only the masked pixels, and the blur reads the others weighted by a zero mask. D3D11 reads the
		// previous frames' values there; the graph texture is cleared to zero first, which also keeps it aliasable.
		client.passes.push_back(pass("cs.subsurface.clear", 199, InMode(state, kBurley, [](const ScatterState& a_state) {
			return GraphPasses::MakeUavClear(a_state.diffuseNoAlbedo->Graph(), { 0.0f, 0.0f, 0.0f, 0.0f });
		})));
		client.passes.push_back(pass("cs.subsurface.burley", 201, InMode(state, kBurley, [main](const ScatterState& a_state) -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<BlurPass>(a_state, a_state.programs->burley, a_state.diffuseNoAlbedo->Graph(), main(a_state));
		})));
		client.registerResources = [state](org::RenderGraph& a_graph) {
			state->diffuseNoAlbedo->Register(a_graph);
			state->blurTemp->Register(a_graph);
		};
		client.commit = [state](const FrameAnchors::Point&) {
			auto& feature = globals::features::subsurfaceScattering;
			LatchData data{};
			data.constants = feature.BuildBlurConstants(state->mode);
			// Only when the frame drew skin, as DrawSSS's check.
			if (feature.validMaterials) {
				const auto groups = Util::GetScreenDispatchCount();
				data.groups[0] = groups.x;
				data.groups[1] = groups.y;
				data.groups[2] = 1;
			}
			state->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, data);
			if (auto& self = Get(); self.impl)
				self.impl->ranThisFrame = true;
		};
		client.onFailed = [] {
			logger::error("[ORG] Subsurface Scattering returns to D3D11: the render graph failed");
			Get().impl.reset();
		};
		return client;
	}

	/**
	 * @brief Registers the anchor client. The shared constant buffers cannot be made before State's first upload of them, so a
	 * refused registration is tried again at the next frame, with the D3D11 path running meanwhile.
	 */
	bool Register()
	{
		registered = FrameAnchors::Register(MakeClient(state));
		if (!registered && !std::exchange(registerLogged, true))
			logger::info("[ORG] Subsurface Scattering: the anchor client is not registered yet; D3D11 draws until it is");
		return registered;
	}

	/**
	 * @brief The programs, the sampler, the intermediate textures and the latch, each failure logged by name. Null on a failure,
	 * which is final: the feature then stays on D3D11.
	 */
	std::shared_ptr<ScatterState> Build() const
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
		// Deferred::pointSampler's description: point, clamp.
		next->pointSampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest,
			.addressU = rhi::AddressMode::Clamp,
			.addressV = rhi::AddressMode::Clamp,
			.addressW = rhi::AddressMode::Clamp });
		if (!next->pointSampler)
			fail("its point sampler");
		// Written before they are read and read only within the frame: transient, so aliasable.
		next->diffuseNoAlbedo = std::make_unique<GraphResources::Texture2D>(desc, "SSS::DiffuseNoAlbedo");
		if (!*next->diffuseNoAlbedo)
			fail("its albedo-free texture");
		next->blurTemp = std::make_unique<GraphResources::Texture2D>(desc, "SSS::BlurHorizontalTemp");
		if (!*next->blurTemp)
			fail("its blur texture");
		next->latch = std::make_shared<org::LatchBlock>("cs.subsurface.latch", static_cast<std::uint32_t>(sizeof(LatchData)), host->FrameSlots());
		return ok ? next : nullptr;
	}

	static int CurrentMode() { return globals::features::subsurfaceScattering.settings.SSMode == kBurley ? kBurley : kSeparable; }
};

ORGSubsurfaceScattering::~ORGSubsurfaceScattering() = default;

bool ORGSubsurfaceScattering::Running() const
{
	return impl != nullptr;
}

void ORGSubsurfaceScattering::Setup(const D3D11_TEXTURE2D_DESC& a_desc)
{
	impl.reset();
	if (!RenderGraphRuntime::Get().Host()) {
		logger::warn("[ORG] Subsurface Scattering stays on D3D11: the render graph is not available");
		return;
	}
	// The rest at the first frames (AfterDeferredPasses): the anchor's epoch, the engine targets and the shared constants
	// exist only once the world renders.
	impl = std::make_unique<Impl>();
	impl->desc = a_desc;
	logger::info("[ORG] Subsurface Scattering: the render graph path is requested ({}x{}); set up at the first frames", a_desc.Width, a_desc.Height);
}

bool ORGSubsurfaceScattering::AfterDeferredPasses()
{
	if (!impl)
		return false;
	const auto giveUp = [this](const char* a_why) {
		logger::error("[ORG] Subsurface Scattering returns to D3D11: {}", a_why);
		FrameAnchors::Unregister(kClientId);
		impl.reset();
		return false;
	};
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive()) {
		if (impl->state)
			return giveUp("the render graph is no longer active");
		return false;
	}

	// Set up: waits are tried again at the next frame, with D3D11 drawing meanwhile.
	if (!impl->state) {
		if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses)) {
			if (!std::exchange(impl->epochLogged, true))
				logger::info("[ORG] Subsurface Scattering: waiting for the DeferredPasses anchor's epoch");
			return false;
		}
		try {
			impl->state = impl->Build();
		} catch (const std::exception& e) {
			logger::error("[ORG] Subsurface Scattering setup failed: {}", e.what());
		}
		if (!impl->state)
			return giveUp("its setup failed");
		logger::info("[ORG] Subsurface Scattering runs on the render graph ({}x{}, {} blur)", impl->desc.Width, impl->desc.Height,
			ModeName(impl->state->mode));
	}
	auto& state = *impl->state;
	if (!state.targets) {
		state.targets = ImportTargets();
		if (!state.targets) {
			if (!std::exchange(impl->targetsLogged, true)) {
				logger::info("[ORG] Subsurface Scattering: waiting for the engine targets (main {}, masks {}, albedo {}, normals {}, depth {})",
					state.targets.main != nullptr, state.targets.mask != nullptr, state.targets.albedo != nullptr, state.targets.normal != nullptr,
					state.targets.depth != nullptr);
			}
			state.targets = {};
			return false;
		}
	}
	if (!impl->registered) {
		// From the next frame once the client is registered (State has uploaded the shared constants by now).
		if (impl->Register())
			logger::info("[ORG] Subsurface Scattering: registered on the DeferredPasses anchor");
		return false;
	}

	const bool ran = std::exchange(impl->ranThisFrame, false);
	bool changed = false;
	if (const int mode = Impl::CurrentMode(); mode != state.mode) {
		state.mode = mode;
		logger::info("[ORG] Subsurface Scattering: {} blur", ModeName(mode));
		changed = true;
	}
	if (impl->programsDirty) {
		impl->programsDirty = false;
		auto programs = LoadPrograms(runtime.Host()->Device());
		if (!programs) {
			giveUp("its programs could not be built again");
			return ran;
		}
		state.programs = std::move(programs);
		changed = true;
	}
	if (auto targets = ImportTargets(); !(targets == state.targets)) {
		if (!targets) {
			giveUp("an engine target could not be imported");
			return ran;
		}
		state.targets = std::move(targets);
		changed = true;
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

// Built without OpenRenderGraph: Subsurface Scattering always draws with D3D11.
struct ORGSubsurfaceScattering::Impl
{};

ORGSubsurfaceScattering::~ORGSubsurfaceScattering() = default;
void ORGSubsurfaceScattering::Setup(const D3D11_TEXTURE2D_DESC&) {}
bool ORGSubsurfaceScattering::Running() const { return false; }
bool ORGSubsurfaceScattering::AfterDeferredPasses() { return false; }
void ORGSubsurfaceScattering::ClearShaderCache() {}

#endif
