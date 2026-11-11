#include "ORGPostProcessing.h"

#include "RenderGraph/Switches.h"

bool ORGPostProcessing::Requested()
{
	static const bool requested = Switches::On("CS_ORG_POST_PROCESSING");
	return requested;
}

ORGPostProcessing& ORGPostProcessing::Get()
{
	static ORGPostProcessing s_postProcessing;
	return s_postProcessing;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGPostEffect.h"

#	include "Features/LinearLighting.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "State.h"
#	include "Utils/ColorSpace.h"
#	include "Utils/D3D.h"
#	include "Utils/Format.h"
#	include "Utils/Game.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <cmath>
#	include <cstring>
#	include <format>
#	include <map>
#	include <span>
#	include <unordered_map>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kClientId = "cs.pp";
		constexpr const char* kCopyShader = "PostProcessing/copy.ps.hlsl";
		// After any earlier client of the Tonemap anchor; the chain's own images order its passes among themselves.
		constexpr std::int32_t kOrderBase = 1000;
		constexpr DXGI_FORMAT kInputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // PostProcessing::texInput's
		// The run gate entry every draw of the chain goes through: the chain's commit opens it on the frames the chain runs.
		constexpr std::uint32_t kDrawEntry = 0;
	}

	/** @brief The program caches, for the session: a rebuild (a settings change) finds the programs it built before. */
	struct Programs
	{
		rhi::Device device;
		std::unordered_map<std::string, std::shared_ptr<const RasterProgram>> raster;
		std::unordered_map<std::string, std::shared_ptr<const ComputeProgram>> compute;
		// Set after the first chain built: a program built later (a setting's new permutation) is logged.
		bool chainBuilt = false;
	};

	/** @brief One build of the chain: what its passes are made from. Immutable once registered. */
	struct BuildState
	{
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		std::unique_ptr<RenderGraph::AnchorPasses::Builder> passes;  // the passes as they are added, until Take
		std::vector<FrameAnchors::PassDesc> descs;                   // the passes taken for the client
		std::uint32_t latchUsed = 0;
		std::uint32_t gateUsed = kDrawEntry + 1;
		std::vector<std::pair<std::array<std::uint32_t, 3>, std::uint32_t>> fixed;  // a fixed dispatch size and its gate entry
		std::vector<std::string> once;                                               // the one-shot passes' names
		std::vector<std::unique_ptr<GraphResources::Texture2D>> images;
		std::vector<std::pair<std::string, std::shared_ptr<org::Resource>>> registered;
		std::map<std::string, Image, std::less<>> published;
		std::map<std::string, std::pair<std::uint32_t, std::uint32_t>, std::less<>> publishedLatches;  // offset, size
		Image current;
		std::string failure;  // empty while the build is good; the pass builder keeps its own

		std::string Failure() const { return failure.empty() ? passes->Failure() : failure; }
	};

	namespace
	{
		std::vector<RenderGraph::AnchorPasses::Bind> Bases(const std::vector<Bind>& a_bindings) { return { a_bindings.begin(), a_bindings.end() }; }

		std::string ProgramKey(std::string_view a_source, const std::wstring& a_entry, std::uint32_t a_words,
			const std::vector<std::pair<std::string, std::string>>& a_defines)
		{
			std::string key = std::format("{}|{}|{}", a_source, Util::WStringToString(a_entry), a_words);
			for (const auto& [name, value] : a_defines)
				key += std::format("|{}={}", name, value);
			return key;
		}

		/** @brief The pixels a D3D11 viewport a_size wide covers: those whose centre lies inside it. */
		std::uint32_t CoveredPixels(float a_size) { return static_cast<std::uint32_t>((std::max)(std::ceil(a_size - 0.5f), 0.0f)); }
	}

	Builder::Builder(BuildState& a_state, Programs& a_programs, const Shape& a_shape, std::string_view a_effect) :
		state(a_state), programs(a_programs), shape(a_shape), effect(a_effect) {}

	const Image& Builder::Current() const { return state.current; }

	void Builder::SetCurrent(const Image& a_image)
	{
		if (!a_image) {
			Fail("its output image is missing");
			return;
		}
		state.current = a_image;
	}

	Image Builder::CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags)
	{
		return CreateImage(a_name, a_format, a_bindFlags, shape.width, shape.height);
	}

	Image Builder::CreateMipChain(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_mips)
	{
		return CreateImage(a_name, a_format, a_bindFlags, shape.width, shape.height, a_mips);
	}

	Image Builder::CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_width, UINT a_height, UINT a_mips)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = (std::max)(a_mips, 1u);
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | a_bindFlags;
		// Transient and not exported: the graph aliases it with the chain's other images.
		auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(a_name));
		if (!*texture) {
			Fail(std::format("{} could not be created", a_name));
			return {};
		}
		Image image{ texture->Graph(), a_format };
		state.images.push_back(std::move(texture));
		return image;
	}

	void Builder::Register(const GraphResources::Resource& a_resource)
	{
		if (!a_resource) {
			Fail(std::format("{} is missing", a_resource.Name()));
			return;
		}
		state.registered.emplace_back(a_resource.Name(), a_resource.Graph());
	}

	void Builder::Publish(std::string_view a_name, const Image& a_image) { state.published.insert_or_assign(std::string(a_name), a_image); }

	Image Builder::Find(std::string_view a_name) const
	{
		const auto found = state.published.find(a_name);
		return found != state.published.end() ? found->second : Image{};
	}

	void Builder::PublishLatchBytes(std::string_view a_name, std::uint32_t a_offset, std::uint32_t a_size)
	{
		state.publishedLatches.insert_or_assign(std::string(a_name), std::pair{ a_offset, a_size });
	}

	std::optional<std::uint32_t> Builder::FindLatchBytes(std::string_view a_name, std::uint32_t a_size)
	{
		const auto found = state.publishedLatches.find(a_name);
		if (found == state.publishedLatches.end())
			return std::nullopt;
		if (found->second.second != a_size) {
			Fail(std::format("the latch region {} holds {} bytes, not {}", a_name, found->second.second, a_size));
			return std::nullopt;
		}
		return found->second.first;
	}

	std::uint32_t Builder::Reserve(std::uint32_t a_bytes)
	{
		const std::uint32_t offset = (state.latchUsed + 15u) & ~15u;
		if (offset + a_bytes > kLatchBytes) {
			Fail(std::format("the chain's latch is full ({} bytes)", kLatchBytes));
			return 0;
		}
		state.latchUsed = offset + a_bytes;
		return offset;
	}

	GateEntry Builder::Gate()
	{
		if (state.gateUsed >= kGateEntries) {
			Fail(std::format("the chain's run gate is full ({} entries)", kGateEntries));
			return {};
		}
		return { state.gateUsed++ };
	}

	std::optional<std::uint32_t> Builder::FixedEntry(const std::array<std::uint32_t, 3>& a_groups)
	{
		// Dispatches of one size share an entry: the chain's commit opens each size once.
		for (const auto& [groups, entry] : state.fixed) {
			if (groups == a_groups)
				return entry;
		}
		const auto entry = Gate();
		if (Failed())
			return std::nullopt;
		state.fixed.emplace_back(a_groups, entry.index);
		return entry.index;
	}

	std::shared_ptr<const org::LatchBlock> Builder::LatchBlock() const { return state.latch; }

	std::shared_ptr<const RasterProgram> Builder::Raster(std::string_view a_source, std::uint32_t a_words, std::vector<DXGI_FORMAT> a_targets,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		std::vector<RasterProgram::Target> targets;
		for (const auto format : a_targets)
			targets.push_back({ .format = rhi::helpers::ToRHI(format) });
		return RasterTargets(a_source, a_words, std::move(targets), std::move(a_defines), std::move(a_entry));
	}

	std::shared_ptr<const RasterProgram> Builder::RasterTargets(std::string_view a_source, std::uint32_t a_words, std::vector<RasterProgram::Target> a_targets,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		auto key = ProgramKey(a_source, a_entry, a_words, a_defines);
		for (const auto& target : a_targets) {
			const auto& blend = target.blend;
			key += std::format("|f{}", static_cast<int>(target.format));
			if (blend.enable)
				key += std::format("b{},{},{},{},{},{},{}", static_cast<int>(blend.srcColor), static_cast<int>(blend.dstColor), static_cast<int>(blend.colorOp),
					static_cast<int>(blend.srcAlpha), static_cast<int>(blend.dstAlpha), static_cast<int>(blend.alphaOp), static_cast<int>(blend.writeMask));
			else if (blend.writeMask != rhi::ColorWriteEnable::All)
				key += std::format("m{}", static_cast<int>(blend.writeMask));
		}
		if (const auto found = programs.raster.find(key); found != programs.raster.end())
			return found->second;
		if (programs.chainBuilt)
			logger::info("[ORG] Post Processing: {} ({}) is new to this session; building it now", a_source, key);
		RasterProgram::Desc desc{};
		desc.pixelSource = std::string(a_source);
		desc.pixelEntry = std::move(a_entry);
		desc.constantWords = a_words;
		desc.defines = RenderGraph::AnchorPasses::Defines(a_defines);
		desc.targets = std::move(a_targets);
		auto program = RasterProgram::Load(programs.device, desc);
		if (!program) {
			Fail(std::format("{} did not build", a_source));
			return nullptr;
		}
		programs.raster.emplace(std::move(key), program);
		return program;
	}

	std::shared_ptr<const ComputeProgram> Builder::Compute(std::string_view a_source, std::uint32_t a_words,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		auto key = ProgramKey(a_source, a_entry, a_words, a_defines);
		if (const auto found = programs.compute.find(key); found != programs.compute.end())
			return found->second;
		if (programs.chainBuilt)
			logger::info("[ORG] Post Processing: {} ({}) is new to this session; building it now", a_source, key);
		auto program = ComputeProgram::Load(programs.device,
			{ .source = std::string(a_source), .entry = std::move(a_entry), .constantWords = a_words, .defines = RenderGraph::AnchorPasses::Defines(a_defines) });
		if (!program) {
			Fail(std::format("{} did not build", a_source));
			return nullptr;
		}
		programs.compute.emplace(std::move(key), program);
		return program;
	}

	RasterProgram::Viewport Builder::FullViewport() const { return { .width = shape.width, .height = shape.height }; }

	RasterProgram::Viewport Builder::DynamicViewport() const
	{
		return { .width = (std::min)(CoveredPixels(shape.dynamicWidth), shape.width), .height = (std::min)(CoveredPixels(shape.dynamicHeight), shape.height) };
	}

	RasterProgram::Viewport Builder::MipViewport(std::uint32_t a_mip) const
	{
		if (a_mip == kAllMips)
			return FullViewport();
		return { .width = (std::max)(shape.width >> a_mip, 1u), .height = (std::max)(shape.height >> a_mip, 1u) };
	}

	void Builder::Draw(FullscreenDraw a_draw)
	{
		const auto viewport = a_draw.viewport.value_or(MipViewport(a_draw.target.mip));
		state.passes->Add(RenderGraph::AnchorPasses::Draw{ .name = Step(a_draw.name),
			.program = std::move(a_draw.program),
			.bindings = Bases(a_draw.bindings),
			.target = a_draw.target.graph,
			.mip = a_draw.target.mip,
			.viewport = viewport,
			.gate = kDrawEntry });
	}

	void Builder::Dispatch(ComputeDispatch a_dispatch)
	{
		RenderGraph::AnchorPasses::Dispatch dispatch{ .name = Step(a_dispatch.name),
			.program = std::move(a_dispatch.program),
			.bindings = Bases(a_dispatch.bindings),
			.groups = a_dispatch.groups };
		if (a_dispatch.once) {
			if (a_dispatch.gate) {
				Fail(std::format("{}: a one-shot dispatch has fixed group counts, not a gate entry", a_dispatch.name));
				return;
			}
			state.once.push_back(std::format("{}.{}", kClientId, dispatch.name));
		} else if (a_dispatch.gate) {
			dispatch.gate = a_dispatch.gate->index;
		} else {
			const auto entry = FixedEntry(a_dispatch.groups);
			if (!entry)
				return;
			dispatch.gate = *entry;
		}
		state.passes->Add(std::move(dispatch));
	}

	void Builder::Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make)
	{
		state.passes->Add(Step(a_name), a_kind, std::move(a_make));
	}

	void Builder::Fail(std::string_view a_reason)
	{
		if (state.failure.empty())
			state.failure = std::format("{}: {}", effect, a_reason);
	}

	bool Builder::Failed() const { return !state.failure.empty() || state.passes->Failed(); }

	std::string Builder::Step(std::string_view a_step) const { return std::format("{}.{}", effect, a_step); }

	void CommitContext::WriteBytes(std::uint32_t a_offset, const void* a_data, std::size_t a_size) const
	{
		latch.Write(slot, a_offset, std::span<const std::byte>(static_cast<const std::byte*>(a_data), a_size));
	}

	void CommitContext::Open(GateEntry a_entry, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const { gate.OpenDispatch(a_entry.index, a_x, a_y, a_z); }
}

