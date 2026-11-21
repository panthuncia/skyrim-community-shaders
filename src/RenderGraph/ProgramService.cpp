// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/ProgramService.h"

#include "RenderGraph/Async/Lanes.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Utils/Format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <OpenRenderGraph/GraphHost.h>

#if defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include "RenderGraph/ProgramBuild.h"
#endif

namespace RenderGraph::Programs
{
	namespace
	{
		/**
		 * @brief The programs pool: a few lanes that take jobs from one queue, so several builds run at once.
		 *
		 * A build waits for its compiles, which DCLF's build pump runs on the preparation pool. So the pool takes at most half the
		 * preparation threads and always leaves one for the compiles. With a single preparation thread it runs one build at a time
		 * on a blocking thread instead.
		 */
		class Pool
		{
		public:
			Pool()
			{
				const unsigned preparation = Frame::PreparationWorkers();
				const bool shared = preparation >= 2;
				const unsigned width = shared ? preparation / 2 : 1u;
				const auto taskClass = shared ? Frame::Executor::Preparation : Frame::Executor::Streaming;
				active.assign(width, 0);
				for (unsigned i = 0; i < width; ++i)
					lanes.push_back(std::make_unique<Frame::Lane>(Frame::LaneDesc{ fmt::format("CS programs {}", i), taskClass, org::async::TaskDispatch::Cpu },
						[this, i] { Drain(i); }));
				logger::info("[ORG] programs: {} builds run at once, on the {} threads", width, shared ? "preparation" : "blocking");
			}

			/** @brief Any thread. Queues a_job and wakes an idle lane, if any. False once stopped. */
			bool Post(std::function<void()> a_job)
			{
				std::size_t lane = 0;
				{
					const std::lock_guard lock(mutex);
					if (stopping)
						return false;
					queue.push_back(std::move(a_job));
					// Every active lane drains the queue before it goes idle, so with none idle the job is taken anyway.
					while (lane < active.size() && active[lane])
						++lane;
					if (lane == active.size())
						return true;
					active[lane] = 1;
				}
				return lanes[lane]->Wake();
			}

			/** @brief Teardown only. Drops the queued jobs and waits for the running ones. */
			void Stop()
			{
				{
					const std::lock_guard lock(mutex);
					stopping = true;
					queue.clear();
				}
				for (auto& lane : lanes)
					lane->Stop();
			}

		private:
			void Drain(std::size_t a_lane)
			{
				for (;;) {
					std::function<void()> job;
					{
						const std::lock_guard lock(mutex);
						if (stopping || queue.empty()) {
							active[a_lane] = 0;
							return;
						}
						job = std::move(queue.front());
						queue.pop_front();
					}
					job();
				}
			}

			std::mutex mutex;
			std::deque<std::function<void()>> queue;
			std::vector<char> active;  // a lane was woken and has not found the queue empty since
			bool stopping = false;
			std::vector<std::unique_ptr<Frame::Lane>> lanes;
		};

		std::atomic<Pool*> s_madePool{ nullptr };

		Pool& Builds()
		{
			// Never destroyed: Stop stops it before the device goes.
			static auto* s_pool = [] {
				auto* made = new Pool();
				s_madePool.store(made, std::memory_order_release);
				return made;
			}();
			return *s_pool;
		}

		/** @brief Render thread only. The slots whose builds no Collect has seen done. */
		std::vector<std::shared_ptr<Detail::Slot>>& InFlight()
		{
			static std::vector<std::shared_ptr<Detail::Slot>> s_slots;
			return s_slots;
		}

		std::string DefinesLabel(const std::vector<std::pair<std::wstring, std::wstring>>& a_defines)
		{
			std::string text;
			for (const auto& [define, value] : a_defines)
				text += fmt::format("{}{}{}{}", text.empty() ? "" : " ", Util::WStringToString(define), value.empty() ? "" : "=", Util::WStringToString(value));
			return text;
		}

		/** @brief On the pool. Writes the result, then done, and wakes the setup join. */
		void RunSlot(Detail::Slot& a_slot)
		{
			try {
				a_slot.program = a_slot.build();
				if (!a_slot.program)
					a_slot.error = "its build returned nothing (the [ORG] error above has the compiler's messages)";
			} catch (const std::exception& e) {
				a_slot.error = e.what();
			}
			a_slot.build = {};
			a_slot.done.store(true, std::memory_order_release);
			a_slot.done.notify_all();
		}

