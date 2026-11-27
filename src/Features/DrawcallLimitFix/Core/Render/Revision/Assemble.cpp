// Assembles a scene revision on the snapshot builder: the sealed versions, each epoch's shape, and the recordings requested from
// ORG's host thread. Also the render thread's lookup and submission of the selected revision.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"

#include "RenderGraph/Frame/EpochRecordings.h"

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

		using Revisions = ImplProducer::SceneRevisions;
		using Draft = org::async::RevisionAssembler::Draft;
		using RecordingCache = Revisions::RecordingCache;

		/** @brief Put each epoch's shape in a_draft, keeping the old fragment while the shape is the same. */
		void AssembleShapes(Revisions& a_rv, Draft& a_draft, const MadeShapes& a_shapes)
		{
			using R = Revisions;
			auto& draft = a_draft;
			auto setShape = [&]<class Value>(std::uint32_t a_epoch, std::shared_ptr<const Value> a_value) {
				draft.Set(R::kShapeSlot + a_epoch, RevisionFragment::MakeReady(std::move(a_value)));
				++a_rv.epochs[a_epoch].changed;
			};
			const auto& shapes = a_shapes;
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
				const auto& traits = EpochTraitsOf(e);
				const auto& current = draft.Get(R::kShapeSlot + e);
				bool changed = false;
				if (traits.sharesWith != kNoEpoch) {
					// Share the same pointer so this epoch's recording stays valid.
					const auto& source = draft.Get(R::kShapeSlot + traits.sharesWith);
					auto share = [&]<class Frame>() {
						const auto shared = source ? source->Value<Frame>() : nullptr;
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
					// A DropWhenNone view loses its shape when no new one is made. A KeepLast view keeps its last shape then.
					const bool dropWhenNone = Extensions::Views().views[traits.index].life == Api::ViewShapeLife::DropWhenNone;
					const auto& made = shapes.views[traits.index];
					const auto currentShape = current ? current->Value<ViewFrame>() : nullptr;
					if (made) {
						auto kept = Kept<ViewFrame>(currentShape, made, changed);
						if (changed)
							setShape(e, std::move(kept));
					} else if (currentShape && dropWhenNone) {
						draft.Set(R::kShapeSlot + e, nullptr);
					}
				}
			}
		}

		/**
		 * @brief Request an epoch's recordings from ORG's host thread, as a new entry of its recording cache for a_key.
		 * a_inputs is the versions, the shape, then each hosted shape. Returns the fragment they resolve, or null when async epochs
		 * were stopped. The fragment resolves once every recording is in, or fails on a drop.
		 */
		std::shared_ptr<RevisionFragment> RequestRecordings(org::GraphHost& a_host, Revisions& a_rv, std::atomic<std::uint32_t>& a_outstanding, std::uint32_t a_epoch,
			const std::shared_ptr<const NamedVersions>& a_versions, std::span<const std::shared_ptr<const RevisionFragment>> a_inputs, std::uint32_t a_hosted,
			const RevisionInputs& a_frame, Revisions::RecordingInputs a_key, std::uint64_t a_use)
		{
			using R = Revisions;
			const std::uint32_t e = a_epoch;
			const auto& shapeFragment = a_inputs[1];
			// DCLF's buffers first, then the frame's versions: a frame revision's pending textures while one with DCLF epochs in its
			// closure is in preparation. The versions fragment changes with them, so every epoch records again.
			std::shared_ptr<const org::IResourceVersions> composed;
			if (a_frame.frameVersions)
				composed = std::make_shared<const org::services::ComposedVersions>(
					std::initializer_list<std::shared_ptr<const org::IResourceVersions>>{ a_versions, a_frame.frameVersions });
			std::vector<std::shared_ptr<const org::IHostExecutionData>> requests;
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
			std::vector<RevisionFragment::Requirement> requirements{ { R::kVersionsSlot, a_inputs[0] }, { R::kShapeSlot + e, shapeFragment } };
			for (std::uint32_t h = 0, input = 2; h < kMaxEpochs; ++h)
				if ((a_hosted >> h) & 1)
					requirements.push_back({ R::kShapeSlot + h, a_inputs[input++] });
			auto fragment = std::make_shared<RevisionFragment>(std::move(requirements));
			const std::uint32_t hosted = traits.shape == ShapeKind::Main ? a_hosted : 0u;
			// Recorded on the build the inputs name, the staged one while one is staged. It attaches with the snapshot.
			// A recording made on any other does not belong to this revision.
			const std::uint64_t buildGeneration = a_frame.epochBuilds[static_cast<std::size_t>(traits.segment)];
			const auto epoch = RenderGraphRuntime::EpochOf(traits.segment);
			auto& cache = a_rv.recordingCaches[e];
			// The cache keeps the recordings of the builds the host has now, attached and staged, read here on the builder thread.
			auto entry = cache.Add(a_host, epoch, std::move(a_key), a_use, requests.size());
			entry->payload = fragment;
			auto& stats = a_rv.epochs[e];
			a_outstanding.fetch_add(1, std::memory_order_acq_rel);
			// Once every request settled, on ORG's host thread. A drop, or a recording on another graph build, is not an error. The
			// fragment still fails so nothing waits on it, and the cache keeps the drop until the inputs change. A real error already
			// stopped the game in the service.
			auto settled = [fragment, hosted, &stats, &a_outstanding](const RecordingCache::Status& a_status) {
				if (a_status.Ready()) {
					++stats.recorded;
					stats.shapes += a_status.recordings.size();
					auto recordings = std::make_shared<RevisionRecordings>();
					recordings->recordings = a_status.recordings;
					recordings->hosted = hosted;
					(void)fragment->Resolve(std::shared_ptr<const RevisionRecordings>(std::move(recordings)));
				} else {
					if (a_status.dropped)
						++stats.dropped;
					(void)fragment->Fail(std::make_exception_ptr(std::runtime_error(a_status.error)));
				}
				a_outstanding.fetch_sub(1, std::memory_order_acq_rel);
			};
			if (cache.Post(a_host, entry, epoch, std::move(requests), buildGeneration, org::RunRef::kNoCopy, fmt::format("DCLF's {} epoch", traits.name),
					std::move(settled)) == ::RenderGraph::Frame::EpochRecordings::Posting::Refused) {
				// Async epochs stopped. The entry goes, so the next make asks again. What was posted still settles the fragment,
				// which this revision does not name.
				cache.EraseIf([&](const RecordingCache::Entry& a_kept) { return &a_kept == entry.get(); });
				return nullptr;
			}
			return fragment;
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
			// A recording keyed on an older versions fragment is never looked up again: a fragment is never reused. Each held its
			// set's buffer versions at their old sizes, in every epoch's cache, including epochs that no longer record. The revisions
			// the frame still runs keep their own references.
			for (auto& cache : rv.recordingCaches)
				(void)cache.EraseIf([&](const RecordingCache::Entry& a_kept) {
					return a_kept.key.fragments.empty() || a_kept.key.fragments.front() != rv.versionsFragment;
				});
		}
		if (draft.Get(R::kVersionsSlot) != rv.versionsFragment)
			draft.Set(R::kVersionsSlot, rv.versionsFragment);
		const auto& versionsFragment = draft.Get(R::kVersionsSlot);
		const auto versions = versionsFragment->Value<NamedVersions>();

		AssembleShapes(rv, draft, producer.made);

		// Keep a recording while its inputs are exactly the draft's, otherwise request a new one (the epoch's recording cache).
		// A dropped recording is not asked for again until its inputs change.
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
			// With the CPU pipeline, epochs without CPU pipeline passes have nothing to record. Nor has a dormant epoch.
			if (!recordable || !shapeFragment || traits.hostedBy != kNoEpoch || traits.dormant || (producer.inputs.cpuPipeline && !CpuPipelineRecords(e))) {
				draft.Set(slot, nullptr);
				continue;
			}
			auto& cache = rv.recordingCaches[e];
			R::RecordingInputs key{ inputs };
			const auto kept = cache.Find(
				key, a_frame, [](const RecordingCache::Status&) { return false; }, [&] { return fmt::format("DCLF's {} epoch", traits.name); });
			if (kept) {
				if (cache.Read(*kept).dropped)
					draft.Set(slot, nullptr);
				else if (current != kept->payload)
					draft.Set(slot, kept->payload);
				continue;
			}
			auto fragment = RequestRecordings(*host, rv, recordingsOutstanding, e, versions, inputs, hosted, producer.inputs, std::move(key), a_frame);
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
			// The frame's conditions are the attached graphs', which the selected revision was recorded on.
			const auto& conditions = Draws::AttachedGraphConditions();
			out.direct = conditions.cpuPipeline;
		}
		if (!out) {
			if (revisions.unrevised[a_epoch]++ == 0)
				logger::warn("[DCLF] the {} epoch of frame {} has no shape or recording in the selected revision: not submitted <- UNREVISED", EpochTraitsOf(a_epoch).name,
					Scene::FrameView::Get().SceneStep());
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