namespace
{
	using ORGPostChain::Bind;
	using Index = PostProcessing::FeaturePipelineIndex;

	constexpr std::size_t kSlots = static_cast<std::size_t>(Index::COUNT);

	/** @brief The ORG implementation of a pipeline slot, or null: not ported, so the chain runs on D3D11 while it is enabled. */
	std::unique_ptr<ORGPostChain::Effect> MakeEffect(Index a_index)
	{
		switch (a_index) {
		case Index::Vignette:
			return ORGPostChain::MakeVignette();
		case Index::ColorGrading:
			return ORGPostChain::MakeColorGrading();
		case Index::LUT:
			return ORGPostChain::MakeLUT();
		case Index::Border:
			return ORGPostChain::MakeBorder();
		case Index::LensFlare:
			return ORGPostChain::MakeLensFlare();
		case Index::CODBloom:
			return ORGPostChain::MakeCODBloom();
		case Index::PhysicalGlare:
			return ORGPostChain::MakePhysicalGlare();
		case Index::DoF:
			return ORGPostChain::MakeDoF();
		case Index::Camera:
			return ORGPostChain::MakeCamera();
		case Index::Composite:
			return ORGPostChain::MakeComposite();
		case Index::MotionBlur:
			return ORGPostChain::MakeMotionBlur();
		case Index::LocalExposure:
			return ORGPostChain::MakeLocalExposure();
		case Index::AutoExposure:
			return ORGPostChain::MakeHistogramAutoExposure();
		default:
			return nullptr;
		}
	}

