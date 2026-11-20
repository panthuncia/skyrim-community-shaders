// Assembles a scene revision on the snapshot builder: the sealed versions, each epoch's shape, and the recordings requested from
// ORG's host thread. Also the render thread's lookup and submission of the selected revision.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"

#include <ORGModuleServices/ComposedVersions.h>

namespace DCLF
{
	using org::async::RevisionFragment;

	namespace
	{
		// Render thread. Counts submissions outside an epoch's trigger, so only the first is logged.
		std::array<std::uint64_t, kMaxEpochs> outsideTrigger{};

		/** @brief Returns a_old when it has the same shape as a_made, so its fragment and recording stay valid. */
		template <class Frame>
		std::shared_ptr<const Frame> Kept(const std::shared_ptr<const Frame>& a_old, const std::shared_ptr<const Frame>& a_made, bool& a_changed)
		{
			if (a_old && a_made && a_old->SameShape(*a_made))
				return a_old;
			a_changed = true;
			// Copy, because live recordings are prepared against the identity of the live shape.
			return std::make_shared<const Frame>(*a_made);
		}

		/** @brief One epoch's pending recordings. The fragment resolves once all of them are recorded on ORG's host thread. */
		struct PendingRecordings
		{
			std::shared_ptr<RevisionFragment> fragment;
			std::vector<std::shared_ptr<const org::GraphHost::EpochRecording>> recordings;
			std::atomic<std::uint32_t> remaining = 0;
			std::atomic<bool> failed = false;
			std::uint32_t hosted = 0;
			// The build it is recorded on (RevisionInputs::epochBuilds). A recording made on any other does not belong to this revision.
			std::uint64_t buildGeneration = 0;
			std::atomic<std::uint64_t>*recorded = nullptr, *failedCount = nullptr, *droppedCount = nullptr, *shapes = nullptr;
			std::atomic<std::uint32_t>* logged = nullptr;
			std::atomic<std::uint32_t>* outstanding = nullptr;
			std::string_view name;
		};

		using Revisions = ImplProducer::SceneRevisions;
		using Draft = org::async::RevisionAssembler::Draft;

		/** @brief Put each epoch's shape in a_draft, keeping the old fragment while the shape is the same. */
		void AssembleShapes(Revisions& a_rv, Draft& a_draft, const MadeShapes& a_shapes, const ToggleSet& a_toggles)
		{
			using R = Revisions;
			auto& draft = a_draft;
			auto setShape = [&]<class Value>(std::uint32_t a_epoch, std::shared_ptr<const Value> a_value) {
				draft.Set(R::kShapeSlot + a_epoch, RevisionFragment::MakeReady(std::move(a_value)));
				++a_rv.epochs[a_epoch].changed;
			};
			const auto& shapes = a_shapes;
			const auto& toggles = a_toggles;
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
				const auto& traits = EpochTraitsOf(e);
				const auto& current = draft.Get(R::kShapeSlot + e);
				bool changed = false;
				if (traits.sharesWith != kNoEpoch) {
					// Share the same pointer so this epoch's recording stays valid. No shape while the gate class is off.
					const auto& source = draft.Get(R::kShapeSlot + traits.sharesWith);
					const bool on = source && (traits.gate == Api::ObjectClass::Count || Extensions::ObjectClassOn(toggles, traits.gate));
					auto share = [&]<class Frame>() {
						const auto shared = on ? source->Value<Frame>() : nullptr;
						if (shared == (current ? current->Value<Frame>() : nullptr))
							return;
						if (shared)
							setShape(e, shared);
						else
							draft.Set(R::kShapeSlot + e, nullptr);
					};
					if (traits.shape == ShapeKind::Main)
						share.template operator()<PassFrame>();
					else
						share.template operator()<ViewFrame>();
					continue;
				}
				if (traits.shape == ShapeKind::Main) {
					if (const auto& made = shapes.main[traits.index]) {
						auto kept = Kept<PassFrame>(current ? current->Value<PassFrame>() : nullptr, made, changed);
						if (changed)
							setShape(e, std::move(kept));
					}
				} else if (traits.shape == ShapeKind::Shadow) {
					const auto& made = shapes.shadow;
					if (made.empty())
						continue;
					const auto old = current ? current->Value<ShadowVariants>() : nullptr;
					auto variants = std::make_shared<ShadowVariants>();
					changed = !old || old->shapes.size() != made.size();
					for (std::size_t v = 0; v < made.size(); ++v)
						variants->shapes.push_back(Kept<ShadowFrame>(old && v < old->shapes.size() ? old->shapes[v] : nullptr, made[v], changed));
					if (changed)
						setShape(e, std::shared_ptr<const ShadowVariants>(std::move(variants)));
				} else {
					// A DropWhenOff view loses its shape while toggled off. A KeepLast view keeps its last shape when no new one is made.
					const auto view = static_cast<ViewKind>(traits.index);
					const bool dropWhenOff = Extensions::Views().views[traits.index].life == Api::ViewShapeLife::DropWhenOff;
					const auto& made = dropWhenOff && !Extensions::ViewOn(toggles, view) ? nullptr : shapes.views[traits.index];
					const auto currentShape = current ? current->Value<ViewFrame>() : nullptr;
					if (made) {
						auto kept = Kept<ViewFrame>(currentShape, made, changed);
						if (changed)
							setShape(e, std::move(kept));
					} else if (currentShape && dropWhenOff) {
						draft.Set(R::kShapeSlot + e, nullptr);
					}
				}
			}
		}

