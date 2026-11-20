#include "ORGPostProcessing.h"
#include "Utils/Logging.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGPostProcessing::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
#	include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#	include "RenderGraph/Engine/TonemapAnchor.h"
#	include "Globals.h"
#	include "RenderGraph/FrameStaging.h"
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
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <cmath>
#	include <cstddef>
#	include <cstring>
#	include <format>
#	include <map>
#	include <mutex>
#	include <span>
#	include <stdexcept>
#	include <unordered_map>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kClientId = "cs.pp";
		constexpr const char* kCopyShader = "PostProcessing/copy.ps.hlsl";
		// After any earlier Tonemap client. The chain's images order its passes among themselves.
		constexpr std::int32_t kOrderBase = 1000;
		constexpr DXGI_FORMAT kInputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // PostProcessing::texInput's
		// Every chain draw uses it, and the commit opens it on frames the chain runs.
		constexpr std::uint32_t kDrawEntry = 0;
	}

	/** @brief For the session, so a rebuild finds the programs it built before. */
	struct Programs
	{
		rhi::Device device;
		std::unordered_map<std::string, std::shared_ptr<const RasterProgram>> raster;
		std::unordered_map<std::string, std::shared_ptr<const ComputeProgram>> compute;
		// After the first chain, a newly built program is logged.
		bool chainBuilt = false;
	};

	/** @brief Immutable once registered. */
	struct BuildState
	{
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		std::unique_ptr<RenderGraph::AnchorPasses::Builder> passes;  // until Take
		std::vector<FrameAnchors::PassDesc> descs;
		std::uint32_t latchUsed = 0;
		std::uint32_t gateUsed = kDrawEntry + 1;
		std::vector<std::pair<std::array<std::uint32_t, 3>, std::uint32_t>> fixed;  // a fixed dispatch size and its gate entry
		std::vector<std::string> once;
		std::vector<std::unique_ptr<GraphResources::Texture2D>> images;
		std::vector<std::pair<std::string, std::shared_ptr<org::Resource>>> registered;
		std::vector<const GraphResources::Resource*> twins;  // declared, so alive for the session
		std::map<std::string, Image, std::less<>> published;
		std::map<std::string, std::pair<std::uint32_t, std::uint32_t>, std::less<>> publishedLatches;  // offset, size
		Image current;
		std::string failure;  // empty while the build is good

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
		// Transient and not exported, so it aliases with the chain's other images.
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
		// A declared twin registers its slot, so its versions need no build. Declarations live for the session.
		// Any other twin is held by its graph resource, since its effect may replace it while an older build still runs.
		if (!a_resource.Versioned()) {
			state.registered.emplace_back(a_resource.Name(), a_resource.Graph());
			return;
		}
		if (std::ranges::find(state.twins, &a_resource) == state.twins.end())
			state.twins.push_back(&a_resource);
	}

	void Builder::Register(std::string_view a_name, std::shared_ptr<org::Resource> a_resource)
	{
		if (!a_resource) {
			Fail(std::format("{} is missing", a_name));
			return;
		}
		state.registered.emplace_back(std::string(a_name), std::move(a_resource));
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
		// Dispatches of one size share an entry, opened once.
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

	std::shared_ptr<const RenderGraph::RunGate> Builder::RunGate() const { return state.gate; }

	std::shared_ptr<const RasterProgram> Builder::Raster(std::string_view a_source, std::uint32_t a_words, std::vector<DXGI_FORMAT> a_targets,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		std::vector<RasterProgram::Target> targets;
		for (const auto format : a_targets)
			targets.push_back({ .format = rhi::helpers::ToRHI(format) });
		return RasterTargets(a_source, a_words, std::move(targets), std::move(a_defines), std::move(a_entry));
	}

	std::shared_ptr<const RasterProgram> Builder::RasterTargets(std::string_view a_source, std::uint32_t a_words, std::vector<RasterProgram::Target> a_targets,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry, std::string_view a_vertexSource, std::wstring a_vertexEntry)
	{
		auto key = ProgramKey(a_source, a_entry, a_words, a_defines);
		if (!a_vertexSource.empty())
			key += std::format("|vs:{}:{}", a_vertexSource, Util::WStringToString(a_vertexEntry));
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
		if (!a_vertexSource.empty()) {
			desc.vertexSource = std::string(a_vertexSource);
			desc.vertexEntry = std::move(a_vertexEntry);
		}
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
		// A recording cannot follow the ratio, so the draw covers kMAIN until step 8 puts the viewport in the latch.
		return FullViewport();
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
		latch.Write(run, a_offset, std::span<const std::byte>(static_cast<const std::byte*>(a_data), a_size));
	}

	void CommitContext::Open(GateEntry a_entry, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const { gate.OpenDispatch(a_entry.index, a_x, a_y, a_z); }

	Image ImageOf(const GraphResources::Resource& a_twin, DXGI_FORMAT a_format) { return Image{ RenderGraph::AnchorPasses::TextureOf(a_twin), a_format }; }

	namespace
	{
		/** @brief The legacy form's Run: one state for every build, kept by the effect. */
		class LegacyRun final : public Run
		{
		public:
			explicit LegacyRun(Effect& a_effect) :
				effect(a_effect) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override { effect.Commit(a_commit, a_feature); }
			void Executed(PostProcessFeature& a_feature) override { effect.Executed(a_feature); }

		private:
			Effect& effect;
		};
	}

	std::shared_ptr<Run> Effect::BuildRun(Builder& a_builder, PostProcessFeature& a_feature, std::span<const std::uint64_t>)
	{
		Build(a_builder, a_feature);
		return std::make_shared<LegacyRun>(*this);
	}

	std::shared_ptr<RunReadback> RunReadback::Create(std::string_view a_name, std::vector<std::uint32_t> a_parts)
	{
		auto readback = RenderGraph::RunReadback::Create(a_name, std::move(a_parts));
		return readback ? std::make_shared<RunReadback>(std::move(readback)) : nullptr;
	}

	LatchRegion<RenderGraph::RunReadback::Stamp> RunReadback::BuildStamp(Builder& a_builder)
	{
		const auto region = a_builder.Latch<RenderGraph::RunReadback::Stamp>();
		readback->Bind(a_builder.LatchBlock(), region.offset);
		return region;
	}

	void RunReadback::Build(Builder& a_builder) { stamp = BuildStamp(a_builder); }

	void RunReadback::AddBuffer(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source)
	{
		std::string why;
		auto make = readback->BufferPass(a_part, std::move(a_source), why);
		if (!make) {
			a_builder.Fail(std::format("{}: {}", a_name, why));
			return;
		}
		a_builder.Add(a_name, FrameAnchors::PassKind::Copy, std::move(make));
	}

	void RunReadback::AddTexture(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source,
		std::uint32_t a_width, std::uint32_t a_height)
	{
		std::string why;
		auto make = readback->TexturePass(a_part, std::move(a_source), a_width, a_height, why);
		if (!make) {
			a_builder.Fail(std::format("{}: {}", a_name, why));
			return;
		}
		a_builder.Add(a_name, FrameAnchors::PassKind::Copy, std::move(make));
	}

	std::uint32_t RunReadback::Commit(const CommitContext& a_commit) { return Commit(a_commit, stamp); }

	std::uint32_t RunReadback::Commit(const CommitContext& a_commit, const LatchRegion<RenderGraph::RunReadback::Stamp>& a_stamp)
	{
		const auto value = readback->Commit(a_commit.Run());
		a_commit.Write(a_stamp, RenderGraph::RunReadback::Stamp{ .value = value });
		return value;
	}

	void RunReadback::Executed() { readback->Executed(); }

	std::vector<std::byte> RunReadback::Take(std::uint32_t a_part, std::uint32_t* a_stamp) { return readback->Take(a_part, a_stamp); }

	bool LogOn()
	{
		static const bool s_on = Switches::On("CS_ORG_PP_LOG");
		return s_on;
	}

	bool LogFrame() { return LogOn() && globals::state->frameCount % 120 == 0; }
}

namespace
{
	using ORGPostChain::Bind;
	using Index = PostProcessing::FeaturePipelineIndex;

	constexpr std::size_t kSlots = static_cast<std::size_t>(Index::COUNT);

	/** @brief Null means not ported, so the chain runs on D3D11 while it is enabled. */
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
		case Index::PhysicalLens:
			return ORGPostChain::MakePhysicalLens();
		default:
			return nullptr;
		}
	}

	/** @brief "cs.pp.<step>.<pass>". */
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
		case Index::PhysicalLens:
			return "physical-lens";
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

	/**
	* @brief CS_ORG_PP_PROBE=<step or image>, read once. The output copies take the image after that step (input, or a StepName such as
	* vignette, composite or color-grading), or an image an effect publishes (such as PostProcessing::CODBloom::Bloom), in place of
	* the chain's last image. It finds the step that turns the image black. Every step still runs.
	*/
	const std::string& Probe()
	{
		static const std::string s_probe = Switches::Value("CS_ORG_PP_PROBE");
		return s_probe;
	}

	// COPY_RESOURCES order.
	struct CopyBlock
	{
		ShaderBindings::Handle texSrc, texSrcAlt;
		ShaderBindings::Latch copy;
	};

	// copy.ps.hlsl's CopyConstants: PostProcessing::CopyCB, then the ORG source selector.
	struct CopyLatch
	{
		PostProcessing::CopyCB conversion;
		std::uint32_t source = 0;
		std::uint32_t pad[3]{};
	};
	static_assert(offsetof(CopyLatch, source) == sizeof(PostProcessing::CopyCB));
}

