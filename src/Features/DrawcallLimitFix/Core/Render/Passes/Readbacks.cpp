// The GPU-to-host copies: the fade and visibility event rings.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	struct EventReadbackBindings
	{
		org::ResourceBindingToken events;
	};
	struct EventReadbackPrepared
	{
		std::shared_ptr<const org::RunReadback> list;  // holds the set
		const org::RunReadback::ListSet* set = nullptr;
	};

	/**
	 * @brief Copies an event list whole into its readback's region for the run. A fixed copy, so the recording depends on the run
	 * alone. The depth commit reads it when the region's run commits again.
	 */
	class EventReadbackPass final : public org::TypedRenderGraphPass<EventReadbackPass, EventReadbackPrepared, EventReadbackBindings>
	{
	public:
		EventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<org::RunReadback> a_list) :
			scene(std::move(a_scene)), events(std::move(a_events)), list(std::move(a_list)) {}

		EventReadbackBindings Declare(org::PassBuilder& a_builder) { return { a_builder.CopySource(*events) }; }

		void Revision(std::vector<std::uint64_t>&) const {}

		EventReadbackPrepared Prepare(const EventReadbackBindings&, const org::PassPrepareContext& a_preparation) const
		{
			return { list, list->SetFor(static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, events))) };
		}

		static void Record(const EventReadbackBindings& a_bindings, const EventReadbackPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.list || !a_prepared.set)
				return;
			a_prepared.list->RecordList(a_recording.Commands(), a_recording.Run(), a_recording.Resolve(a_bindings.events).GetHandle(), *a_prepared.set);
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
		Versioned events;
		std::shared_ptr<org::RunReadback> list;
	};

	std::shared_ptr<org::RenderPass> MakeEventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<org::RunReadback> a_list)
	{
		return std::make_shared<EventReadbackPass>(std::move(a_scene), std::move(a_events), std::move(a_list));
	}

	namespace
	{
		// Weak, so only the graph's pass keeps each readback and its host buffers alive.
		std::array<std::atomic<std::weak_ptr<CounterReadback>>, static_cast<std::size_t>(CullCounterView::kCount)> counterReadbacks;
	}

	std::shared_ptr<CounterReadback> CullCounterReadback(CullCounterView a_view)
	{
		return counterReadbacks[static_cast<std::size_t>(a_view)].load(std::memory_order_acquire).lock();
	}

	struct CounterReadbackBindings
	{
		std::vector<org::ResourceBindingToken> counts;  // by source
	};
	struct CounterReadbackPrepared
	{
		std::shared_ptr<CounterReadback> readback;
		CounterSource source;
	};

	/**
	 * @brief Copies each drawn source's count words and its latch's stamp into the run's readback region. A fixed copy, so
	 * the recording depends on the shape and run alone.
	 */
	class CounterReadbackPass final : public org::TypedRenderGraphPass<CounterReadbackPass, CounterReadbackPrepared, CounterReadbackBindings>
	{
	public:
		CounterReadbackPass(CullCounterView a_view, std::vector<std::shared_ptr<org::Buffer>> a_counts, std::function<CounterSource(const org::PassPrepareContext&)> a_source) :
			view(a_view), counts(std::move(a_counts)), source(std::move(a_source))
		{
			readback = std::make_shared<CounterReadback>(static_cast<std::uint32_t>(counts.size()));
		}

		CounterReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			CounterReadbackBindings bindings{};
			for (const auto& count : counts)
				bindings.counts.push_back(a_builder.CopySource(count));
			counterReadbacks[static_cast<std::size_t>(view)].store(readback, std::memory_order_release);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		CounterReadbackPrepared Prepare(const CounterReadbackBindings&, const org::PassPrepareContext& a_preparation) const
		{
			CounterReadbackPrepared prepared{ readback, source(a_preparation) };
			std::erase_if(prepared.source.copies, [&](const CounterSource::Copy& a_copy) { return a_copy.source >= counts.size(); });
			return prepared;
		}

		static void Record(const CounterReadbackBindings& a_bindings, const CounterReadbackPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			const auto& readback = a_prepared.readback;
			const auto& latch = a_prepared.source.latch;
			const auto run = a_recording.Run();
			const std::uint32_t range = readback ? readback->RangeOf(run) : org::RunReadback::kNoRegion;
			if (!readback || !latch || range == org::RunReadback::kNoRegion)
				return;
			auto& commands = a_recording.Commands();
			const auto at = latch->At(run);
			const auto latchBuffer = at.buffer->GetAPIResource().GetHandle();
			const std::uint64_t region = at.offset;
			constexpr std::uint64_t kWordBytes = sizeof(std::uint32_t);
			for (const auto& copy : a_prepared.source.copies) {
				const std::uint64_t hostAt = std::uint64_t(copy.source) * CounterReadback::kSourceWords * kWordBytes;
				readback->readback->RecordCopy(commands, range, hostAt, a_recording.Resolve(a_bindings.counts[copy.source]).GetHandle(), 0, kCountWords * kWordBytes);
				readback->readback->RecordCopy(commands, range, hostAt + kCountWords * kWordBytes, latchBuffer, region + copy.latchOffset + offsetof(BuildDrawsLatch, visibilityStamp), kWordBytes);
			}
		}

	private:
		CullCounterView view;
		std::vector<std::shared_ptr<org::Buffer>> counts;
		std::function<CounterSource(const org::PassPrepareContext&)> source;
		std::shared_ptr<CounterReadback> readback;
	};

	std::shared_ptr<org::RenderPass> MakeCounterReadbackPass(CullCounterView a_view, std::vector<std::shared_ptr<org::Buffer>> a_counts,
		std::function<CounterSource(const org::PassPrepareContext&)> a_source)
	{
		return std::make_shared<CounterReadbackPass>(a_view, std::move(a_counts), std::move(a_source));
	}

	std::shared_ptr<org::RenderPass> MakeShadowCullCounterPass(std::shared_ptr<ShadowResources> a_resources)
	{
		// Every view slot's count buffer, with each drawn view's latch at its slot within the region.
		return MakeCounterReadbackPass(CullCounterView::kShadow, a_resources->count, [](const org::PassPrepareContext& a_preparation) {
			CounterSource out;
			const auto* shapes = RevisionShapesOf(a_preparation);
			const auto frame = shapes ? shapes->shadow : nullptr;
			if (!frame)
				return out;
			out.latch = frame->latch;
			for (const auto& view : frame->views)
				out.copies.push_back({ view.slot, view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)) });
			return out;
		});
	}
}