	/** @brief A slot's step in the pass names: "cs.pp.<step>.<pass>". */
	const char* StepName(Index a_index)
	{
		switch (a_index) {
		case Index::DoF:
			return "dof";
		case Index::Vignette:
			return "vignette";
		case Index::LocalExposure:
			return "local-exposure";
		case Index::AutoExposure:
			return "auto-exposure";
		case Index::MotionBlur:
			return "motion-blur";
		case Index::PhysicalGlare:
			return "physical-glare";
		case Index::CODBloom:
			return "cod-bloom";
		case Index::LensFlare:
			return "lens-flare";
		case Index::Composite:
			return "composite";
		case Index::ColorGrading:
			return "color-grading";
		case Index::LUT:
			return "lut";
		case Index::Camera:
			return "camera";
		case Index::Border:
			return "border";
		default:
			return "effect";
		}
	}

	// copy.ps.hlsl's push block: COPY_RESOURCES under ORG, in order.
	struct CopyBlock
	{
		ShaderBindings::Handle texSrc, texSrcAlt;
		ShaderBindings::Latch copy;
	};

	// copy.ps.hlsl's CopyConstants: PostProcessing::CopyCB with the ORG source selector in its last word.
	struct CopyLatch
	{
		std::uint32_t inputGamut = 0;
		std::uint32_t outputGamut = 0;
		float gamma = 1.0f;
		std::uint32_t source = 0;
	};
	static_assert(sizeof(CopyLatch) == sizeof(PostProcessing::CopyCB));