		/** @brief ORG's host thread. One recording finished. The fragment resolves after the last one, or fails. */
		void RecordingCompleted(const std::shared_ptr<PendingRecordings>& a_work, std::size_t a_index, std::shared_ptr<const org::GraphHost::EpochRecording> a_recording,
			std::exception_ptr a_error)
		{
			const auto& work = a_work;
			// A drop or a recording on another graph build is not an error. The fragment still fails so nothing waits on it.
			// The next make asks again for the new build.
			bool dropped = false;
			if (a_error) {
				try {
					std::rethrow_exception(a_error);
				} catch (const org::GraphHost::EpochRecordingDropped&) {
					dropped = true;
				} catch (...) {
				}
			} else if (a_recording && a_recording->buildGeneration != work->buildGeneration) {
				dropped = true;
			}
			if (dropped || a_error || !a_recording) {
				if (!work->failed.exchange(true, std::memory_order_acq_rel)) {
					if (dropped) {
						++*work->droppedCount;
					} else {
						++*work->failedCount;
						if ((*work->logged).fetch_add(1, std::memory_order_relaxed) < 8) {
							std::string what = "no recording";
							try {
								if (a_error)
									std::rethrow_exception(a_error);
							} catch (const std::exception& error) {
								what = error.what();
							} catch (...) {
								what = "unknown";
							}
							logger::warn("[DCLF] scene revision: the {} epoch's recording failed: {}", work->name, what);
						}
					}
					(void)work->fragment->Fail(a_error ? a_error : std::make_exception_ptr(std::runtime_error(dropped ? "recorded on another graph build" : "no recording")));
				}
			} else {
				work->recordings[a_index] = std::move(a_recording);
			}
			if (work->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 && !work->failed.load(std::memory_order_acquire)) {
				++*work->recorded;
				*work->shapes += work->recordings.size();
				auto recordings = std::make_shared<RevisionRecordings>();
				recordings->recordings = std::move(work->recordings);
				recordings->hosted = work->hosted;
				(void)work->fragment->Resolve(std::shared_ptr<const RevisionRecordings>(std::move(recordings)));
			}
			work->outstanding->fetch_sub(1, std::memory_order_acq_rel);
		}