namespace
{
	/** @brief What the last BeforeTonemap found that is no variable yet: the chain's order, kMAIN and the effects' imports. */
	struct Observed
	{
		std::vector<Index> order;
		ORGPostChain::Shape shape;
		FrameAnchors::Texture main, mainCopy;  // mainCopy is empty when the chain reads and writes one target
		std::vector<std::uint64_t> words;      // what the passes depend on of the above, then the effects' Update words

		friend bool operator==(const Observed& a_left, const Observed& a_right) { return a_left.words == a_right.words; }
	};

	/** @brief The chain's key: what it observed, and per effect in its order what Key read from the revision. */
	struct ChainKey
	{
		Observed observed;
		std::vector<std::vector<std::uint64_t>> effects;
		std::uint64_t programs = 0;  // ClearShaderCache's count, so a reload builds again

		friend bool operator==(const ChainKey& a_left, const ChainKey& a_right)
		{
			return a_left.observed == a_right.observed && a_left.effects == a_right.effects && a_left.programs == a_right.programs;
		}
	};

	/** @brief One build of the chain. Its registration holds it, so a frame whose schedule holds it commits it. */
	struct ChainBuild
	{
		std::shared_ptr<ORGPostChain::BuildState> state;
		std::vector<Index> order;
		std::vector<std::shared_ptr<ORGPostChain::Run>> runs;  // one per order entry
		ORGPostChain::LatchRegion<CopyLatch> input, output;
		std::uint64_t generation = 0;
		bool legacy = false;  // holds an effect of the legacy form, so only the newest build may commit
	};
}