	std::uint64_t Bits(float a_value)
	{
		std::uint32_t bits = 0;
		std::memcpy(&bits, &a_value, sizeof(bits));
		return bits;
	}
}

struct ORGPostProcessing::Impl
{
	ORGPostChain::Programs programs;
	std::array<std::unique_ptr<ORGPostChain::Effect>, kSlots> effects;

	// The registered build, what it was made for, and the last key that failed to build (not tried again).
	std::shared_ptr<ORGPostChain::BuildState> build;
	std::vector<std::uint64_t> key;
	std::vector<std::uint64_t> failedKey;
	ORGPostChain::LatchRegion<CopyLatch> input, output;
	bool failed = false;

	// This frame's, from BeforeTonemap, for the commit.
	bool ready = false;
	bool ran = false;
	bool readsMainCopy = false;
	bool linear = false;
	bool colorGraded = false;
	std::vector<Index> order;

	// Why the chain runs on D3D11 (empty: on the graph), logged when it changes.
	std::optional<std::string> reason;

	void SetReason(std::string a_reason)
	{
		if (reason && *reason == a_reason)
			return;
		if (a_reason.empty())
			logger::info("[ORG] Post Processing: the chain runs on the render graph ({} passes)", build ? build->descs.size() : 0);
		else
			logger::info("[ORG] Post Processing: the chain runs on D3D11: {}", a_reason);
		reason = std::move(a_reason);
	}