		/**
		 * @brief Request an epoch's recordings from ORG's host thread. a_inputs is the versions, the shape, then each hosted shape.
		 * Returns the fragment they resolve, or null when async epochs were stopped.
		 */
		std::shared_ptr<RevisionFragment> RequestRecordings(org::GraphHost& a_host, Revisions& a_rv, std::atomic<std::uint32_t>& a_outstanding, std::uint32_t a_epoch,
			const std::shared_ptr<const NamedVersions>& a_versions, std::span<const std::shared_ptr<const RevisionFragment>> a_inputs, std::uint32_t a_hosted,
			const RevisionInputs& a_frame)
		{
			using R = Revisions;
			const std::uint32_t e = a_epoch;
			const auto& shapeFragment = a_inputs[1];
			// DCLF's buffers first, then the frame's versions: a frame revision's pending textures while one with DCLF epochs in its
			// closure is in preparation (plan step 10b). The versions fragment changes with them, so every epoch records again.
			std::shared_ptr<const org::IResourceVersions> composed;
			if (a_frame.frameVersions)
				composed = std::make_shared<const org::services::ComposedVersions>(
					std::initializer_list<std::shared_ptr<const org::IResourceVersions>>{ a_versions, a_frame.frameVersions });
			std::vector<std::shared_ptr<const EpochRevisionData>> requests;
			auto request = [&](auto&& a_fill) {
				auto data = std::make_shared<EpochRevisionData>();
				data->versions = a_versions;
				data->composed = composed;
				a_fill(data->shapes);
				requests.push_back(std::move(data));
			};
			const auto& traits = EpochTraitsOf(e);
			if (traits.shape == ShapeKind::Main) {
				// One set of variants per subset of hosted views that are absent, so a frame without that view records nothing for it.
				// The layout must match RevisionRecordings::IndexOf.
				std::array<std::pair<std::size_t, std::shared_ptr<const ViewFrame>>, kMaxEpochs> hosted{};
				std::uint32_t dimensions = 0;
				for (std::uint32_t h = 0, input = 2; h < kMaxEpochs; ++h)
					if ((a_hosted >> h) & 1)
						hosted[dimensions++] = { EpochTraitsOf(h).index, a_inputs[input++]->Value<ViewFrame>() };
				for (std::uint32_t absent = 0; absent < (1u << dimensions); ++absent) {
					auto fill = [&](RevisionShapes& a_shapes, bool a_withoutIndexPool) {
						a_shapes.main[traits.index] = shapeFragment->Value<PassFrame>();
						a_shapes.depthWithoutIndexPool = a_withoutIndexPool;
						for (std::uint32_t d = 0; d < dimensions; ++d)
							if (!((absent >> d) & 1))
								a_shapes.views[hosted[d].first] = hosted[d].second;
					};
					request([&](RevisionShapes& a_shapes) { fill(a_shapes, false); });
					// A depth variant without the index pool dispatch, so a frame with no range to upload records no zero-group copy.
					if (traits.index == kDepthShape && traits.sharesWith == kNoEpoch)
						request([&](RevisionShapes& a_shapes) { fill(a_shapes, true); });
				}
			} else if (traits.shape == ShapeKind::View)
				request([&](RevisionShapes& a_shapes) { a_shapes.views[traits.index] = shapeFragment->Value<ViewFrame>(); });
			else
				for (const auto& variant : shapeFragment->Value<ShadowVariants>()->shapes)
					request([&](RevisionShapes& a_shapes) { a_shapes.shadow = variant; });
			auto work = std::make_shared<PendingRecordings>();
			std::vector<RevisionFragment::Requirement> requirements{ { R::kVersionsSlot, a_inputs[0] }, { R::kShapeSlot + e, shapeFragment } };
			for (std::uint32_t h = 0, input = 2; h < kMaxEpochs; ++h)
				if ((a_hosted >> h) & 1)
					requirements.push_back({ R::kShapeSlot + h, a_inputs[input++] });
			work->fragment = std::make_shared<RevisionFragment>(std::move(requirements));
			work->hosted = traits.shape == ShapeKind::Main ? a_hosted : 0u;
			work->recordings.resize(requests.size());
			work->remaining.store(static_cast<std::uint32_t>(requests.size()), std::memory_order_relaxed);
			work->buildGeneration = a_frame.epochBuilds[static_cast<std::size_t>(traits.segment)];
			work->recorded = &a_rv.epochs[e].recorded;
			work->failedCount = &a_rv.epochs[e].failed;
			work->droppedCount = &a_rv.epochs[e].dropped;
			work->shapes = &a_rv.epochs[e].shapes;
			work->logged = &a_rv.failuresLogged;
			work->outstanding = &a_outstanding;
			work->name = traits.name;
			const auto epoch = RenderGraphRuntime::EpochOf(traits.segment);
			bool refused = false;
			for (std::size_t i = 0; i < requests.size() && !refused; ++i) {
				a_outstanding.fetch_add(1, std::memory_order_acq_rel);
				// Recorded on the build the inputs name, the staged one while one is staged. It attaches with the snapshot (plan step 10a).
				refused = !a_host.RequestEpochRecording(
					epoch, requests[i],
					[work, i](std::shared_ptr<const org::GraphHost::EpochRecording> a_recording, std::exception_ptr a_error) {
						RecordingCompleted(work, i, std::move(a_recording), std::move(a_error));
					},
					org::RunRef::kNoCopy, work->buildGeneration);
				if (refused)
					a_outstanding.fetch_sub(1, std::memory_order_acq_rel);
			}
			if (refused)
				return nullptr;
			return work->fragment;
		}
	}

	void IndirectDraws::Impl::AssembleRevision(std::uint32_t a_frame)
	{
		DCLF_ZONE("CS.DCLF.AssembleRevision");
		using R = SceneRevisions;
		auto& rv = revisions;
		// Wait for a graph build we asked the frame for. Recording now would use the graph it is about to replace.
		if (producer.buildWanted && producer.inputs.buildGeneration < producer.buildWanted) {
			++rv.buildWaits;
			return;
		}
		producer.buildWanted = 0;
		auto draft = rv.assembler.Begin();
		// Keep the versions fragment while nothing changed, so every recording stays valid.
		// A graph build or new frame versions get a new fragment, which forces every epoch to record again.
		const std::uint64_t builds = producer.inputs.buildGeneration;
		const std::uint64_t frameVersions = producer.inputs.frameVersionsSerial;
		const auto& seal = rv.sealed;
		if (rv.versionsFragmentSet != seal.named || rv.graphBuilds != builds || rv.frameVersions != frameVersions) {
			rv.versionsFragment = rv.versionsFragmentSet == seal.named ? RevisionFragment::MakeReady(seal.named) : seal.fragment;
			rv.versionsFragmentSet = seal.named;
			rv.graphBuilds = builds;
			rv.frameVersions = frameVersions;
			++rv.versionSets;
		}
		if (draft.Get(R::kVersionsSlot) != rv.versionsFragment)
			draft.Set(R::kVersionsSlot, rv.versionsFragment);
		const auto& versionsFragment = draft.Get(R::kVersionsSlot);
		const auto versions = versionsFragment->Value<NamedVersions>();

		AssembleShapes(rv, draft, producer.made, producer.toggles);

		// Keep a recording while its inputs are exactly the draft's, otherwise request a new one.
		// A failed recording is not asked for again until its inputs change.
		auto* host = RenderGraphRuntime::Get().Host();
		const bool recordable = host && producer.inputs.asyncEpochs;
		std::vector<std::shared_ptr<const RevisionFragment>> inputs;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
			const std::uint32_t slot = R::kRecordingSlot + e;
			const auto& traits = EpochTraitsOf(e);
			const auto& shapeFragment = draft.Get(R::kShapeSlot + e);
			const auto current = draft.Get(slot);
			inputs.assign({ versionsFragment, shapeFragment });
			std::uint32_t hosted = 0;
			for (std::uint32_t h = 0; h < epochs; ++h)
				if ((traits.hostsMask >> h) & 1)
					if (const auto& hostedFragment = draft.Get(R::kShapeSlot + h)) {
						inputs.push_back(hostedFragment);
						hosted |= 1u << h;
					}
			auto requiresExactly = [&](const RevisionFragment& a_fragment) {
				const auto& requirements = a_fragment.Requirements();
				if (requirements.size() != inputs.size())
					return false;
				for (std::size_t i = 0; i < inputs.size(); ++i)
					if (requirements[i].fragment != inputs[i])
						return false;
				return true;
			};
			// With the CPU pipeline, epochs without CPU pipeline passes have nothing to record.
			if (!recordable || !shapeFragment || traits.hostedBy != kNoEpoch || (producer.inputs.cpuPipeline && !CpuPipelineRecords(e))) {
				draft.Set(slot, nullptr);
				continue;
			}
			if (current && requiresExactly(*current)) {
				if (current->GetState() != RevisionFragment::State::Failed)
					continue;
				draft.Set(slot, nullptr);
				rv.failedFor[e] = inputs;
				continue;
			}
			if (rv.failedFor[e] == inputs) {
				draft.Set(slot, nullptr);
				continue;
			}
			auto fragment = RequestRecordings(*host, rv, recordingsOutstanding, e, versions, inputs, hosted, producer.inputs);
			if (!fragment) {
				// Async epochs stopped. Anything already asked for still settles the fragment, which this revision does not name.
				++rv.epochs[e].refused;
				draft.Set(slot, nullptr);
				continue;
			}
			++rv.epochs[e].requested;
			draft.Set(slot, std::move(fragment));
		}

		try {
			(void)rv.assembler.Seal(std::move(draft));
			rv.revisionVersions = versions;
			rv.revisionLayout = rv.layout;
			++rv.seals;
		} catch (const std::exception& error) {
			if (rv.sealFailures++ < 4)
				logger::error("[DCLF] scene revision of frame {} not sealed: {}", a_frame, error.what());
		}
	}

	Draws::EpochRevision IndirectDraws::Impl::RevisionOf(std::uint32_t a_epoch)
	{
		using R = SceneRevisions;
		EpochRevision out;
		if (const auto& active = revisions.active) {
			out.shape = active->Fragment(R::kShapeSlot + a_epoch);
			out.recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + a_epoch);
			out.direct = Draws::CurrentGraphConditions().cpuPipeline;
		}
		if (!out) {
			if (revisions.unrevised[a_epoch]++ == 0)
				logger::warn("[DCLF] the {} epoch of frame {} has no shape or recording in the selected revision: not submitted <- UNREVISED", EpochTraitsOf(a_epoch).name,
					Scene::FrameView::Get().GetFrame());
			return {};
		}
		return out;
	}

	void IndirectDraws::Impl::SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index)
	{
		// Submitting outside the epoch's trigger means a hook is out of step with the schedule.
		if (const auto trigger = EpochTrigger(a_epoch); Extensions::ActiveTrigger() != trigger) [[unlikely]] {
			if (outsideTrigger[a_epoch]++ == 0)
				logger::warn("[DCLF] the {} epoch was submitted outside its trigger {} (active: {})", EpochTraitsOf(a_epoch).name, TriggerName(trigger),
					TriggerName(Extensions::ActiveTrigger()));
		}
		if (auto* host = RenderGraphRuntime::Get().Host(); host && a_index < a_recordings.recordings.size()) {
			host->UseEpochRecording(a_recordings.recordings[a_index]);
			++revisions.submitted[a_epoch];
			revisions.variants[a_epoch] += a_index != 0;
		}
	}

}