struct ORGPostProcessing::Impl
{
	ORGPostChain::Programs programs;
	std::array<std::unique_ptr<ORGPostChain::Effect>, kSlots> effects;

	// The chain's client, declared on the effects' settings. Its key reads observed.
	RenderGraph::Frame::DeclaredClient declared;
	std::optional<Observed> observed;
	// The build the key function made for the registration the revision makes next.
	std::optional<ChainKey> pendingKey;
	std::shared_ptr<ChainBuild> pendingBuild;
	FrameAnchors::ClientDesc pendingDesc;
	// A key that failed to build is not tried again. The chain then runs on D3D11.
	std::optional<ChainKey> failedKey;
	// Builds a registration may still hold, newest last.
	std::vector<std::shared_ptr<ChainBuild>> builds;
	std::uint64_t generation = 0;
	std::uint64_t programsGeneration = 0;

	// Set by BeforeTonemap for the commit.
	bool ready = false;
	bool ran = false;
	bool readsMainCopy = false;
	bool linear = false;
	bool colorGraded = false;
	bool exposureResolved = true;
	std::uint64_t readyGeneration = 0;  // the build this frame's schedule holds
	float2 dynamicSize{};
	// A tonemap after the post stage was seen and logged.
	bool passedLogged = false;

	// Empty means on the graph. Logged when it changes.
	std::optional<std::string> reason;
	// The effects the graph ran last frame. Logged when one joins or leaves.
	std::vector<Index> members;

