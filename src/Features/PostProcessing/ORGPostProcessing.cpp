#include "ORGPostProcessing.h"
#include "Snapshot/EngineReads.h"
#include "Utils/Logging.h"

#include "RenderGraph/Switches.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGPostEffect.h"

#include "Features/LinearLighting.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Targets/EngineTargetSet.h"
#include "RenderGraph/Engine/TonemapAnchor.h"
#include "Globals.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"
#include "Utils/ColorSpace.h"
#include "Utils/D3D.h"
#include "Utils/Format.h"
#include "Utils/Game.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <variant>

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

	/** @brief The programs one chain plan lists, by key. Built on the programs lane, read by the build. */
	struct ChainPrograms
	{
		std::unordered_map<std::string, std::shared_ptr<const RasterProgram>> raster;
		std::unordered_map<std::string, std::shared_ptr<const ComputeProgram>> compute;
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

	Builder::Builder(BuildState& a_state, const ChainPrograms& a_programs, const Shape& a_shape, bool a_linear, std::string_view a_effect) :
		ProgramSource(a_shape, a_linear), state(a_state), programs(a_programs), effect(a_effect) {}

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

	bool Builder::Published(std::string_view a_name) const
	{
		return state.published.find(a_name) != state.published.end() || state.publishedLatches.find(a_name) != state.publishedLatches.end();
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

	std::shared_ptr<const RasterProgram> ProgramSource::Raster(std::string_view a_source, std::uint32_t a_words, std::vector<DXGI_FORMAT> a_targets,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		std::vector<RasterProgram::Target> targets;
		for (const auto format : a_targets)
			targets.push_back({ .format = rhi::helpers::ToRHI(format) });
		return RasterTargets(a_source, a_words, std::move(targets), std::move(a_defines), std::move(a_entry));
	}

	std::shared_ptr<const RasterProgram> ProgramSource::RasterTargets(std::string_view a_source, std::uint32_t a_words, std::vector<RasterProgram::Target> a_targets,
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
		return TakeRaster(std::move(key), std::move(desc));
	}

	std::shared_ptr<const ComputeProgram> ProgramSource::Compute(std::string_view a_source, std::uint32_t a_words,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		auto key = ProgramKey(a_source, a_entry, a_words, a_defines);
		ComputeProgram::Desc desc{ .source = std::string(a_source), .entry = std::move(a_entry), .constantWords = a_words, .defines = RenderGraph::AnchorPasses::Defines(a_defines) };
		return TakeCompute(std::move(key), std::move(desc));
	}

	std::shared_ptr<const RasterProgram> Builder::TakeRaster(std::string a_key, RasterProgram::Desc a_desc)
	{
		if (const auto found = programs.raster.find(a_key); found != programs.raster.end() && found->second)
			return found->second;
		Fail(std::format("{} ({}) is not among the programs its Programs lists", a_desc.pixelSource, a_key));
		return nullptr;
	}

	std::shared_ptr<const ComputeProgram> Builder::TakeCompute(std::string a_key, ComputeProgram::Desc a_desc)
	{
		if (const auto found = programs.compute.find(a_key); found != programs.compute.end() && found->second)
			return found->second;
		Fail(std::format("{} ({}) is not among the programs its Programs lists", a_desc.source, a_key));
		return nullptr;
	}

	RasterProgram::Viewport Builder::FullViewport() const { return { .width = shape.width, .height = shape.height }; }

	RasterProgram::Viewport Builder::DynamicViewport() const
	{
		// After the upscale the dynamic part is the whole of kMAIN (step 8). A recording never follows the ratio.
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
		if (a_dispatch.gate) {
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

	std::shared_ptr<RunReadback> RunReadback::Create(std::string_view a_name, std::vector<std::uint32_t> a_parts)
	{
		return std::make_shared<RunReadback>(org::RunReadback::Create(a_name, std::move(a_parts), RenderGraph::FatalHandler(ORGPostProcessing::kName)));
	}

	LatchRegion<org::RunReadback::Stamp> RunReadback::BuildStamp(Builder& a_builder)
	{
		const auto region = a_builder.Latch<org::RunReadback::Stamp>();
		readback->Bind(a_builder.LatchBlock(), region.offset);
		return region;
	}

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

	std::uint32_t RunReadback::Commit(const CommitContext& a_commit, const LatchRegion<org::RunReadback::Stamp>& a_stamp)
	{
		const auto value = readback->Commit(a_commit.Run());
		a_commit.Write(a_stamp, org::RunReadback::Stamp{ .value = value });
		return value;
	}

	void RunReadback::Executed() { readback->Executed(); }

	std::vector<std::byte> RunReadback::Take(std::uint32_t a_part, std::uint32_t* a_stamp) { return readback->Take(a_part, a_stamp); }

}

namespace
{
	using ORGPostChain::Bind;
	using Index = PostProcessing::FeaturePipelineIndex;

	constexpr std::size_t kSlots = static_cast<std::size_t>(Index::COUNT);

	/** @brief Every effect is ported, so null is a missing case here, which Setup makes fatal. */
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
	/** @brief What BeforeTonemap finds that is no variable yet: the chain's order, its mode and the effects' Update words. */
	struct ChainSeen
	{
		std::vector<Index> order;
		bool single = false;               // the chain reads and writes kMAIN only, with no kMAIN_COPY
		bool linear = false;               // linear lighting: the scene is stored pre-exposed
		std::vector<std::uint64_t> words;  // the effects' slots and Update words, then single and linear

		friend bool operator==(const ChainSeen& a_left, const ChainSeen& a_right) { return a_left.words == a_right.words; }
	};

	/** @brief The chain's key: what it observed, the main targets' shape, and per effect in its order what Key read from the revision. */
	struct ChainKey
	{
		ChainSeen observed;
		ORGPostChain::Shape shape;
		// The engine's slots are stable for the session, so they follow from observed.single and are not compared.
		FrameAnchors::Texture main, mainCopy;  // mainCopy is empty when the chain reads and writes one target
		std::vector<std::vector<std::uint64_t>> effects;
		std::uint64_t generation = 0;  // ClearShaderCache's count, so a reload builds again
		// The programs the plan of this key lists, built on the programs lane. Follows from the rest, so it is not compared.
		std::shared_ptr<const ORGPostChain::ChainPrograms> programs;

		friend bool operator==(const ChainKey& a_left, const ChainKey& a_right)
		{
			return a_left.observed == a_right.observed && a_left.shape.width == a_right.shape.width && a_left.shape.height == a_right.shape.height &&
			       a_left.shape.mainFormat == a_right.shape.mainFormat && a_left.shape.mainCopyFormat == a_right.shape.mainCopyFormat &&
			       a_left.effects == a_right.effects && a_left.generation == a_right.generation;
		}
	};

	/** @brief One program of a chain plan: its key and its desc, with the defines resolved on the render thread. */
	struct PlannedProgram
	{
		std::string key;
		std::variant<RasterProgram::Desc, ComputeProgram::Desc> desc;
	};

	/** @brief The chain's declared program: every program one chain key's build asks for. Equal keys are one plan. */
	struct ProgramPlanKey
	{
		std::uint64_t generation = 0;
		std::vector<std::string> keys;  // sorted
		std::shared_ptr<const std::vector<PlannedProgram>> programs;

		friend bool operator==(const ProgramPlanKey& a_left, const ProgramPlanKey& a_right)
		{
			return a_left.generation == a_right.generation && a_left.keys == a_right.keys;
		}
	};

	/** @brief Every program compiled this session, by generation and key, so a plan rebuilds only what is new. Any thread. */
	struct ProgramStore
	{
		std::mutex mutex;
		std::unordered_map<std::string, std::shared_ptr<const RasterProgram>> raster;
		std::unordered_map<std::string, std::shared_ptr<const ComputeProgram>> compute;
	};

	/** @brief Render thread. Resolved descs by key: the common defines read State, so they are resolved here, never on the lane. */
	using ResolvedPrograms = std::unordered_map<std::string, std::variant<RasterProgram::Desc, ComputeProgram::Desc>>;

	/** @brief Effect::Programs' source: it lists each program once and records what effects publish. Render thread. */
	class ProgramPlan final : public ORGPostChain::ProgramSource
	{
	public:
		ProgramPlan(const ORGPostChain::Shape& a_shape, bool a_linear, ResolvedPrograms& a_resolved) :
			ProgramSource(a_shape, a_linear), resolved(a_resolved) {}

		bool Published(std::string_view a_name) const override { return published.find(a_name) != published.end(); }
		void Publishes(std::string_view a_name) override { published.emplace(a_name); }

		std::vector<PlannedProgram> programs;

	protected:
		std::shared_ptr<const RasterProgram> TakeRaster(std::string a_key, RasterProgram::Desc a_desc) override
		{
			Add(std::move(a_key), [&] {
				RenderGraph::Programs::ResolveDefines(a_desc);
				return std::variant<RasterProgram::Desc, ComputeProgram::Desc>(std::move(a_desc));
			});
			return nullptr;
		}

		std::shared_ptr<const ComputeProgram> TakeCompute(std::string a_key, ComputeProgram::Desc a_desc) override
		{
			Add(std::move(a_key), [&] {
				RenderGraph::Programs::ResolveDefines(a_desc);
				return std::variant<RasterProgram::Desc, ComputeProgram::Desc>(std::move(a_desc));
			});
			return nullptr;
		}

	private:
		template <class Resolve>
		void Add(std::string a_key, Resolve&& a_resolve)
		{
			if (!seen.insert(a_key).second)
				return;
			auto found = resolved.find(a_key);
			if (found == resolved.end())
				found = resolved.emplace(a_key, a_resolve()).first;
			programs.push_back({ std::move(a_key), found->second });
		}

		ResolvedPrograms& resolved;
		std::set<std::string, std::less<>> seen;
		std::set<std::string, std::less<>> published;
	};

	/** @brief One build of the chain. Its registration holds it, so a frame whose schedule holds it commits it. */
	struct ChainBuild
	{
		std::shared_ptr<ORGPostChain::BuildState> state;
		std::vector<Index> order;
		std::vector<std::shared_ptr<ORGPostChain::Run>> runs;  // one per order entry
		ORGPostChain::LatchRegion<CopyLatch> input, output;
		std::uint64_t generation = 0;
	};
}

struct ORGPostProcessing::Impl
{
	rhi::Device device;
	std::array<std::unique_ptr<ORGPostChain::Effect>, kSlots> effects;

	// What BeforeTonemap saw, and ClearShaderCache's count. Both keys read them, so they are made before the declarations.
	RenderGraph::Frame::Observed<ChainSeen> observed{ "PostProcessing.Chain" };
	RenderGraph::Frame::Observed<std::uint64_t> shaderGeneration{ "PostProcessing.ShaderGeneration" };

	// The chain's client and its programs, declared on the effects' settings and on what is observed.
	RenderGraph::Frame::DeclaredClientOf<ChainBuild> declared;
	RenderGraph::Frame::DeclaredProgram<ORGPostChain::ChainPrograms> chainPrograms;
	std::shared_ptr<ProgramStore> store = std::make_shared<ProgramStore>();
	ResolvedPrograms resolved;
	std::uint64_t buildGeneration = 0;  // numbers the builds in the logs
	std::uint64_t programsGeneration = 0;

	// Set by BeforeTonemap for the commit.
	bool ready = false;
	bool ran = false;
	bool readsMainCopy = false;
	bool linear = false;
	bool colorGraded = false;
	bool exposureResolved = true;
	// The build this frame's schedule holds. Every registration's commit runs it, since the client taps by id.
	std::shared_ptr<ChainBuild> readyBuild;
	float2 dynamicSize{};
	// A tonemap after the post stage was seen and logged.
	bool passedLogged = false;

	// Empty means on the graph. Logged when it changes.
	std::optional<std::string> reason;
	// The effects the graph ran last frame. Logged when one joins or leaves.
	std::vector<Index> members;

	/** @brief The chain runs on the graph this frame. Logged when it starts again. */
	void SetRunning()
	{
		if (reason && reason->empty())
			return;
		logger::info("[ORG] Post Processing: the chain runs on the render graph ({} passes)", readyBuild ? readyBuild->state->descs.size() : 0);
		reason = std::string();
	}

	/** @brief For a frame where no chain runs. D3D11 never runs it in graph mode. Logged when the reason changes. */
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

	/** @brief Render thread, at setup. Declares the effects' textures, the chain's programs and its client on their settings. */
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
		dependencies.push_back(RenderGraph::Frame::DependencyOf(observed.Var()));
		dependencies.push_back(RenderGraph::Frame::DependencyOf(shaderGeneration.Var()));
		dependencies.push_back(RenderGraph::Frame::DependencyOf(RenderGraph::Frame::Vars::DisplaySize()));
		// The first plan is empty (nothing is observed yet), so the declaration builds nothing. Later plans build on the lane.
		chainPrograms = RenderGraph::Frame::DeclareProgram(
			"PostProcessing::Chain", dependencies, [this](const RenderGraph::Frame::Inputs& a_values) { return PlanFor(a_values); },
			[store = store, device = device](const ProgramPlanKey& a_plan) { return BuildPlan(*store, device, a_plan); });
		declared = RenderGraph::Frame::DeclareClient<ChainKey, ChainBuild>(
			ORGPostChain::kClientId, std::move(dependencies), [this](const RenderGraph::Frame::Inputs& a_values) { return Prepare(a_values); },
			[this](const ChainKey& a_key, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<ChainBuild> { return Make(a_key, a_client); });
	}

	/** @brief The chain's key for a_values, without its programs. Nullopt before BeforeTonemap observed the chain. Render thread. */
	std::optional<ChainKey> KeyFor(const RenderGraph::Frame::Inputs& a_values)
	{
		const auto* seen = observed.Get(a_values);
		if (!seen)
			return std::nullopt;
		namespace Targets = RenderGraph::Engine::Targets;
		const auto mainIndex = static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN);
		const auto mainCopyIndex = static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN_COPY);
		auto& pp = globals::features::postProcessing;
		ChainKey key{ .observed = *seen, .generation = GenerationOf(a_values) };
		// Both main targets follow the display size. Their formats are fixed for the session.
		const auto extent = a_values.Get(RenderGraph::Frame::Vars::DisplaySize());
		key.shape.width = extent.width;
		key.shape.height = extent.height;
		key.shape.mainFormat = Targets::Desc(Targets::Colour(mainIndex)).Format;
		key.shape.mainCopyFormat = seen->single ? key.shape.mainFormat : Targets::Desc(Targets::Colour(mainCopyIndex)).Format;
		key.main = FrameAnchors::EngineTexture(FrameAnchors::Colour(mainIndex));
		if (!seen->single)
			key.mainCopy = FrameAnchors::EngineTexture(FrameAnchors::Colour(mainCopyIndex));
		for (const auto index : seen->order) {
			const auto slot = static_cast<std::size_t>(index);
			auto& words = key.effects.emplace_back();
			effects[slot]->Key(a_values, *pp.pipeline[slot], words);
		}
		return key;
	}

	/** @brief ClearShaderCache's count as a_values carry it. */
	std::uint64_t GenerationOf(const RenderGraph::Frame::Inputs& a_values) const
	{
		const auto* generation = shaderGeneration.Get(a_values);
		return generation ? *generation : 0;
	}

	/** @brief The declared program's key: every program the build of a_values' chain key asks for, in build order. Render thread. */
	ProgramPlanKey PlanFor(const RenderGraph::Frame::Inputs& a_values)
	{
		ProgramPlanKey plan{ .generation = GenerationOf(a_values) };
		const auto key = KeyFor(a_values);
		if (!key)
			return plan;
		auto& pp = globals::features::postProcessing;
		const auto& shape = key->shape;
		ProgramPlan source(shape, key->observed.linear, resolved);
		// As Rebuild: the input copy, the effects in order, then the output copies.
		source.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { ORGPostChain::kInputFormat });
		for (std::size_t i = 0; i < key->observed.order.size(); ++i) {
			const auto slot = static_cast<std::size_t>(key->observed.order[i]);
			effects[slot]->Programs(source, *pp.pipeline[slot], key->effects[i]);
		}
		source.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { shape.mainFormat });
		if (key->mainCopy)
			source.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { shape.mainCopyFormat });
		for (const auto& program : source.programs)
			plan.keys.push_back(program.key);
		std::ranges::sort(plan.keys);
		plan.programs = std::make_shared<const std::vector<PlannedProgram>>(std::move(source.programs));
		return plan;
	}

	/** @brief The declared program's build, on the programs lane. Null, logged, when a program does not build: that is fatal. */
	static std::shared_ptr<const ORGPostChain::ChainPrograms> BuildPlan(ProgramStore& a_store, rhi::Device a_device, const ProgramPlanKey& a_plan)
	{
		auto built = std::make_shared<ORGPostChain::ChainPrograms>();
		if (!a_plan.programs)
			return built;
		std::uint32_t compiled = 0;
		for (const auto& planned : *a_plan.programs) {
			const auto stored = std::format("{}|{}", a_plan.generation, planned.key);
			if (const auto* raster = std::get_if<RasterProgram::Desc>(&planned.desc)) {
				std::shared_ptr<const RasterProgram> program;
				{
					const std::lock_guard lock(a_store.mutex);
					if (const auto found = a_store.raster.find(stored); found != a_store.raster.end())
						program = found->second;
				}
				if (!program) {
					program = RasterProgram::Load(a_device, *raster);
					if (!program) {
						logger::critical("[ORG] Post Processing: {} ({}) did not build on the programs lane", raster->pixelSource, planned.key);
						return nullptr;
					}
					++compiled;
					const std::lock_guard lock(a_store.mutex);
					a_store.raster.emplace(stored, program);
				}
				built->raster.emplace(planned.key, std::move(program));
			} else {
				const auto& compute = std::get<ComputeProgram::Desc>(planned.desc);
				std::shared_ptr<const ComputeProgram> program;
				{
					const std::lock_guard lock(a_store.mutex);
					if (const auto found = a_store.compute.find(stored); found != a_store.compute.end())
						program = found->second;
				}
				if (!program) {
					program = ComputeProgram::Load(a_device, compute);
					if (!program) {
						logger::critical("[ORG] Post Processing: {} ({}) did not build on the programs lane", compute.source, planned.key);
						return nullptr;
					}
					++compiled;
					const std::lock_guard lock(a_store.mutex);
					a_store.compute.emplace(stored, program);
				}
				built->compute.emplace(planned.key, std::move(program));
			}
		}
		logger::info("[ORG] Post Processing: the chain's {} programs are built on the programs lane ({} new)", a_plan.programs->size(), compiled);
		return built;
	}

	/**
	* @brief The client's key function, when a revision starts. Reads values and programs only: the revision started once the
	* programs of its values were built, so the plan's programs are there.
	*/
	std::optional<ChainKey> Prepare(const RenderGraph::Frame::Inputs& a_values)
	{
		auto key = KeyFor(a_values);
		if (!key)
			return std::nullopt;
		key->programs = chainPrograms.For(a_values);
		return key;
	}

	/** @brief The make function: builds the chain for a_key. Null is fatal (Frame/Declare.h), so the reason is logged first. */
	std::shared_ptr<ChainBuild> Make(const ChainKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		std::string failure;
		std::shared_ptr<ChainBuild> build;
		try {
			build = Rebuild(a_key, a_client, failure);
		} catch (const std::exception& e) {
			failure = e.what();
		}
		if (!build) {
			logger::critical("[ORG] Post Processing: the chain could not be built: {}", failure);
			return nullptr;
		}
		return build;
	}

	/** @brief The build and its registration in a_client, or null with the reason in a_failure. */
	std::shared_ptr<ChainBuild> Rebuild(const ChainKey& a_key, FrameAnchors::ClientDesc& a_client, std::string& a_failure)
	{
		const auto& shape = a_key.shape;
		const ORGPostChain::Image main{ a_key.main, shape.mainFormat };
		const ORGPostChain::Image mainCopy{ a_key.mainCopy, shape.mainCopyFormat };
		const auto& order = a_key.observed.order;
		const bool linearKey = a_key.observed.linear;
		// CS's native chain has one image, read and written back.
		const auto& second = mainCopy ? mainCopy : main;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host) {
			a_failure = "the render graph has no host";
			return nullptr;
		}
		if (!a_key.programs) {
			a_failure = "its programs are not built";
			return nullptr;
		}
		const auto& programSet = *a_key.programs;
		auto next = std::make_shared<ORGPostChain::BuildState>();
		next->latch = std::make_shared<org::LatchBlock>("cs.pp.latch", ORGPostChain::kLatchBytes);
		next->gate = RenderGraph::RunGate::Create("cs.pp.gate", ORGPostChain::kGateEntries);
		if (!next->gate) {
			a_failure = "the chain's run gate could not be made";
			return nullptr;
		}
		next->passes = std::make_unique<RenderGraph::AnchorPasses::Builder>(FrameAnchors::Anchor::Tonemap, ORGPostChain::kClientId, ORGPostChain::kOrderBase,
			next->latch, next->gate);
		auto& pp = globals::features::postProcessing;
		auto build = std::make_shared<ChainBuild>();
		build->state = next;
		build->order = order;

		// As BeginLinearProcessing. The input pass also picks kMAIN or kMAIN_COPY from the latch, so one recording serves either.
		{
			ORGPostChain::Builder builder(*next, programSet, shape, linearKey, "input");
			auto program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { ORGPostChain::kInputFormat });
			auto image = builder.CreateImage("PostProcessing::Input", ORGPostChain::kInputFormat, D3D11_BIND_RENDER_TARGET);
			build->input = builder.Latch<CopyLatch>();
			builder.Draw({ .name = "copy",
				.program = std::move(program),
				.bindings = { Bind::Srv(main), Bind::Srv(second), Bind::Latch(build->input) },
				.target = image });
			builder.SetCurrent(image);
		}
		for (std::size_t i = 0; i < order.size(); ++i) {
			if (!next->failure.empty() || next->passes->Failed())
				break;
			const auto index = order[i];
			const auto slot = static_cast<std::size_t>(index);
			ORGPostChain::Builder builder(*next, programSet, shape, linearKey, StepName(index));
			auto run = effects[slot]->BuildRun(builder, *pp.pipeline[slot], a_key.effects[i]);
			if (!run && !builder.Failed())
				builder.Fail("it made no run");
			build->runs.push_back(std::move(run));
		}

		// As PreProcess's two CopyToRenderTarget, into both main targets. Each is a copy.ps draw rather than a copy,
		// because a copy has no run gate and would run on frames the chain does not.
		{
			ORGPostChain::Builder builder(*next, programSet, shape, linearKey, "output");
			const auto chainOutput = builder.Current();
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
			return nullptr;
		}
		next->descs = next->passes->Take();
		// The post stage is recorded ahead and its recording is submitted again, so every pass must give a revision.
		// A pass without one makes the stage's recording fatal at its first frame, so name it here.
		for (const auto& desc : next->descs) {
			const auto pass = desc.make ? desc.make() : nullptr;
			std::uint64_t revision = 0;
			if (!pass || !pass->RevisionHash(revision)) {
				a_failure = std::format("{} gives no revision, so a kept recording cannot hold it", desc.name);
				return nullptr;
			}
		}

		build->generation = ++buildGeneration;
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
		// The newest registration's callbacks run for the one the schedule holds (it taps by id), so they use the held build.
		client.commit = [this](const FrameAnchors::Point&) {
			if (readyBuild)
				Commit(*readyBuild);
		};
		client.executed = [this](const FrameAnchors::Point&) {
			if (readyBuild)
				Executed(*readyBuild);
		};
		client.active = [this] { return ready; };
		std::string steps = "input";
		for (const auto index : order)
			steps += std::format(", {}", StepName(index));
		logger::info("[ORG] Post Processing: chain {} built for the next revision ({}, output; {} passes, {} programs, {}x{})", build->generation, steps,
			next->descs.size(), programSet.raster.size() + programSet.compute.size(), shape.width, shape.height);
		a_client = std::move(client);
		return build;
	}

	void Commit(const ChainBuild& a_build)
	{
		ran = false;
		if (!ready)
			return;
		auto& pp = globals::features::postProcessing;
		const auto& gate = *a_build.state->gate;
		const ORGPostChain::CommitContext commit(*a_build.state->latch, RenderGraphRuntime::Get().Host()->CurrentRun(), gate, dynamicSize);
		gate.OpenDraw(ORGPostChain::kDrawEntry);
		for (const auto& [groups, entry] : a_build.state->fixed)
			gate.OpenDispatch(entry, groups[0], groups[1], groups[2]);
		commit.Write(a_build.input, CopyLatch{ .conversion = { .gamma = linear ? 1.0f : Util::ColorSpace::GAME_GAMMA }, .source = readsMainCopy ? 1u : 0u });
		for (std::size_t i = 0; i < a_build.order.size(); ++i) {
			ZoneScopedN("CS.PP.StepCommit");
			[[maybe_unused]] const char* name = StepName(a_build.order[i]);
			ZoneText(name, std::strlen(name));
			a_build.runs[i]->Commit(commit, *pp.pipeline[static_cast<std::size_t>(a_build.order[i])]);
		}
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

	/**
	* @brief The build this frame's schedule holds, or null with the reason in a_why, and the chain draws nothing. A build Declare
	* does not know is fatal there.
	*/
	std::shared_ptr<ChainBuild> HeldBuild(std::string& a_why)
	{
		const auto held = declared.Held(FrameAnchors::Anchor::Tonemap);
		const auto& place = held.Placement();
		if (place.draws)
			return held.Shared();
		if (place.reason == FrameAnchors::Reason::Unregistered) {
			// Its key needs one observed tonemap. Every later frame has a build.
			a_why = "the chain has no build before its first observed tonemap";
			return nullptr;
		}
		if (!place.InStage()) {
			// A frame kind without a post stage, such as the main menu's, has no place for the chain.
			a_why = fmt::format("this frame has no post stage that holds the chain ({})", FrameAnchors::ReasonName(place.reason));
			return nullptr;
		}
		if (!place.MayLag())
			RenderGraph::Fatal(ORGPostProcessing::kName, fmt::format("the post stage will never hold the chain ({})", FrameAnchors::ReasonName(place.reason)));
		a_why = fmt::format("the post stage has no schedule that holds the chain yet ({})", FrameAnchors::ReasonName(place.reason));
		return nullptr;
	}
};

