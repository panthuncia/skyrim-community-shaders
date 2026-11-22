#include "ORGSubsurfaceScattering.h"

#include "RenderGraph/Switches.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Deferred.h"
#include "Features/SubsurfaceScattering.h"
#include "Globals.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "RenderGraph/AnchorPasses.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include "RenderGraph/ProgramBuild.h"
#endif

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/VersionedTexture.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
		kSeparable = 0,
		kBurley = 1,
	};

	int ModeOf(int a_ssMode) { return a_ssMode == kBurley ? kBurley : kSeparable; }

	const char* ModeName(int a_mode) { return a_mode == kBurley ? "Burley" : "separable"; }

	using Program = std::shared_ptr<const ComputeProgram>;
	using Defines = std::vector<std::pair<std::wstring, std::wstring>>;

	/**
	* @brief One mode's programs, as SubsurfaceScattering's GetComputeShader* compile them. Separable: the extraction, the
	* horizontal and the vertical blur. Burley: its extraction and its blur, with no second blur.
	*/
	struct ModePrograms
	{
		int mode = kSeparable;
		Program extraction, blur, secondBlur;
	};

	/**
	* @brief Render thread, at setup. The defines every D3D11 build gets. They read State, so they are resolved here and not on
	* the programs lane. A program's own defines come first, as ComputeProgram::Load orders them, so the shader store hits.
	*/
	Defines ResolveCommonDefines()
	{
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
		return RenderGraph::ProgramBuild::WithCommonDefines({}, L"cs_6_6");
#else
		return {};
#endif
	}

	/** @brief Any thread. a_own, then the common defines it does not name. */
	template <class Block>
	Program LoadProgram(rhi::Device a_device, const char* a_source, Defines a_own, const Defines& a_common)
	{
		Defines defines = std::move(a_own);
		for (const auto& define : a_common)
			if (std::ranges::none_of(defines, [&](const auto& a_define) { return a_define.first == define.first; }))
				defines.push_back(define);
		return ComputeProgram::Load(a_device,
			{ .source = a_source, .constantWords = ShaderBindings::Words<Block>(), .defines = std::move(defines), .commonDefines = false });
	}

	/** @brief Any thread. Null, logged by ComputeProgram, on failure. */
	std::shared_ptr<const ModePrograms> LoadModePrograms(rhi::Device a_device, int a_mode, const Defines& a_common)
	{
		auto programs = std::make_shared<ModePrograms>();
		programs->mode = a_mode;
		if (a_mode == kBurley) {
			programs->extraction = LoadProgram<ExtractionBlock>(a_device, kExtractionShader, { { L"BURLEY", L"" } }, a_common);
			programs->blur = LoadProgram<BlurBlock>(a_device, kBlurShader, { { L"BURLEY", L"" } }, a_common);
			if (!programs->extraction || !programs->blur)
				return nullptr;
		} else {
			programs->extraction = LoadProgram<ExtractionBlock>(a_device, kExtractionShader, {}, a_common);
			programs->blur = LoadProgram<BlurBlock>(a_device, kBlurShader, { { L"HORIZONTAL", L"" } }, a_common);
			programs->secondBlur = LoadProgram<BlurBlock>(a_device, kBlurShader, {}, a_common);
			if (!programs->extraction || !programs->blur || !programs->secondBlur)
				return nullptr;
		}
		return programs;
	}

	/** @brief The engine targets the passes read. Slots, stable for the session, so a new version of one is not a change here. */
	struct Targets
	{
		FrameAnchors::Texture main, mask, albedo, normal, depth;

		bool operator==(const Targets&) const = default;
		explicit operator bool() const { return main && mask && albedo && normal && depth; }
	};

	/** @brief kMAIN, the GBuffer masks, albedo and normals, and the final depth copy. */
	Targets ImportTargets()
	{
		return { FrameAnchors::EngineTexture(FrameAnchors::Colour(RE::RENDER_TARGETS::kMAIN)), FrameAnchors::EngineTexture(FrameAnchors::Colour(MASKS)), FrameAnchors::EngineTexture(FrameAnchors::Colour(ALBEDO)),
			FrameAnchors::EngineTexture(FrameAnchors::Colour(NORMALROUGHNESS)), FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY)) };
	}

	/** @brief Made at setup and kept. The working textures are declared transients: each registration makes its own at its size. */
	struct Fixed
	{
		RenderGraph::Frame::DeclaredTransient2D diffuseNoAlbedo, blurTemp;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		org::OwnedDescriptorBinding pointSampler;
	};

	/** @brief What a registration is made for: the mode's programs, the imported targets and the working textures' extent. */
	struct Shape
	{
		std::shared_ptr<const ModePrograms> programs;
		Targets targets;
		RenderGraph::Frame::DeclaredTransient2D::Extent extent{};

		bool operator==(const Shape&) const = default;
	};

	/** @brief One registration's working textures, transients aliased in its builds' pool. blurTemp is null for Burley. */
	struct Working
	{
		std::shared_ptr<GraphResources::Texture2D> diffuseNoAlbedo, blurTemp;
	};

	/** @brief Kept for the build, so the recording can be submitted again. */
	struct PassFrame
	{
		std::shared_ptr<const ModePrograms> programs;
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
		ExtractionPass(const Fixed& a_fixed, const Shape& a_shape, FrameAnchors::Texture a_output) :
			programs(a_shape.programs),
			program(a_shape.programs->extraction.get()),
			latch(a_fixed.latch),
			gate(a_fixed.gate),
			output(std::move(a_output)),
			targets(a_shape.targets) {}

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
		std::shared_ptr<const ModePrograms> programs;
		const ComputeProgram* program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		FrameAnchors::Texture output;
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
		BlurPass(const Fixed& a_fixed, const Shape& a_shape, const Program& a_program, FrameAnchors::Texture a_color, FrameAnchors::Texture a_output) :
			programs(a_shape.programs),
			program(a_program.get()),
			latch(a_fixed.latch),
			gate(a_fixed.gate),
			pointSampler(a_fixed.pointSampler.Index()),
			color(std::move(a_color)),
			output(std::move(a_output)),
			targets(a_shape.targets) {}

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
		std::shared_ptr<const ModePrograms> programs;
		const ComputeProgram* program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t pointSampler;
		FrameAnchors::Texture color;
		FrameAnchors::Texture output;  // the temporary, or the engine's main target slot
		Targets targets;
	};

	/** @brief The registration for a new shape, when a revision starts. Only the shape's mode has passes. */
	bool MakeClient(const std::shared_ptr<const Fixed>& a_fixed, const Shape& a_shape, FrameAnchors::ClientDesc& a_client)
	{
		a_client.id = kClientId;
		a_client.sharedConstants = true;
		a_client.gates = { a_fixed->gate };
		const int mode = a_shape.programs->mode;
		// Written and read in this run only, so the builds alias them. The extraction writes the first before any read.
		auto working = std::make_shared<Working>();
		working->diffuseNoAlbedo = a_fixed->diffuseNoAlbedo.Make(a_shape.extent);
		if (mode != kBurley)
			working->blurTemp = a_fixed->blurTemp.Make(a_shape.extent);
		const auto diffuse = RenderGraph::AnchorPasses::TextureOf(*working->diffuseNoAlbedo);
		const auto temp = working->blurTemp ? RenderGraph::AnchorPasses::TextureOf(*working->blurTemp) : FrameAnchors::Texture();
		// After SSGI's passes, below 200, and before the composite, as the D3D11 path runs. Every pass waits on the gate entry, so
		// the anchor can run in a frame phase.
		const auto pass = [&a_client](const char* a_name, std::int32_t a_order, std::function<std::shared_ptr<org::RenderPass>()> a_make,
							  bool a_atExecution = false) {
			a_client.passes.push_back({ .name = a_name,
				.anchor = FrameAnchors::Anchor::DeferredPasses,
				.order = a_order,
				.kind = FrameAnchors::PassKind::Compute,
				.make = std::move(a_make),
				.atExecution = a_atExecution });
		};
		pass("cs.subsurface.extract", 200,
			[a_fixed, a_shape, diffuse]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ExtractionPass>(*a_fixed, a_shape, diffuse); });
		if (mode == kBurley) {
			// Burley's extraction writes only masked pixels. D3D11 reads stale values elsewhere, but the graph clears to zero, which
			// also keeps it aliasable.
			pass(
				"cs.subsurface.clear", 199,
				[a_fixed, working]() {
					return RenderGraph::AnchorPasses::MakeClear(RenderGraph::AnchorPasses::ClearOf("cs.subsurface.clear", *working->diffuseNoAlbedo, {}, kGateEntry),
						a_fixed->gate);
				},
				true);
			pass("cs.subsurface.burley", 201, [a_fixed, a_shape, diffuse]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<BlurPass>(*a_fixed, a_shape, a_shape.programs->blur, diffuse, a_shape.targets.main);
			});
		} else {
			pass("cs.subsurface.horizontal", 201, [a_fixed, a_shape, diffuse, temp]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<BlurPass>(*a_fixed, a_shape, a_shape.programs->blur, diffuse, temp);
			});
			pass("cs.subsurface.vertical", 202, [a_fixed, a_shape, temp]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<BlurPass>(*a_fixed, a_shape, a_shape.programs->secondBlur, temp, a_shape.targets.main);
			});
		}
		a_client.registerResources = [working](org::RenderGraph& a_graph) {
			RenderGraph::Frame::RegisterTransients(a_graph, { working->diffuseNoAlbedo.get(), working->blurTemp.get() });
		};
		a_client.commit = [a_fixed, mode](const FrameAnchors::Point&) {
			auto& feature = globals::features::subsurfaceScattering;
			a_fixed->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, feature.BuildBlurConstants(mode));
			// Only when the frame drew skin, as DrawSSS checks.
			if (feature.validMaterials) {
				const auto groups = Util::GetScreenDispatchCount();
				a_fixed->gate->OpenDispatch(kGateEntry, groups.x, groups.y, 1);
			}
			ORGSubsurfaceScattering::Get().NoteRan();
		};
		logger::info("[ORG] Subsurface Scattering: a client for the next revision ({} blur, working textures {}x{}{})", ModeName(mode), a_shape.extent[0],
			a_shape.extent[1], working->diffuseNoAlbedo->Aliasable() ? "" : ", kept apart");
		return true;
	}
}