		template <class Desc>
		std::shared_ptr<Detail::Slot> QueueDesc(std::string_view a_owner, Desc a_desc)
		{
			if (!Available())
				return nullptr;
			ResolveDefines(a_desc);
			auto label = fmt::format("{}: {}", a_owner, Label(a_desc));
			return Detail::Queue(std::move(label), [desc = std::move(a_desc)]() -> std::shared_ptr<const void> { return Load(desc); }, true);
		}
	}

	void ResolveDefines(RasterProgram::Desc& a_desc)
	{
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
		if (!a_desc.commonDefines)
			return;
		if (a_desc.vertexStageDefines.empty())
			a_desc.vertexStageDefines = ProgramBuild::WithCommonDefines(a_desc.defines, L"vs_6_6");
		if (a_desc.pixelStageDefines.empty())
			a_desc.pixelStageDefines = ProgramBuild::WithCommonDefines(a_desc.defines, L"ps_6_6");
#else
		(void)a_desc;
#endif
	}

	void ResolveDefines(ComputeProgram::Desc& a_desc)
	{
#if defined(ORG_MODULE_SERVICES_HAS_DXC)
		if (!a_desc.commonDefines)
			return;
		a_desc.defines = ProgramBuild::WithCommonDefines(a_desc.defines, L"cs_6_6");
		a_desc.commonDefines = false;
#else
		(void)a_desc;
#endif
	}

	std::string Label(const RasterProgram::Desc& a_desc)
	{
		return fmt::format("{} + {} ({}) [{}]", a_desc.vertexSource, a_desc.pixelSource, Util::WStringToString(a_desc.pixelEntry), DefinesLabel(a_desc.defines));
	}

	std::string Label(const ComputeProgram::Desc& a_desc)
	{
		return fmt::format("{} ({}) [{}]", a_desc.source, Util::WStringToString(a_desc.entry), DefinesLabel(a_desc.defines));
	}

	std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& a_desc)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		return host ? RasterProgram::Load(host->Device(), a_desc) : nullptr;
	}

	std::shared_ptr<const ComputeProgram> Load(const ComputeProgram::Desc& a_desc)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		return host ? ComputeProgram::Load(host->Device(), a_desc) : nullptr;
	}

	bool Post(std::function<void()> a_job)
	{
		return Builds().Post(std::move(a_job));
	}

	bool Available()
	{
		return RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::Get().Host();
	}

	void Stop()
	{
		// A pool never made has nothing to stop.
		if (auto* pool = s_madePool.load(std::memory_order_acquire))
			pool->Stop();
	}

	std::shared_ptr<Detail::Slot> Detail::Queue(std::string a_label, std::function<std::shared_ptr<const void>()> a_build, bool a_gates)
	{
		auto slot = std::make_shared<Slot>();
		slot->label = std::move(a_label);
		slot->gates = a_gates;
		slot->build = std::move(a_build);
		InFlight().push_back(slot);
		if (!Post([slot] { RunSlot(*slot); }))
			RenderGraph::Fatal("Programs", fmt::format("the programs pool refused the build of {}", slot->label));
		return slot;
	}

	void Detail::NotBuilt(const Slot& a_slot)
	{
		RenderGraph::Fatal("Programs", fmt::format("{} was read before its build was done; it is queued at setup and read only after CS's setup or a revision start",
								  a_slot.label));
	}

	Fixed<RasterProgram> Queue(std::string_view a_owner, RasterProgram::Desc a_desc)
	{
		return Fixed<RasterProgram>(QueueDesc(a_owner, std::move(a_desc)));
	}

	Fixed<ComputeProgram> Queue(std::string_view a_owner, ComputeProgram::Desc a_desc)
	{
		return Fixed<ComputeProgram>(QueueDesc(a_owner, std::move(a_desc)));
	}

	bool Collect()
	{
		auto& slots = InFlight();
		std::erase_if(slots, [](const std::shared_ptr<Detail::Slot>& a_slot) {
			if (!a_slot->done.load(std::memory_order_acquire))
				return false;
			if (!a_slot->error.empty())
				RenderGraph::Fatal("Programs", fmt::format("{} could not be built: {}", a_slot->label, a_slot->error));
			a_slot->collected = true;
			return true;
		});
		return std::ranges::none_of(slots, [](const std::shared_ptr<Detail::Slot>& a_slot) { return a_slot->gates; });
	}

	bool Pending()
	{
		return !InFlight().empty();
	}

	void JoinSetup(std::string_view a_point)
	{
		const auto count = InFlight().size();
		if (!count)
			return;
		const auto start = std::chrono::steady_clock::now();
		for (const auto& slot : InFlight())
			slot->done.wait(false, std::memory_order_acquire);
		(void)Collect();
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		logger::info("[ORG] programs: the setup batch of {} programs is built at {}; {:.0f} ms waited", count, a_point, ms);
	}
}