	void SetReason(std::string a_reason)
	{
		// While the chain is held, no effect runs on the graph.
		if (!a_reason.empty())
			SetMembers({});
		if (reason && *reason == a_reason)
			return;
		if (a_reason.empty())
			logger::info("[ORG] Post Processing: the chain runs on the render graph ({} passes)", builds.empty() ? 0 : builds.back()->state->descs.size());
		else
			logger::info("[ORG] Post Processing: the chain runs on D3D11: {}", a_reason);
		reason = std::move(a_reason);
	}

	/** @brief For a frame where neither path runs the chain. Logged when the reason changes. */
	void SetSkipped(std::string_view a_why)
	{
		auto text = std::format("skipped: {}", a_why);
		if (reason && *reason == text)
			return;
		logger::info("[ORG] Post Processing: no chain runs this frame: {}", a_why);
		reason = std::move(text);
		SetMembers({});
	}

	/** @brief The effects the graph runs this frame. Each one that joins or leaves is logged. */
	void SetMembers(const std::vector<Index>& a_members)
	{
		auto& pp = globals::features::postProcessing;
		const auto name = [&](Index a_index) {
			const auto& feature = pp.pipeline[static_cast<std::size_t>(a_index)];
			return feature ? feature->GetType() : std::string(StepName(a_index));
		};
		for (const auto index : a_members)
			if (std::ranges::find(members, index) == members.end())
				logger::info("[ORG] Post Processing: {} joins the graph chain", name(index));
		for (const auto index : members)
			if (std::ranges::find(a_members, index) == a_members.end())
				logger::info("[ORG] Post Processing: {} leaves the graph chain", name(index));
		members = a_members;
	}

	/** @brief Render thread, at setup. Declares the effects' textures and the chain's client on their settings. */
	void Declare()
	{
		auto& pp = globals::features::postProcessing;
		RenderGraph::Frame::Dependencies dependencies;
		for (std::size_t i = 0; i < kSlots; ++i) {
			if (!effects[i] || !pp.pipeline[i])
				continue;
			dependencies.push_back(RenderGraph::Frame::DependencyOf(pp.pipeline[i]->enabled));
			effects[i]->Declare(*pp.pipeline[i], dependencies);
		}
		declared = RenderGraph::Frame::DeclareClient<ChainKey>(
			ORGPostChain::kClientId, std::move(dependencies), [this](const RenderGraph::Frame::Inputs& a_values) { return Prepare(a_values); },
			[this](const ChainKey& a_key, FrameAnchors::ClientDesc& a_client) { return Make(a_key, a_client); });
	}

	/**
	* @brief The key function, when a revision starts. The chain's key for a_values, built here when it is new, so a key that
	* cannot be built takes the chain off the graph (nullopt) rather than failing the revision. Make then takes the build.
	*/
	std::optional<ChainKey> Prepare(const RenderGraph::Frame::Inputs& a_values)
	{
		if (!observed)
			return std::nullopt;
		auto& pp = globals::features::postProcessing;
		ChainKey key{ .observed = *observed, .programs = programsGeneration };
		for (const auto index : observed->order) {
			const auto slot = static_cast<std::size_t>(index);
			auto& words = key.effects.emplace_back();
			effects[slot]->Key(a_values, *pp.pipeline[slot], words);
		}
		if (pendingKey && *pendingKey == key)
			return key;
		if (failedKey && *failedKey == key)
			return std::nullopt;
		std::string failure;
		bool built = false;
		try {
			built = Rebuild(key, failure);
		} catch (const std::exception& e) {
			failure = e.what();
		}
		if (!built) {
			logger::error("[ORG] Post Processing: the chain could not be built: {}", failure);
			failedKey = std::move(key);
			return std::nullopt;
		}
		failedKey.reset();
		return pendingKey;
	}

	/** @brief The make function: the registration Prepare built for a_key. */
	bool Make(const ChainKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		if (!pendingKey || !(*pendingKey == a_key) || !pendingBuild)
			return false;
		a_client = pendingDesc;
		builds.push_back(pendingBuild);
		return true;
	}