ORGPostProcessing::ORGPostProcessing() = default;
ORGPostProcessing::~ORGPostProcessing() = default;

void ORGPostProcessing::Setup()
{
	auto* host = BeginSetup();
	if (!host)
		return;
	auto state = std::make_unique<Impl>();
	state->device = host->Device();
	std::string ported;
	auto& pp = globals::features::postProcessing;
	for (std::size_t i = 0; i < kSlots; ++i) {
		state->effects[i] = MakeEffect(static_cast<Index>(i));
		if (!state->effects[i])
			Fatal(fmt::format("effect slot {} has no ORG implementation", i));
		if (pp.pipeline[i])
			ported += std::format("{}{}", ported.empty() ? "" : ", ", pp.pipeline[i]->GetType());
	}
	state->Declare();
	impl = std::move(state);
	logger::info("[ORG] Post Processing: the tonemap chain runs on the render graph ({}); it follows the frame revisions", ported);
}

void ORGPostProcessing::BeforeTonemap(const RenderGraph::Engine::TonemapTargets& a_targets, bool a_refraction)
{
	if (!impl)
		return;
	auto& s = *impl;
	s.ready = false;
	s.readyBuild.reset();
	s.ran = false;
	// A second tonemap in the frame can come after the post stage has ended. The loading menu's 3D model draws one in every frame
	// of DCLF's load hold. No stage takes it, so no chain runs for it, as in a frame kind without a post stage.
	// It is not the world's tonemap, so it leaves the logged state alone. Otherwise the chain would seem to leave and rejoin the graph each frame.
	if (FrameAnchors::Place(ORGPostChain::kClientId, FrameAnchors::Anchor::Tonemap).reason == FrameAnchors::Reason::Passed) {
		if (!s.passedLogged) {
			s.passedLogged = true;
			logger::info("[ORG] Post Processing: a tonemap after this frame's post stage, such as a menu scene's, runs no chain (logged once)");
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

	// Every effect's choices are in the key. An effect that cannot read its inputs is fatal: D3D11 never runs the chain here.
	ChainSeen seen{ .order = order };
	for (const auto index : order) {
		const auto slot = static_cast<std::size_t>(index);
		auto& feature = *pp.pipeline[slot];
		// D3D11 never draws the chain here. Only an effect whose key imports what SetupResources makes is set up.
		if (s.effects[slot]->ReadsD3D11Resources())
			pp.EnsureResources(feature);
		seen.words.push_back(slot);
		if (const char* why = s.effects[slot]->Update(feature, seen.words))
			Fatal(std::format("{}: {}", feature.GetType(), why));
	}

	// Native: kMAIN is read and written back. Guest: either main target may hold the scene, and both get the result.
	// The engine's slots are stable for the session, so the key reads them itself.
	const bool single = a_targets.chain;
	const auto mainIndex = static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN);
	const auto mainCopyTarget = single ? FrameAnchors::Texture{} : FrameAnchors::EngineTexture(FrameAnchors::Colour(RE::RENDER_TARGETS::kMAIN_COPY));
	if (!FrameAnchors::EngineTexture(FrameAnchors::Colour(mainIndex)) || (!single && !mainCopyTarget))
		Fatal("kMAIN or kMAIN_COPY has no slot");
	seen.single = single;
	seen.linear = linear;
	seen.words.push_back(std::uint64_t(single));
	seen.words.push_back(std::uint64_t(linear));
	// The dynamic-resolution ratio is a per-frame value: it reaches the commit, never the key.
	s.dynamicSize = RenderGraph::Frame::Vars::DynamicArea();

	// A change is a new key, built when the next revision starts. This frame runs the build its schedule holds.
	s.observed.Observe(seen);

	std::string why;
	const auto build = s.HeldBuild(why);
	if (!build) {
		s.SetSkipped(why);
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
	s.readyBuild = build;
	s.ready = true;
	s.SetRunning();
	s.SetMembers(build->order);

	// As PreProcess, unbind the main targets, since ISRefraction may still have kMAIN_COPY bound.
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	// A write: the engine re-binds its targets (goes with the engine's render half).
	CS_ENGINE_READ(Shaders, Other, globals::game::stateUpdateFlags)->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

bool ORGPostProcessing::Ran() const
{
	return impl && impl->ran;
}

void ORGPostProcessing::ClearShaderCache()
{
	if (!impl)
		return;
	// A new count is a new plan, so the programs lane compiles every program again, and a new chain key.
	{
		const std::lock_guard lock(impl->store->mutex);
		impl->store->raster.clear();
		impl->store->compute.clear();
	}
	impl->resolved.clear();
	impl->shaderGeneration.Observe(++impl->programsGeneration);
}

