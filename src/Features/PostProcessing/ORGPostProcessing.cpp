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

		/** @brief Pixels whose centre lies inside a D3D11 viewport a_size wide. */
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
		state.registered.emplace_back(a_resource.Name(), a_resource.Graph());
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
		latch.Write(run, a_offset, std::span<const std::byte>(static_cast<const std::byte*>(a_data), a_size));
	}

	void CommitContext::Open(GateEntry a_entry, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const { gate.OpenDispatch(a_entry.index, a_x, a_y, a_z); }

	namespace
	{
		// Enough lease indices for every run in flight. The occlusion query ring makes the same number at setup.
		constexpr std::uint32_t kReadbackLeases = 256;
		// A texture copy's offset must be 512-aligned on D3D12. Each part has its stamp, then its data, both at that alignment.
		constexpr std::uint32_t kReadbackAlign = 512;

		constexpr std::uint32_t AlignReadback(std::uint32_t a_bytes) { return (a_bytes + kReadbackAlign - 1) / kReadbackAlign * kReadbackAlign; }

		/** @brief The latch word a committed run copies with its parts. 0 is never a stamp. */
		struct ReadbackStamp
		{
			std::uint32_t value = 0;
			std::uint32_t pad[3]{};
		};
	}

	struct RunReadback::State final : RenderGraph::RunSlotsRegistry::Entry
	{
		struct Run
		{
			org::RunRef ref;
			std::uint32_t stamp = 0;
			bool committed = false, submitted = false;
		};

		std::string name;
		std::shared_ptr<org::Buffer> buffer;
		std::vector<std::uint32_t> sizes;  // by part
		std::vector<std::uint32_t> bases;  // by part, the stamp's offset in a region. The data follows at kReadbackAlign.
		std::uint32_t stride = 0;          // one lease's region

		// The current build's stamp. Render thread.
		std::shared_ptr<const org::LatchBlock> latch;
		LatchRegion<ReadbackStamp> stamp;

		// Render thread. MoveRun runs there too, inside a stage's submission.
		mutable std::mutex mutex;
		mutable std::vector<Run> runs;  // by lease index
		mutable std::uint32_t lastCommitted = ~0u;
		mutable std::vector<std::vector<std::byte>> latest;  // by part, not taken yet
		mutable std::vector<std::uint32_t> latestStamps;      // by part
		std::uint32_t sequence = 0;

		State() { RenderGraph::RunSlotsRegistry::Add(this); }
		~State() { RenderGraph::RunSlotsRegistry::Remove(this); }
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		/** @brief Fatal: a lease past the regions would copy outside the buffer. */
		void CheckLease(std::uint32_t a_lease) const
		{
			if (a_lease >= kReadbackLeases)
				stl::report_and_fail(std::format("Community Shaders: the Post Processing readback {} got run lease {}, past its {} regions.", name, a_lease,
					kReadbackLeases));
		}

		/** @brief Under the mutex. The lease's last run is done, so its region holds what that run copied. */
		void Harvest(std::uint32_t a_lease) const
		{
			if (a_lease >= runs.size())
				return;
			const auto run = std::exchange(runs[a_lease], Run{});
			// A run that was committed but not submitted copied nothing of its own.
			if (!run.committed || !run.submitted)
				return;
			void* mapped = nullptr;
			buffer->GetAPIResource().Map(&mapped);
			if (!mapped)
				stl::report_and_fail(std::format("Community Shaders: the Post Processing readback {} could not be mapped.", name));
			const auto* region = static_cast<const std::byte*>(mapped) + std::uint64_t(a_lease) * stride;
			for (std::size_t part = 0; part < sizes.size(); ++part) {
				std::uint32_t written = 0;
				std::memcpy(&written, region + bases[part], sizeof(written));
				// Another stamp means this part's pass was not in the run, or a later uncommitted run wrote over it.
				if (written != run.stamp)
					continue;
				const auto* data = region + bases[part] + kReadbackAlign;
				latest[part].assign(data, data + sizes[part]);
				latestStamps[part] = run.stamp;
			}
			buffer->GetAPIResource().Unmap(0, 0);
		}

		/** @brief A stage's commit wrote its staging run. The stage now submits it as a_to, whose last run is done. */
		void MoveRun(const org::RunRef& a_from, const org::RunRef& a_to) const override
		{
			if (a_from.lease == a_to.lease)
				return;
			const std::scoped_lock lock(mutex);
			if (a_from.lease >= runs.size() || !runs[a_from.lease].committed || !(runs[a_from.lease].ref == a_from))
				return;
			CheckLease(a_to.lease);
			if (runs.size() <= a_to.lease)
				runs.resize(static_cast<std::size_t>(a_to.lease) + 1);
			Harvest(a_to.lease);
			auto moved = std::exchange(runs[a_from.lease], Run{});
			moved.ref = a_to;
			runs[a_to.lease] = moved;
			if (lastCommitted == a_from.lease)
				lastCommitted = a_to.lease;
		}
	};

	namespace
	{
		struct RunReadbackSpec
		{
			std::shared_ptr<const RunReadback::State> state;
			std::shared_ptr<const org::LatchBlock> latch;
			std::uint32_t stamp = 0;  // the stamp's offset in the latch
			std::uint32_t part = 0;
			std::shared_ptr<org::Resource> source;
			bool texture = false;
			std::uint32_t width = 0, height = 0;
		};

		struct RunReadbackBindings
		{
			org::ResourceBindingToken source;
		};

		struct RunReadbackFrame
		{
			std::shared_ptr<const RunReadbackSpec> spec;
		};

		/** @brief Copies the run's stamp and one part into the run's region. Its commands depend only on the run, so it is replayable. */
		class RunReadbackPass final : public org::TypedRenderGraphPass<RunReadbackPass, RunReadbackFrame, RunReadbackBindings>
		{
		public:
			explicit RunReadbackPass(std::shared_ptr<const RunReadbackSpec> a_spec) :
				spec(std::move(a_spec)) {}

			RunReadbackBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				RunReadbackBindings bindings{};
				bindings.source = a_builder.CopySource(spec->source);
				a_builder.CopyDestination(std::static_pointer_cast<org::Resource>(spec->state->buffer));
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			RunReadbackFrame Prepare(const RunReadbackBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static void Record(const RunReadbackBindings& a_bindings, const RunReadbackFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& spec = *a_frame.spec;
				const auto& state = *spec.state;
				const auto& run = a_recording.Run();
				if (run.lease >= kReadbackLeases)
					throw std::runtime_error(std::format("the Post Processing readback {} got run lease {}, past its {} regions", state.name, run.lease,
						kReadbackLeases));
				auto& commands = a_recording.Commands();
				const auto destination = state.buffer->GetAPIResource().GetHandle();
				const std::uint64_t base = std::uint64_t(run.lease) * state.stride + state.bases[spec.part];
				const auto latch = spec.latch->At(run);
				commands.CopyBufferRegion(destination, base, latch.buffer->GetAPIResource().GetHandle(), latch.offset + spec.stamp, sizeof(std::uint32_t));
				const auto source = a_recording.Resolve(a_bindings.source).GetHandle();
				if (!spec.texture) {
					commands.CopyBufferRegion(destination, base + kReadbackAlign, source, 0, state.sizes[spec.part]);
					return;
				}
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = source;
				copy.buffer = destination;
				copy.mip = 0;
				copy.arraySlice = 0;
				copy.footprint = { base + kReadbackAlign, 256, spec.width, spec.height, 1 };
				commands.CopyTextureToBuffer(copy);
			}

		private:
			std::shared_ptr<const RunReadbackSpec> spec;
		};
	}

	std::shared_ptr<RunReadback> RunReadback::Create(std::string_view a_name, std::vector<std::uint32_t> a_parts)
	{
		auto state = std::make_shared<State>();
		state->name = std::string(a_name);
		for (const auto bytes : a_parts) {
			state->bases.push_back(state->stride);
			state->sizes.push_back(bytes);
			state->stride += kReadbackAlign + AlignReadback(bytes);
		}
		state->latest.resize(a_parts.size());
		state->latestStamps.resize(a_parts.size());
		state->buffer = org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(state->stride) * kReadbackLeases);
		if (!state->buffer) {
			logger::error("[ORG] Post Processing: the readback buffer {} could not be made", state->name);
			return nullptr;
		}
		state->buffer->SetName(state->name);
		return std::make_shared<RunReadback>(std::move(state));
	}

	void RunReadback::Build(Builder& a_builder)
	{
		state->latch = a_builder.LatchBlock();
		state->stamp = a_builder.Latch<ReadbackStamp>();
	}

	void RunReadback::AddBuffer(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source)
	{
		if (a_part >= state->sizes.size() || !a_source || !state->latch) {
			a_builder.Fail(std::format("{}: the readback part, its source or its Build is missing", a_name));
			return;
		}
		auto spec = std::make_shared<const RunReadbackSpec>(RunReadbackSpec{ .state = state, .latch = state->latch, .stamp = state->stamp.offset, .part = a_part,
			.source = std::move(a_source) });
		a_builder.Add(a_name, FrameAnchors::PassKind::Copy, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<RunReadbackPass>(spec); });
	}

	void RunReadback::AddTexture(Builder& a_builder, std::string_view a_name, std::uint32_t a_part, std::shared_ptr<org::Resource> a_source,
		std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_part >= state->sizes.size() || !a_source || !state->latch) {
			a_builder.Fail(std::format("{}: the readback part, its source or its Build is missing", a_name));
			return;
		}
		if (!a_width || !a_height || a_width * 16 > 256 || std::uint64_t(a_height) * 256 > state->sizes[a_part]) {
			a_builder.Fail(std::format("{}: a {}x{} texture does not fit its readback part", a_name, a_width, a_height));
			return;
		}
		auto spec = std::make_shared<const RunReadbackSpec>(RunReadbackSpec{ .state = state, .latch = state->latch, .stamp = state->stamp.offset, .part = a_part,
			.source = std::move(a_source), .texture = true, .width = a_width, .height = a_height });
		a_builder.Add(a_name, FrameAnchors::PassKind::Copy, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<RunReadbackPass>(spec); });
	}

	std::uint32_t RunReadback::Commit(const CommitContext& a_commit)
	{
		const auto& run = a_commit.Run();
		std::uint32_t stamp = 0;
		{
			const std::scoped_lock lock(state->mutex);
			state->CheckLease(run.lease);
			if (state->runs.size() <= run.lease)
				state->runs.resize(static_cast<std::size_t>(run.lease) + 1);
			// A leased run's last run on this index retired before the lease reopened. A stage's staging run holds nothing.
			state->Harvest(run.lease);
			stamp = ++state->sequence;
			if (stamp == 0)
				stamp = ++state->sequence;
			state->runs[run.lease] = { .ref = run, .stamp = stamp, .committed = true };
			state->lastCommitted = run.lease;
		}
		a_commit.Write(state->stamp, ReadbackStamp{ .value = stamp });
		return stamp;
	}

	void RunReadback::Executed()
	{
		const std::scoped_lock lock(state->mutex);
		if (state->lastCommitted < state->runs.size())
			state->runs[state->lastCommitted].submitted = true;
	}

	std::vector<std::byte> RunReadback::Take(std::uint32_t a_part, std::uint32_t* a_stamp)
	{
		const std::scoped_lock lock(state->mutex);
		if (a_part >= state->latest.size())
			return {};
		if (a_stamp)
			*a_stamp = state->latestStamps[a_part];
		return std::exchange(state->latest[a_part], {});
	}
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

	// A key that failed to build is not tried again.
	std::shared_ptr<ORGPostChain::BuildState> build;
	std::vector<std::uint64_t> key;
	std::vector<std::uint64_t> failedKey;
	ORGPostChain::LatchRegion<CopyLatch> input, output;

	// Set by BeforeTonemap for the commit.
	bool ready = false;
	bool ran = false;
	bool readsMainCopy = false;
	bool linear = false;
	bool colorGraded = false;
	bool exposureResolved = true;
	std::vector<Index> order;
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
			logger::info("[ORG] Post Processing: the chain runs on the render graph ({} passes)", build ? build->descs.size() : 0);
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

	/** @brief False, with the reason in a_failure, when it cannot. */
	bool Rebuild(const ORGPostChain::Shape& a_shape, const ORGPostChain::Image& a_main, const ORGPostChain::Image& a_mainCopy,
		const std::vector<Index>& a_order, std::vector<std::uint64_t> a_key, std::string& a_failure)
	{
		// CS's native chain has one image, read and written back.
		const auto& second = a_mainCopy ? a_mainCopy : a_main;
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
		ORGPostChain::LatchRegion<CopyLatch> nextInput, nextOutput;

		// As BeginLinearProcessing. The input pass also picks kMAIN or kMAIN_COPY from the latch, so one recording serves either.
		{
			ORGPostChain::Builder builder(*next, programs, a_shape, "input");
			auto program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { ORGPostChain::kInputFormat });
			auto image = builder.CreateImage("PostProcessing::Input", ORGPostChain::kInputFormat, D3D11_BIND_RENDER_TARGET);
			nextInput = builder.Latch<CopyLatch>();
			builder.Draw({ .name = "copy",
				.program = std::move(program),
				.bindings = { Bind::Srv(a_main), Bind::Srv(second), Bind::Latch(nextInput) },
				.target = image });
			builder.SetCurrent(image);
		}

		for (const auto index : a_order) {
			if (!next->failure.empty() || next->passes->Failed())
				break;
			ORGPostChain::Builder builder(*next, programs, a_shape, StepName(index));
			effects[static_cast<std::size_t>(index)]->Build(builder, *pp.pipeline[static_cast<std::size_t>(index)]);
		}

		// As PreProcess's two CopyToRenderTarget, into both main targets. Each is a copy.ps draw rather than a copy,
		// because a copy has no run gate and would run on frames the chain does not.
		{
			ORGPostChain::Builder builder(*next, programs, a_shape, "output");
			const auto chainOutput = builder.Current();
			nextOutput = builder.Latch<CopyLatch>();
			for (const auto& [target, name] : { std::pair{ &a_main, "main" }, std::pair{ &a_mainCopy, "main-copy" } }) {
				if (!*target)
					continue;
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

	void Commit()
	{
		ran = false;
		if (!ready || !build)
			return;
		auto& pp = globals::features::postProcessing;
		const auto& gate = *build->gate;
		const ORGPostChain::CommitContext commit(*build->latch, RenderGraphRuntime::Get().Host()->CurrentRun(), gate);
		gate.OpenDraw(ORGPostChain::kDrawEntry);
		for (const auto& [groups, entry] : build->fixed)
			gate.OpenDispatch(entry, groups[0], groups[1], groups[2]);
		commit.Write(input, CopyLatch{ .conversion = { .gamma = linear ? 1.0f : Util::ColorSpace::GAME_GAMMA }, .source = readsMainCopy ? 1u : 0u });
		for (const auto index : order)
			effects[static_cast<std::size_t>(index)]->Commit(commit, *pp.pipeline[static_cast<std::size_t>(index)]);
		commit.Write(output, CopyLatch{ .conversion = pp.OutputConversion(colorGraded, exposureResolved) });
		ran = true;
	}

	/** @brief After a run whose commit reached the GPU with this build. */
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

	// The graph runs the chain only when it can run every effect in it.
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
			 std::uint64_t(shape.mainFormat), std::uint64_t(shape.mainCopyFormat), std::uint64_t(linear), reinterpret_cast<std::uintptr_t>(mainTarget.Key()), reinterpret_cast<std::uintptr_t>(mainCopyTarget.Key()) })
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

	// The Tonemap anchor has no epoch of its own. The chain's work must land in the post stage's Tonemap step.
	// That step shares the anchor with Effects11 and does not hold it wholly, so HeldByStage is false for it and cannot be the test.
	// The frame's schedules, taken at Renderer::Begin, say whether a stage of this frame draws this registration.
	// A frame kind without the post stage, such as the main menu's, has none. Nor does the post stage after a rebuild,
	// until the build with the new registration switches in. Those are D3D11 frames for the chain, so D3D11 draws it.
	if (!FrameAnchors::ScheduledThisFrame(ORGPostChain::kClientId, FrameAnchors::Anchor::Tonemap)) {
		s.SetReason("this frame's schedule has no post stage that holds the chain's build (a frame kind without one, or a build not switched in yet)");
		return;
	}
	// The frame's schedule holds the chain, so the open stage must take the tap now. If it does not, that is a schedule bug.
	// It is logged, and D3D11 draws the chain this frame, since no stage would take the graph's work.
	if (!FrameAnchors::Scheduled(ORGPostChain::kClientId, FrameAnchors::Anchor::Tonemap)) {
		static Util::ReasonLog s_lag("[ORG] Post Processing: the post stage does not take the Tonemap tap, though the frame's schedule holds the chain",
			spdlog::level::err);
		s_lag.Report("not scheduled at the tap");
		s.SetReason("the post stage does not take the Tonemap tap this frame (logged above)");
		return;
	}

	s.order = std::move(order);
	s.linear = linear;
	// CS's ImageSpace steps keep the world scene in kMAIN, so the engine's kMAIN_COPY input or a refraction does not move it.
	// kMAIN_COPY then holds an older copy of the scene at best, from before the volumetric composite and the upscale.
	const bool sceneInMain = RenderGraph::Engine::ImageSpace::SceneInMain();
	s.readsMainCopy = !single && !sceneInMain && (a_refraction || a_targets.input == mainCopyTarget);
	s.colorGraded = colorGraded;
	s.exposureResolved = exposureResolved;
	s.ready = true;
	s.SetReason({});
	s.SetMembers(s.order);

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
	// Failed compiles are cached as null and failed keys are not retried, so clear both.
	impl->programs.raster.clear();
	impl->programs.compute.clear();
	impl->failedKey.clear();
	impl->key.clear();
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