struct ORGSubsurfaceScattering::Impl
{
	std::shared_ptr<const Fixed> fixed;
	// Declared at Setup and kept. A new mode is a program built off the render thread and a new client, in one revision.
	RenderGraph::Frame::DeclaredProgram<ModePrograms> program;
	RenderGraph::Frame::DeclaredClient client;
	// The engine targets' slots, taken at setup. They never change in a session.
	Targets targets;
	// Whether this frame's DeferredPasses epoch ran the passes.
	bool ranThisFrame = false;
	// A frame that ran no passes, logged once.
	bool lagLogged = false;
};

ORGSubsurfaceScattering::ORGSubsurfaceScattering() = default;
ORGSubsurfaceScattering::~ORGSubsurfaceScattering() = default;

bool ORGSubsurfaceScattering::Setup(const D3D11_TEXTURE2D_DESC& a_desc)
{
	// Once a session.
	auto* host = BeginSetup();
	if (!host)
		return IsActive();
	try {
		using namespace RenderGraph::Frame;
		const auto& settings = globals::features::subsurfaceScattering.settings;
		auto fixed = std::make_shared<Fixed>();
		// Deferred::pointSampler's description.
		fixed->pointSampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::PointClamp);
		// kMAIN's description on the display size (decision 6a). A new size registers the client again with transients of
		// that size, and the revision switches in with its builds. The engine target set writes the display size first.
		const auto onDisplay = [a_desc](const Inputs& a_in) {
			const auto display = a_in.Get(Vars::DisplaySize());
			D3D11_TEXTURE2D_DESC desc = a_desc;
			desc.Width = display.width;
			desc.Height = display.height;
			return desc;
		};
		// Transients, aliased in the deferred stage's pool. CS_ORG_SSS_KEEP_APART=1 keeps the albedo-free copy out of it.
		const bool keepApart = Switches::On("CS_ORG_SSS_KEEP_APART");
		fixed->diffuseNoAlbedo = DeclareTransient2D("SSS::DiffuseNoAlbedo", SizedDesc2D{ DependsOn(Vars::DisplaySize()), onDisplay },
			GraphResources::Options{ .lifetime = keepApart ? GraphResources::Lifetime::History : GraphResources::Lifetime::Transient });
		fixed->blurTemp = DeclareTransient2D("SSS::BlurHorizontalTemp", SizedDesc2D{ DependsOn(Vars::DisplaySize()), onDisplay },
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient });
		fixed->latch = std::make_shared<org::LatchBlock>("cs.subsurface.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
		fixed->gate = RenderGraph::RunGate::Create("cs.subsurface.dispatch");
		if (!fixed->pointSampler)
			Fatal("its sampler could not be made");
		auto made = std::make_unique<Impl>();
		// The engine's targets are declared before any feature's setup, so a missing slot is an error.
		made->targets = ImportTargets();
		if (!made->targets)
			Fatal(fmt::format("an engine target has no slot (main {}, masks {}, albedo {}, normals {}, depth {})", static_cast<bool>(made->targets.main),
				static_cast<bool>(made->targets.mask), static_cast<bool>(made->targets.albedo), static_cast<bool>(made->targets.normal),
				static_cast<bool>(made->targets.depth)));
		// The mode picks the programs. They are built on the programs lane, before the revision that needs them starts.
		made->program = DeclareProgram(
			"SubsurfaceScattering::Programs", DependsOn(settings.SSMode),
			[](const Inputs& a_in) { return ModeOf(a_in.Get(globals::features::subsurfaceScattering.settings.SSMode)); },
			[device = host->Device(), common = ResolveCommonDefines()](const int& a_mode) { return LoadModePrograms(device, a_mode, common); });
		if (!made->program)
			Fatal("its programs could not be made");
		const std::shared_ptr<const Fixed> shared = fixed;
		auto* self = made.get();
		// Registered at setup's build, so the first frame has it. A frame whose stage lags draws nothing; D3D11 never draws.
		// The working textures' size is part of the key, so a new display size registers the client again.
		auto dependencies = DependsOn(settings.SSMode);
		for (const auto& dependency : fixed->diffuseNoAlbedo.Depends())
			dependencies.push_back(dependency);
		made->client = DeclareClient<Shape>(
			kClientId, std::move(dependencies),
			[self, shared](const Inputs& a_in) -> std::optional<Shape> {
				return Shape{ self->program.For(a_in), self->targets, shared->diffuseNoAlbedo.ExtentAt(a_in) };
			},
			[shared](const Shape& a_shape, FrameAnchors::ClientDesc& a_client) { return MakeClient(shared, a_shape, a_client); });
		made->fixed = shared;
		impl = std::move(made);
		logger::info("[ORG] Subsurface Scattering runs on the render graph ({}x{}, {} blur); its programs and client follow the frame revisions, and each "
					 "registration makes its working textures as transients",
			a_desc.Width, a_desc.Height, ModeName(ModeOf(Inputs(nullptr).Get(settings.SSMode))));
		return true;
	} catch (const std::exception& e) {
		Fatal(fmt::format("setup threw: {}", e.what()));
	}
}

void ORGSubsurfaceScattering::NoteRan()
{
	if (impl)
		impl->ranThisFrame = true;
}

bool ORGSubsurfaceScattering::AfterDeferredPasses()
{
	if (!IsActive())
		return false;
	// A frame whose commit did not run (its stage lags, or it drew no skin) draws nothing more. D3D11 never draws in graph mode.
	if (!std::exchange(impl->ranThisFrame, false) && !std::exchange(impl->lagLogged, true))
		logger::info("[ORG] Subsurface Scattering: a frame ran no passes (its stage lags behind the client); it draws nothing (logged once)");
	return true;
}