	/** @brief The chain for a_order as a new client of the Tonemap anchor. False, with the reason in a_failure, when it cannot. */
	bool Rebuild(const ORGPostChain::Shape& a_shape, const ORGPostChain::Image& a_main, const ORGPostChain::Image& a_mainCopy,
		const std::vector<Index>& a_order, std::vector<std::uint64_t> a_key, std::string& a_failure)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host) {
			a_failure = "the render graph has no host";
			return false;
		}
		auto next = std::make_shared<ORGPostChain::BuildState>();
		next->latch = std::make_shared<org::LatchBlock>("cs.pp.latch", ORGPostChain::kLatchBytes, host->FrameSlots());
		next->gate = RenderGraph::RunGate::Create("cs.pp.gate", ORGPostChain::kGateEntries);
		if (!next->gate) {
			a_failure = "the chain's run gate could not be made";
			return false;
		}
		next->passes = std::make_unique<RenderGraph::AnchorPasses::Builder>(FrameAnchors::Anchor::Tonemap, ORGPostChain::kClientId, ORGPostChain::kOrderBase,
			next->latch, next->gate);
		auto& pp = globals::features::postProcessing;
		ORGPostChain::LatchRegion<CopyLatch> nextInput, nextOutput;

		// As BeginLinearProcessing: the scene in RGBA16F, linear unless Linear Lighting already is. Under ORG the input pass also
		// picks kMAIN or kMAIN_COPY (CopyLatch::source), so one recording serves either.
		{
			ORGPostChain::Builder builder(*next, programs, a_shape, "input");
			auto program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { ORGPostChain::kInputFormat });
			auto image = builder.CreateImage("PostProcessing::Input", ORGPostChain::kInputFormat, D3D11_BIND_RENDER_TARGET);
			nextInput = builder.Latch<CopyLatch>();
			builder.Draw({ .name = "copy",
				.program = std::move(program),
				.bindings = { Bind::Srv(a_main), Bind::Srv(a_mainCopy), Bind::Latch(nextInput) },
				.target = image });
			builder.SetCurrent(image);
		}

		for (const auto index : a_order) {
			if (!next->failure.empty() || next->passes->Failed())
				break;
			ORGPostChain::Builder builder(*next, programs, a_shape, StepName(index));
			effects[static_cast<std::size_t>(index)]->Build(builder, *pp.pipeline[static_cast<std::size_t>(index)]);
		}

		// As PreProcess's two CopyToRenderTarget: the chain's output into both main targets. Both receive the same image, as the
		// D3D11 copy of one into the other gives (when their formats differ, each is converted from the output directly). Each
		// is a copy.ps draw, also without a conversion (the latch then holds gamma 1 and one gamut, and copy.ps returns the texel
		// it loads): a copy has no run gate, and the chain's passes do nothing on a frame the chain does not run.
		{
			ORGPostChain::Builder builder(*next, programs, a_shape, "output");
			const auto chainOutput = builder.Current();
			nextOutput = builder.Latch<CopyLatch>();
			for (const auto& [target, name] : { std::pair{ &a_main, "main" }, std::pair{ &a_mainCopy, "main-copy" } }) {
				builder.Draw({ .name = name,
					.program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { target->format }),
					.bindings = { Bind::Srv(chainOutput), Bind::Unused(), Bind::Latch(nextOutput) },
					.target = *target });
			}
		}

		if (!next->failure.empty() || next->passes->Failed()) {
			a_failure = next->Failure();
			return false;
		}
		next->descs = next->passes->Take();
		for (auto& desc : next->descs)
			desc.once = std::ranges::find(next->once, desc.name) != next->once.end();

		FrameAnchors::ClientDesc client{};
		client.id = ORGPostChain::kClientId;
		client.passes = next->descs;
		client.sharedConstants = next->passes->ReadsSharedConstants();
		client.gates = { next->gate };
		client.registerResources = [next](org::RenderGraph& a_graph) {
			for (const auto& image : next->images)
				image->Register(a_graph);
			for (const auto& [name, resource] : next->registered)
				a_graph.RegisterResource(org::ResourceIdentifier(name), resource);
		};
		client.commit = [this](const FrameAnchors::Point&) { Commit(); };
		client.executed = [this](const FrameAnchors::Point&) { Executed(); };
		client.active = [this] { return ready; };
		client.onFailed = [this] {
			logger::error("[ORG] Post Processing: the render graph failed; the chain returns to D3D11 for the session");
			failed = true;
			ready = false;
			ran = false;
			build.reset();
			key.clear();
		};
		if (!FrameAnchors::Register(std::move(client))) {
			a_failure = "the Tonemap anchor refused the client";
			return false;
		}
		build = std::move(next);
		key = std::move(a_key);
		programs.chainBuilt = true;
		input = nextInput;
		output = nextOutput;
		std::string steps = "input";
		for (const auto index : a_order)
			steps += std::format(", {}", StepName(index));
		logger::info("[ORG] Post Processing: chain built ({}, output; {} passes, {}x{})", steps, build->descs.size(), a_shape.width, a_shape.height);
		return true;
	}

	/** @brief In the epoch: this frame's values into the latch, and the chain's draws and dispatches opened. */
	void Commit()
	{
		ran = false;
		if (!ready || !build)
			return;
		auto& pp = globals::features::postProcessing;
		const auto& gate = *build->gate;
		const ORGPostChain::CommitContext commit(*build->latch, RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), gate);
		gate.OpenDraw(ORGPostChain::kDrawEntry);
		for (const auto& [groups, entry] : build->fixed)
			gate.OpenDispatch(entry, groups[0], groups[1], groups[2]);
		commit.Write(input, CopyLatch{ .gamma = linear ? 1.0f : Util::ColorSpace::GAME_GAMMA, .source = readsMainCopy ? 1u : 0u });
		for (const auto index : order)
			effects[static_cast<std::size_t>(index)]->Commit(commit, *pp.pipeline[static_cast<std::size_t>(index)]);
		const auto conversion = pp.OutputConversion(colorGraded);
		commit.Write(output, CopyLatch{ .inputGamut = static_cast<std::uint32_t>(conversion.inputGamut),
								 .outputGamut = static_cast<std::uint32_t>(conversion.outputGamut),
								 .gamma = conversion.gamma });
		ran = true;
	}

	/** @brief After a run whose commit reached the GPU with this build: the effects' opened work is done. */
	void Executed()
	{
		auto& pp = globals::features::postProcessing;
		for (const auto index : order)
			effects[static_cast<std::size_t>(index)]->Executed(*pp.pipeline[static_cast<std::size_t>(index)]);
	}
};