	/** @brief False, with the reason in a_failure, when it cannot. Leaves the build and its registration pending for Make. */
	bool Rebuild(const ChainKey& a_key, std::string& a_failure)
	{
		const auto& shape = a_key.observed.shape;
		const ORGPostChain::Image main{ a_key.observed.main, shape.mainFormat };
		const ORGPostChain::Image mainCopy{ a_key.observed.mainCopy, shape.mainCopyFormat };
		const auto& order = a_key.observed.order;
		// CS's native chain has one image, read and written back.
		const auto& second = mainCopy ? mainCopy : main;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host) {
			a_failure = "the render graph has no host";
			return false;
		}
		auto next = std::make_shared<ORGPostChain::BuildState>();
		next->latch = std::make_shared<org::LatchBlock>("cs.pp.latch", ORGPostChain::kLatchBytes);
		next->gate = RenderGraph::RunGate::Create("cs.pp.gate", ORGPostChain::kGateEntries);
		if (!next->gate) {
			a_failure = "the chain's run gate could not be made";
			return false;
		}
		next->passes = std::make_unique<RenderGraph::AnchorPasses::Builder>(FrameAnchors::Anchor::Tonemap, ORGPostChain::kClientId, ORGPostChain::kOrderBase,
			next->latch, next->gate);
		auto& pp = globals::features::postProcessing;
		auto build = std::make_shared<ChainBuild>();
		build->state = next;
		build->order = order;

