#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/RunReadback.h"

#	include "RenderGraph/FrameStaging.h"

#	include <Render/LatchBlock.h>
#	include <Render/RunRef.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>

#	include <cstring>
#	include <format>
#	include <mutex>
#	include <stdexcept>
#	include <utility>

namespace
{
	// Enough lease indices for every run in flight. The occlusion query ring makes the same number at setup.
	constexpr std::uint32_t kRunReadbackLeases = 256;
	// A texture copy's offset must be 512-aligned on D3D12. Each part has its stamp, then its data, both at that alignment.
	constexpr std::uint32_t kRunReadbackAlign = 512;

	constexpr std::uint32_t AlignRunReadback(std::uint32_t a_bytes) { return (a_bytes + kRunReadbackAlign - 1) / kRunReadbackAlign * kRunReadbackAlign; }
}

namespace RenderGraph
{
	static_assert(sizeof(RunReadback::Stamp) == 16);

	struct RunReadback::State final : RunSlotsRegistry::Entry
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
		std::vector<std::uint32_t> bases;  // by part, the stamp's offset in a region. The data follows at kRunReadbackAlign.
		std::uint32_t stride = 0;          // one lease's region

		// The current build's latch and stamp offset. Render thread.
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t stampOffset = 0;

		// Render thread. MoveRun runs there too, inside a stage's submission.
		mutable std::mutex mutex;
		mutable std::vector<Run> runs;  // by lease index
		mutable std::uint32_t lastCommitted = ~0u;
		mutable std::vector<std::vector<std::byte>> latest;  // by part, not taken yet
		mutable std::vector<std::uint32_t> latestStamps;     // by part
		std::uint32_t sequence = 0;

		State() { RunSlotsRegistry::Add(this); }
		~State() { RunSlotsRegistry::Remove(this); }
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		/** @brief Fatal: a lease past the regions would copy outside the buffer. */
		void CheckLease(std::uint32_t a_lease) const
		{
			if (a_lease >= kRunReadbackLeases)
				stl::report_and_fail(std::format("Community Shaders: the readback {} got run lease {}, past its {} regions.", name, a_lease, kRunReadbackLeases));
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
				stl::report_and_fail(std::format("Community Shaders: the readback {} could not be mapped.", name));
			const auto* region = static_cast<const std::byte*>(mapped) + std::uint64_t(a_lease) * stride;
			for (std::size_t part = 0; part < sizes.size(); ++part) {
				std::uint32_t written = 0;
				std::memcpy(&written, region + bases[part], sizeof(written));
				// Another stamp means this part's pass was not in the run, or a later uncommitted run wrote over it.
				if (written != run.stamp)
					continue;
				const auto* data = region + bases[part] + kRunReadbackAlign;
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
}

namespace
{
	struct RunReadbackSpec
	{
		std::shared_ptr<const RenderGraph::RunReadback::State> state;
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
			if (run.lease >= kRunReadbackLeases)
				throw std::runtime_error(std::format("the readback {} got run lease {}, past its {} regions", state.name, run.lease, kRunReadbackLeases));
			auto& commands = a_recording.Commands();
			const auto destination = state.buffer->GetAPIResource().GetHandle();
			const std::uint64_t base = std::uint64_t(run.lease) * state.stride + state.bases[spec.part];
			const auto latch = spec.latch->At(run);
			commands.CopyBufferRegion(destination, base, latch.buffer->GetAPIResource().GetHandle(), latch.offset + spec.stamp, sizeof(std::uint32_t));
			const auto source = a_recording.Resolve(a_bindings.source).GetHandle();
			if (!spec.texture) {
				commands.CopyBufferRegion(destination, base + kRunReadbackAlign, source, 0, state.sizes[spec.part]);
				return;
			}
			rhi::BufferTextureCopyFootprint copy{};
			copy.texture = source;
			copy.buffer = destination;
			copy.mip = 0;
			copy.arraySlice = 0;
			copy.footprint = { base + kRunReadbackAlign, 256, spec.width, spec.height, 1 };
			commands.CopyTextureToBuffer(copy);
		}

	private:
		std::shared_ptr<const RunReadbackSpec> spec;
	};
}

namespace RenderGraph
{
	std::shared_ptr<RunReadback> RunReadback::Create(std::string_view a_name, std::vector<std::uint32_t> a_parts)
	{
		auto made = std::make_shared<State>();
		made->name = std::string(a_name);
		for (const auto bytes : a_parts) {
			made->bases.push_back(made->stride);
			made->sizes.push_back(bytes);
			made->stride += kRunReadbackAlign + AlignRunReadback(bytes);
		}
		made->latest.resize(a_parts.size());
		made->latestStamps.resize(a_parts.size());
		made->buffer = org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(made->stride) * kRunReadbackLeases);
		if (!made->buffer) {
			logger::error("[ORG] The readback buffer {} could not be made", made->name);
			return nullptr;
		}
		made->buffer->SetName(made->name);
		return std::make_shared<RunReadback>(std::move(made));
	}

	void RunReadback::Bind(std::shared_ptr<const org::LatchBlock> a_latch, std::uint32_t a_stampOffset)
	{
		state->latch = std::move(a_latch);
		state->stampOffset = a_stampOffset;
	}

	std::function<std::shared_ptr<org::RenderPass>()> RunReadback::BufferPass(std::uint32_t a_part, std::shared_ptr<org::Resource> a_source, std::string& a_why) const
	{
		if (a_part >= state->sizes.size() || !a_source || !state->latch) {
			a_why = std::format("the readback {}: the part, its source or its Bind is missing", state->name);
			return {};
		}
		auto spec = std::make_shared<const RunReadbackSpec>(RunReadbackSpec{ .state = state, .latch = state->latch, .stamp = state->stampOffset, .part = a_part, .source = std::move(a_source) });
		return [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<RunReadbackPass>(spec); };
	}

	std::function<std::shared_ptr<org::RenderPass>()> RunReadback::TexturePass(std::uint32_t a_part, std::shared_ptr<org::Resource> a_source,
		std::uint32_t a_width, std::uint32_t a_height, std::string& a_why) const
	{
		if (a_part >= state->sizes.size() || !a_source || !state->latch) {
			a_why = std::format("the readback {}: the part, its source or its Bind is missing", state->name);
			return {};
		}
		if (!a_width || !a_height || a_width * 16 > 256 || std::uint64_t(a_height) * 256 > state->sizes[a_part]) {
			a_why = std::format("the readback {}: a {}x{} texture does not fit its part", state->name, a_width, a_height);
			return {};
		}
		auto spec = std::make_shared<const RunReadbackSpec>(RunReadbackSpec{ .state = state, .latch = state->latch, .stamp = state->stampOffset, .part = a_part, .source = std::move(a_source), .texture = true, .width = a_width, .height = a_height });
		return [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<RunReadbackPass>(spec); };
	}

	std::uint32_t RunReadback::Commit(const org::RunRef& a_run)
	{
		const std::scoped_lock lock(state->mutex);
		state->CheckLease(a_run.lease);
		if (state->runs.size() <= a_run.lease)
			state->runs.resize(static_cast<std::size_t>(a_run.lease) + 1);
		// A leased run's last run on this index retired before the lease reopened. A stage's staging run holds nothing.
		state->Harvest(a_run.lease);
		std::uint32_t stamp = ++state->sequence;
		if (stamp == 0)
			stamp = ++state->sequence;
		state->runs[a_run.lease] = { .ref = a_run, .stamp = stamp, .committed = true };
		state->lastCommitted = a_run.lease;
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

#endif