ORGPostProcessing::~ORGPostProcessing() = default;

void ORGPostProcessing::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Tonemap))
		logger::warn("[ORG] Post Processing: the Tonemap anchor could not be reserved; the chain stays on D3D11");
}

void ORGPostProcessing::Setup()
{
	if (!Requested() || impl)
		return;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.Host() || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Tonemap)) {
		logger::warn("[ORG] Post Processing: CS_ORG_POST_PROCESSING=1 but the render graph or its Tonemap anchor is not available; the chain stays on D3D11");
		return;
	}
	auto state = std::make_unique<Impl>();
	state->programs.device = runtime.Host()->Device();
	std::string ported;
	auto& pp = globals::features::postProcessing;
	for (std::size_t i = 0; i < kSlots; ++i) {
		state->effects[i] = MakeEffect(static_cast<Index>(i));
		if (state->effects[i] && pp.pipeline[i])
			ported += std::format("{}{}", ported.empty() ? "" : ", ", pp.pipeline[i]->GetType());
	}
	impl = std::move(state);
	logger::info("[ORG] Post Processing: the tonemap chain runs on the render graph while every enabled effect is ported ({})", ported);
}

void ORGPostProcessing::BeforeTonemap(bool a_readsMainCopy)
{
	if (!impl)
		return;
	auto& s = *impl;
	s.ready = false;
	s.ran = false;
	if (s.failed || !RenderGraphRuntime::Get().IsActive())
		return;

	auto& pp = globals::features::postProcessing;
	auto* state = globals::state;
	const bool linear = globals::features::linearLighting.IsLinearLightingActive();
	// PreProcess draws nothing in these cases, so neither does the graph.
	if (linear && (state->permutationData.RenderToUI || state->IsMainOrLoadingMenuOpen()))
		return;
	if (pp.bypass && !linear)
		return;
	// Effects11 owns the tonemap: it renders it, or PreProcess runs the D3D11 chain when it cannot.
	if (state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing)
		return;

	pp.UpdatePipeline();
	auto order = pp.ChainOrder();

	// The mixed rule: the graph runs the chain only when it can run every effect in it.
	std::vector<std::uint64_t> key;
	std::string held;
	const auto hold = [&](const std::string& a_why) { held += std::format("{}{}", held.empty() ? "" : "; ", a_why); };
	for (const auto index : order) {
		const auto slot = static_cast<std::size_t>(index);
		auto& feature = *pp.pipeline[slot];
		key.push_back(slot);
		if (!s.effects[slot]) {
			hold(std::format("{} has no ORG implementation", feature.GetType()));
			continue;
		}
		if (const char* why = s.effects[slot]->Update(feature, key))
			hold(std::format("{}: {}", feature.GetType(), why));
	}
	if (!held.empty()) {
		s.SetReason(held);
		return;
	}

	auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	auto mainTarget = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
	auto mainCopyTarget = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN_COPY));
	auto* mainTexture = targets[RE::RENDER_TARGETS::kMAIN].texture;
	auto* mainCopyTexture = targets[RE::RENDER_TARGETS::kMAIN_COPY].texture;
	if (!mainTarget || !mainCopyTarget || !mainTexture || !mainCopyTexture) {
		s.SetReason("kMAIN or kMAIN_COPY cannot be imported");
		return;
	}
	D3D11_TEXTURE2D_DESC mainDesc{}, mainCopyDesc{};
	mainTexture->GetDesc(&mainDesc);
	mainCopyTexture->GetDesc(&mainCopyDesc);
	if (mainDesc.Width != mainCopyDesc.Width || mainDesc.Height != mainCopyDesc.Height) {
		s.SetReason("kMAIN and kMAIN_COPY differ in size");
		return;
	}
	ORGPostChain::Shape shape{};
	shape.width = mainDesc.Width;
	shape.height = mainDesc.Height;
	const auto dynamic = Util::ConvertToDynamic(float2(static_cast<float>(mainDesc.Width), static_cast<float>(mainDesc.Height)));
	shape.dynamicWidth = dynamic.x;
	shape.dynamicHeight = dynamic.y;
	shape.mainFormat = mainDesc.Format;
	shape.mainCopyFormat = mainCopyDesc.Format;

	const bool colorGraded = std::ranges::find(order, Index::ColorGrading) != order.end();
	const auto conversion = pp.OutputConversion(colorGraded);
	for (const std::uint64_t word : { std::uint64_t(shape.width), std::uint64_t(shape.height), Bits(shape.dynamicWidth), Bits(shape.dynamicHeight),
			 std::uint64_t(shape.mainFormat), std::uint64_t(shape.mainCopyFormat), std::uint64_t(linear), reinterpret_cast<std::uintptr_t>(mainTarget.get()), reinterpret_cast<std::uintptr_t>(mainCopyTarget.get()) })
		key.push_back(word);

	if (key != s.key || !s.build) {
		if (key == s.failedKey) {
			s.SetReason("the chain failed to build for these settings (logged above)");
			return;
		}
		std::string failure;
		bool built = false;
		try {
			built = s.Rebuild(shape, { mainTarget, shape.mainFormat }, { mainCopyTarget, shape.mainCopyFormat }, order, key, failure);
		} catch (const std::exception& e) {
			failure = e.what();
		}
		if (!built) {
			logger::error("[ORG] Post Processing: the chain could not be built: {}", failure);
			s.failedKey = std::move(key);
			s.SetReason("the chain failed to build for these settings (logged above)");
			return;
		}
		s.failedKey.clear();
	}

	s.order = std::move(order);
	s.linear = linear;
	s.readsMainCopy = a_readsMainCopy;
	s.colorGraded = colorGraded;
	s.ready = true;
	s.SetReason({});

	// As PreProcess: the main targets leave the output merger, which ISRefraction may still have kMAIN_COPY bound to.
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

bool ORGPostProcessing::Ran() const
{
	return impl && impl->ran;
}

void ORGPostProcessing::ClearShaderCache()
{
	if (!impl)
		return;
	// Failed compiles are cached as null, and a failed key is not tried again: both go, and the next frame builds the chain anew.
	impl->programs.raster.clear();
	impl->programs.compute.clear();
	impl->failedKey.clear();
	impl->key.clear();
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the chain always runs on D3D11.
struct ORGPostProcessing::Impl
{};

ORGPostProcessing::~ORGPostProcessing() = default;
void ORGPostProcessing::Reserve() {}
void ORGPostProcessing::Setup() {}
void ORGPostProcessing::BeforeTonemap(bool) {}
bool ORGPostProcessing::Ran() const { return false; }
void ORGPostProcessing::ClearShaderCache() {}

#endif