		// As BeginLinearProcessing. The input pass also picks kMAIN or kMAIN_COPY from the latch, so one recording serves either.
		{
			ORGPostChain::Builder builder(*next, programs, shape, "input");
			auto program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { ORGPostChain::kInputFormat });
			auto image = builder.CreateImage("PostProcessing::Input", ORGPostChain::kInputFormat, D3D11_BIND_RENDER_TARGET);
			build->input = builder.Latch<CopyLatch>();
			builder.Draw({ .name = "copy",
				.program = std::move(program),
				.bindings = { Bind::Srv(main), Bind::Srv(second), Bind::Latch(build->input) },
				.target = image });
			builder.SetCurrent(image);
		}
		// CS_ORG_PP_PROBE: the image the output copies take in place of the chain's last one.
		const auto& probe = Probe();
		ORGPostChain::Image probed;
		if (probe == "input")
			probed = next->current;

		for (std::size_t i = 0; i < order.size(); ++i) {
			if (!next->failure.empty() || next->passes->Failed())
				break;
			const auto index = order[i];
			const auto slot = static_cast<std::size_t>(index);
			ORGPostChain::Builder builder(*next, programs, shape, StepName(index));
			auto run = effects[slot]->BuildRun(builder, *pp.pipeline[slot], a_key.effects[i]);
			if (!run && !builder.Failed())
				builder.Fail("it made no run");
			build->runs.push_back(std::move(run));
			build->legacy = build->legacy || effects[slot]->Legacy();
			if (probe == StepName(index))
				probed = next->current;
		}
		if (!probe.empty() && !probed) {
			if (const auto found = next->published.find(probe); found != next->published.end())
				probed = found->second;
		}
		if (!probe.empty()) {
			if (probed)
				logger::warn("[ORG] Post Processing: CS_ORG_PP_PROBE={}: the output copies take that image, not the chain's last one", probe);
			else
				logger::warn("[ORG] Post Processing: CS_ORG_PP_PROBE={} names no step of this chain and no image it publishes; the probe is off", probe);
		}

		// As PreProcess's two CopyToRenderTarget, into both main targets. Each is a copy.ps draw rather than a copy,
		// because a copy has no run gate and would run on frames the chain does not.
		{
			ORGPostChain::Builder builder(*next, programs, shape, "output");
			const auto chainOutput = probed ? probed : builder.Current();
			build->output = builder.Latch<CopyLatch>();
			for (const auto& [target, name] : { std::pair{ &main, "main" }, std::pair{ &mainCopy, "main-copy" } }) {
				if (!*target)
					continue;
				builder.Draw({ .name = name,
					.program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { target->format }),
					.bindings = { Bind::Srv(chainOutput), Bind::Unused(), Bind::Latch(build->output) },
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
		// The post stage is recorded ahead and its recording is submitted again, so every pass must give a revision.
		// A pass without one makes the stage's recording fatal at its first frame, so name it here.
		for (const auto& desc : next->descs) {
			const auto pass = desc.make ? desc.make() : nullptr;
			std::uint64_t revision = 0;
			if (!pass || !pass->RevisionHash(revision)) {
				a_failure = std::format("{} gives no revision, so a kept recording cannot hold it", desc.name);
				return false;
			}
		}

		build->generation = ++generation;
		FrameAnchors::ClientDesc client{};
		client.id = ORGPostChain::kClientId;
		client.passes = next->descs;
		client.sharedConstants = next->passes->ReadsSharedConstants();
		client.gates = { next->gate };
		client.registerResources = [next](org::RenderGraph& a_graph) {
			for (const auto& image : next->images)
				image->Register(a_graph);
			for (const auto* twin : next->twins)
				twin->Register(a_graph);
			for (const auto& [name, resource] : next->registered)
				a_graph.RegisterResource(org::ResourceIdentifier(name), resource);
		};
		// Each registration commits its own build. A schedule that still holds an older one taps as it (matchesById),
		// unless an effect of the legacy form shares its state across builds.
		client.commit = [this, build](const FrameAnchors::Point&) { Commit(*build); };
		client.executed = [this, build](const FrameAnchors::Point&) { Executed(*build); };
		client.active = [this] { return ready; };
		client.matchesById = !build->legacy;
		client.generation = build->generation;
		programs.chainBuilt = true;
		std::string steps = "input";
		for (const auto index : order)
			steps += std::format(", {}", StepName(index));
		logger::info("[ORG] Post Processing: chain {} built for the next revision ({}, output; {} passes, {}x{})", build->generation, steps, next->descs.size(),
			shape.width, shape.height);
		pendingKey = a_key;
		pendingBuild = std::move(build);
		pendingDesc = std::move(client);
		return true;
	}

	void Commit(const ChainBuild& a_build)
	{
		ran = false;
		if (!ready || a_build.generation != readyGeneration)
			return;
		auto& pp = globals::features::postProcessing;
		const auto& gate = *a_build.state->gate;
		const ORGPostChain::CommitContext commit(*a_build.state->latch, RenderGraphRuntime::Get().Host()->CurrentRun(), gate, dynamicSize);
		gate.OpenDraw(ORGPostChain::kDrawEntry);
		for (const auto& [groups, entry] : a_build.state->fixed)
			gate.OpenDispatch(entry, groups[0], groups[1], groups[2]);
		commit.Write(a_build.input, CopyLatch{ .conversion = { .gamma = linear ? 1.0f : Util::ColorSpace::GAME_GAMMA }, .source = readsMainCopy ? 1u : 0u });
		for (std::size_t i = 0; i < a_build.order.size(); ++i)
			a_build.runs[i]->Commit(commit, *pp.pipeline[static_cast<std::size_t>(a_build.order[i])]);
		commit.Write(a_build.output, CopyLatch{ .conversion = pp.OutputConversion(colorGraded, exposureResolved) });
		ran = true;
	}

	/** @brief After a run whose commit reached the GPU with a_build. */
	void Executed(const ChainBuild& a_build)
	{
		auto& pp = globals::features::postProcessing;
		for (std::size_t i = 0; i < a_build.order.size(); ++i)
			a_build.runs[i]->Executed(*pp.pipeline[static_cast<std::size_t>(a_build.order[i])]);
	}

	/** @brief The build this frame's schedule holds, or null with the reason in a_why. Drops the builds older than it. */
	const ChainBuild* HeldBuild(std::string& a_why)
	{
		if (builds.empty()) {
			a_why = failedKey ? "the chain failed to build for these settings (logged above)" : "the chain's client is not registered yet";
			return nullptr;
		}
		// A frame kind without a post stage, such as the main menu's, has no place for the chain, so D3D11 draws it.
		const auto hold = FrameAnchors::HeldNow(ORGPostChain::kClientId, FrameAnchors::Anchor::Tonemap);
		if (!hold.held) {
			a_why = "this frame has no post stage that holds the chain";
			return nullptr;
		}
		if (!hold.scheduled) {
			a_why = "the post stage has no schedule that holds the chain yet";
			return nullptr;
		}
		const auto found = std::ranges::find_if(builds, [&](const auto& a_build) { return a_build->generation == hold.generation; });
		if (found == builds.end()) {
			static Util::ReasonLog s_lost("[ORG] Post Processing: the post stage holds a chain build this driver no longer has", spdlog::level::err);
			s_lost.Report(std::format("build {}", hold.generation));
			a_why = "the post stage holds an unknown chain build (logged above)";
			return nullptr;
		}
		builds.erase(builds.begin(), found);
		const auto& build = *builds.front();
		if (build.legacy && build.generation != builds.back()->generation) {
			a_why = "the post stage still runs an older build, and Physical Lens or Camera FX keep one state for every build";
			return nullptr;
		}
		return &build;
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
	state->Declare();
	impl = std::move(state);
	logger::info("[ORG] Post Processing: the tonemap chain runs on the render graph while every enabled effect is ported ({}); it follows the frame revisions",
		ported);
}

void ORGPostProcessing::BeforeTonemap(const RenderGraph::Engine::TonemapTargets& a_targets, bool a_refraction)
{
	if (!impl)
		return;
	auto& s = *impl;
	s.ready = false;
	s.ran = false;
	if (!RenderGraphRuntime::Get().IsActive()) {
		s.SetSkipped("the render graph is not active");
		return;
	}
	// A second tonemap in the frame can come after the post stage has ended. The loading menu's 3D model draws one in every frame
	// of DCLF's load hold. No stage takes it, so D3D11 runs the chain for it, as in a frame kind without a post stage.
	// It is not the world's tonemap, so it leaves the logged state alone. Otherwise the chain would seem to leave and rejoin the graph each frame.
	if (FrameAnchors::StagePassed(ORGPostChain::kClientId, FrameAnchors::Anchor::Tonemap)) {
		if (!s.passedLogged) {
			s.passedLogged = true;
			logger::info("[ORG] Post Processing: a tonemap after this frame's post stage, such as a menu scene's, runs the chain on D3D11 (logged once)");
		}
		return;
	}

	auto& pp = globals::features::postProcessing;
	auto* state = globals::state;
	const bool linear = globals::features::linearLighting.IsLinearLightingActive();
	// PreProcess draws nothing here, so neither does the graph.
	if (linear && (state->permutationData.RenderToUI || state->IsMainOrLoadingMenuOpen())) {
		s.SetSkipped("a menu or UI frame under linear lighting");
		return;
	}
	if (pp.bypass && !linear) {
		s.SetSkipped("Post Processing is bypassed");
		return;
	}
	// Effects11 owns the tonemap.
	if (state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing) {
		s.SetSkipped("Post Processing does not own the tonemap");
		return;
	}

	pp.UpdatePipeline();
	auto order = pp.ChainOrder();
	// As PreProcess: without Composite the scene stays pre-exposed, so grading is skipped and the output copy divides it out.
	const bool exposureResolved = !linear || state->preExposure == 1.0f || std::ranges::find(order, Index::Composite) != order.end();
	if (!exposureResolved)
		std::erase_if(order, [&](Index a_index) { return a_index == Index::ColorGrading || pp.pipeline[static_cast<std::size_t>(a_index)]->DrawAfterColorGrading(); });

	// The graph runs the chain only when it can run every effect in it. A held frame keeps the observed state, so the build stays.
	Observed seen{ .order = order };
	std::string held;
	const auto hold = [&](const std::string& a_why) { held += std::format("{}{}", held.empty() ? "" : "; ", a_why); };
	for (const auto index : order) {
		const auto slot = static_cast<std::size_t>(index);
		auto& feature = *pp.pipeline[slot];
		seen.words.push_back(slot);
		if (!s.effects[slot]) {
			hold(std::format("{} has no ORG implementation", feature.GetType()));
			continue;
		}
		if (const char* why = s.effects[slot]->Update(feature, seen.words))
			hold(std::format("{}: {}", feature.GetType(), why));
	}
	if (!held.empty()) {
		s.SetReason(held);
		return;
	}

	auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	// Native: kMAIN is read and written back. Guest: either main target may hold the scene, and both get the result.
	const bool single = a_targets.chain;
	auto mainTarget = single ? a_targets.input : FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
	auto mainCopyTarget = single ? FrameAnchors::Texture{} : FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN_COPY));
	auto* mainTexture = targets[RE::RENDER_TARGETS::kMAIN].texture;
	auto* mainCopyTexture = targets[RE::RENDER_TARGETS::kMAIN_COPY].texture;
	if (!mainTarget || !mainTexture || (!single && (!mainCopyTarget || !mainCopyTexture))) {
		s.SetReason("kMAIN or kMAIN_COPY cannot be imported");
		return;
	}
	D3D11_TEXTURE2D_DESC mainDesc{}, mainCopyDesc{};
	mainTexture->GetDesc(&mainDesc);
	if (!single) {
		mainCopyTexture->GetDesc(&mainCopyDesc);
		if (mainDesc.Width != mainCopyDesc.Width || mainDesc.Height != mainCopyDesc.Height) {
			s.SetReason("kMAIN and kMAIN_COPY differ in size");
			return;
		}
	} else
		mainCopyDesc = mainDesc;
	seen.shape.width = mainDesc.Width;
	seen.shape.height = mainDesc.Height;
	seen.shape.mainFormat = mainDesc.Format;
	seen.shape.mainCopyFormat = mainCopyDesc.Format;
	seen.main = mainTarget;
	seen.mainCopy = mainCopyTarget;
	// The dynamic-resolution ratio is a per-frame value: it reaches the commit, never the key.
	s.dynamicSize = Util::ConvertToDynamic(float2(static_cast<float>(mainDesc.Width), static_cast<float>(mainDesc.Height)));
	for (const std::uint64_t word : { std::uint64_t(seen.shape.width), std::uint64_t(seen.shape.height), std::uint64_t(seen.shape.mainFormat),
			 std::uint64_t(seen.shape.mainCopyFormat), std::uint64_t(linear), reinterpret_cast<std::uintptr_t>(mainTarget.Key()),
			 reinterpret_cast<std::uintptr_t>(mainCopyTarget.Key()) })
		seen.words.push_back(word);

	// A new shape is a new key, built when the next revision starts. This frame runs the build its schedule holds.
	if (!s.observed || !(*s.observed == seen)) {
		s.observed = std::move(seen);
		s.declared.Invalidate();
	}

	std::string why;
	const auto* build = s.HeldBuild(why);
	if (!build) {
		s.SetReason(why);
		return;
	}

	const bool colorGraded = std::ranges::find(build->order, Index::ColorGrading) != build->order.end();
	s.linear = linear;
	// CS's ImageSpace steps keep the world scene in kMAIN, so the engine's kMAIN_COPY input or a refraction does not move it.
	// kMAIN_COPY then holds an older copy of the scene at best, from before the volumetric composite and the upscale.
	const bool sceneInMain = RenderGraph::Engine::ImageSpace::SceneInMain();
	s.readsMainCopy = !single && !sceneInMain && (a_refraction || a_targets.input == mainCopyTarget);
	s.colorGraded = colorGraded;
	s.exposureResolved = exposureResolved;
	s.readyGeneration = build->generation;
	s.ready = true;
	s.SetReason({});
	s.SetMembers(build->order);
	if (ORGPostChain::LogFrame())
		logger::info("[ORG] Post Processing log: frame {}: build {} reads {} (one target {}, refraction {}, engine input kMAIN_COPY {}, scene in kMAIN {}); "
					 "linear {}, exposure resolved {}, pre-exposure {}",
			globals::state->frameCount, build->generation, s.readsMainCopy ? "kMAIN_COPY" : "kMAIN", single, a_refraction,
			a_targets.input == mainCopyTarget && !single, sceneInMain, linear, exposureResolved, state->preExposure);

	// As PreProcess, unbind the main targets, since ISRefraction may still have kMAIN_COPY bound.
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
	// Failed compiles are cached as null and failed keys are not retried, so clear both. A new count is a new key.
	impl->programs.raster.clear();
	impl->programs.compute.clear();
	impl->failedKey.reset();
	++impl->programsGeneration;
	impl->declared.Invalidate();
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph the chain always runs on D3D11.
struct ORGPostProcessing::Impl
{};

ORGPostProcessing::~ORGPostProcessing() = default;
void ORGPostProcessing::Reserve() {}
void ORGPostProcessing::Setup() {}
void ORGPostProcessing::BeforeTonemap(const RenderGraph::Engine::TonemapTargets&, bool) {}
bool ORGPostProcessing::Ran() const { return false; }
void ORGPostProcessing::ClearShaderCache() {}

#endif
